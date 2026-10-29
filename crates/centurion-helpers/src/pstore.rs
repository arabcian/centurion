//! pstore: the panic / oops / console records the kernel leaves behind across a crash
//! (/sys/fs/pstore: ramoops, EFI variables; systemd-pstore moves them to /var/lib/systemd/pstore).
//!
//! * `list` (read-only, health-reader): every record found, text scrubbed (kernel addresses) and
//!   clipped to its tail, newest first. Files are opened O_NOFOLLOW and must be regular files.
//! * `clear` (tune-helper, root, after the GUI asked the user): copies each record from
//!   /sys/fs/pstore to /var/lib/centurion/pstore/ (the *scrubbed* text: the archive is world-readable,
//!   like every other state file) and removes the pstore file only when its archive was written.
//!   Removing a file from the pstore file system is what frees the EFI variable behind it
//!   (OpenRC has no systemd-pstore, so efi-pstore records pile up in NVRAM). Nothing else is deleted
//!   except archive files beyond the newest `KEEP_ARCHIVE`; EFI variables are never touched directly.

use serde_json::{json, Value};
use std::io::Read;
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};

pub const PSTORE: &str = "/sys/fs/pstore";
pub const SYSTEMD: &str = "/var/lib/systemd/pstore";
pub const ARCHIVE: &str = "/var/lib/centurion/pstore";
const SOURCES: &[(&str, &str)] = &[("pstore", PSTORE), ("systemd", SYSTEMD), ("archive", ARCHIVE)];
const MAX_FILE: u64 = 1 << 20;
const MAX_TEXT: usize = 12 * 1024;
const MAX_ENTRIES: usize = 40;
const MAX_CANDIDATES: usize = 400;
const MAX_DEPTH: usize = 3;
const KEEP_ARCHIVE: usize = 30;

fn kind_of(name: &str) -> &'static str {
    let base = name.rsplit('/').next().unwrap_or(name);
    if base.starts_with("dmesg") { "dmesg" } else if base.starts_with("console") { "console" } else { "other" }
}

/// "<crash time>-<name>.txt": the archive keeps the crash time (the copy's own mtime is the clearing time).
pub fn archive_name(t: u64, src_name: &str) -> String {
    let clean: String = src_name.chars().map(|c| if c.is_ascii_alphanumeric() || c == '.' || c == '-' { c } else { '_' }).take(80).collect();
    format!("{t}-{}.txt", clean.trim_end_matches(".txt"))
}

pub fn archive_time(file_name: &str) -> Option<u64> {
    let (t, _) = file_name.split_once('-')?;
    t.parse().ok()
}

/// The last `max` bytes as text, cut at a line start; the kernel prints the cause of a panic last.
fn tail_text(bytes: &[u8], max: usize) -> String {
    if bytes.len() <= max { return String::from_utf8_lossy(bytes).into_owned(); }
    let cut = &bytes[bytes.len() - max..];
    let start = cut.iter().position(|b| *b == b'\n').map_or(0, |i| i + 1);
    format!("[… earlier lines left out]\n{}", String::from_utf8_lossy(&cut[start..]))
}

/// (mtime, size, content) of a regular file, never through a symlink.
fn read_file(p: &Path) -> Option<(u64, u64, Vec<u8>)> {
    let f = std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK).open(p).ok()?;
    let m = f.metadata().ok()?;
    if !m.is_file() { return None; }
    let mut buf = Vec::new();
    (&f).take(MAX_FILE).read_to_end(&mut buf).ok()?;
    let t = m.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs());
    Some((t, m.len(), buf))
}

/// Regular files below `dir` (relative name, path), a few levels deep (systemd-pstore nests one directory per record).
fn walk(dir: &Path, rel: &str, depth: usize, out: &mut Vec<(String, PathBuf)>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        if out.len() >= MAX_CANDIDATES { return; }
        let Ok(ft) = e.file_type() else { continue };  // does not follow symlinks
        let name = e.file_name().to_string_lossy().into_owned();
        let rel_name = if rel.is_empty() { name } else { format!("{rel}/{name}") };
        if ft.is_file() { out.push((rel_name, e.path())); }
        else if ft.is_dir() && depth < MAX_DEPTH { walk(&e.path(), &rel_name, depth + 1, out); }
    }
}

/// {"available": any source directory exists, "entries": [{"src", "name", "kind", "t", "size", "text"}]}, newest first.
pub fn list() -> Value { list_in(SOURCES) }

fn list_in(sources: &[(&str, &str)]) -> Value {
    let mut entries: Vec<Value> = Vec::new();
    let mut available = false;
    for (src, dir) in sources {
        if !Path::new(dir).is_dir() { continue; }
        available = true;
        let mut files = Vec::new();
        walk(Path::new(dir), "", 0, &mut files);
        for (name, path) in files {
            let Some((mtime, size, bytes)) = read_file(&path) else { continue };
            let t = if *src == "archive" { archive_time(&name).unwrap_or(mtime) } else { mtime };
            entries.push(json!({"src": src, "name": name, "kind": kind_of(&name), "t": t, "size": size,
                                "text": crate::health::scrub(&tail_text(&bytes, MAX_TEXT))}));
        }
    }
    entries.sort_by(|a, b| b["t"].as_u64().cmp(&a["t"].as_u64()));
    entries.truncate(MAX_ENTRIES);
    json!({"available": available, "entries": entries})
}

/// Archive, then remove, every record in /sys/fs/pstore. {"ok", "archived", "removed", "errors"?}.
pub fn clear() -> Value { clear_in(PSTORE, ARCHIVE, &mut |m| crate::wlog::log("pstore-clear", m)) }

fn clear_in(pstore: &str, archive: &str, log: &mut dyn FnMut(&str)) -> Value {
    let mut files = Vec::new();
    walk(Path::new(pstore), "", 0, &mut files);
    if files.is_empty() { return json!({"ok": true, "archived": 0, "removed": 0}); }
    if let Err(e) = crate::secure_dir(archive) { return json!({"ok": false, "error": e}); }
    let (mut archived, mut removed, mut errors) = (0u32, 0u32, Vec::<String>::new());
    for (name, path) in files {
        let Some((mtime, size, bytes)) = read_file(&path) else { errors.push(format!("{name}: cannot read")); continue };
        // read_file stops at MAX_FILE: a clipped copy is not an archive of the record, so it stays in place.
        if (bytes.len() as u64) < size { errors.push(format!("{name}: larger than {} MiB, left in place", MAX_FILE >> 20)); continue; }
        // The whole record (not the clipped tail), scrubbed: the archive is world-readable.
        let text = crate::health::scrub(&String::from_utf8_lossy(&bytes));
        let dest = format!("{archive}/{}", archive_name(mtime, &name));
        if let Err(e) = crate::write_root_file(&dest, text.as_bytes()) { errors.push(e); continue; }  // never remove what is not archived
        archived += 1;
        match std::fs::remove_file(&path) {
            Ok(()) => { removed += 1; log(&format!("archived {name} to {dest}, removed it from {pstore}")); }
            Err(e) => errors.push(format!("{name}: {e}")),
        }
    }
    prune_archive(archive, log);
    let mut out = json!({"ok": errors.is_empty(), "archived": archived, "removed": removed});
    if !errors.is_empty() { out["error"] = json!(errors.join("; ")); }
    out
}

/// Only files this module wrote ("<digits>-….txt"), oldest crash time first out beyond KEEP_ARCHIVE.
fn prune_archive(archive: &str, log: &mut dyn FnMut(&str)) {
    let mut mine: Vec<(u64, PathBuf)> = std::fs::read_dir(archive).into_iter().flatten().flatten()
        .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
        .filter_map(|e| { let n = e.file_name().to_string_lossy().into_owned(); (n.ends_with(".txt")).then(|| archive_time(&n)).flatten().map(|t| (t, e.path())) })
        .collect();
    mine.sort_by_key(|m| std::cmp::Reverse(m.0));
    for (_, p) in mine.into_iter().skip(KEEP_ARCHIVE) {
        if std::fs::remove_file(&p).is_ok() { log(&format!("removed old archive {}", p.display())); }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("centurion-pstore-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn names() {
        assert_eq!(kind_of("dmesg-efi-171234567890123"), "dmesg");
        assert_eq!(kind_of("171/001/dmesg.txt"), "dmesg");
        assert_eq!(kind_of("console-ramoops-0"), "console");
        assert_eq!(kind_of("pmsg-ramoops-0"), "other");
        assert_eq!(archive_name(1700000000, "dmesg-efi-1"), "1700000000-dmesg-efi-1.txt");
        assert_eq!(archive_name(5, "a/b c/dmesg.txt"), "5-a_b_c_dmesg.txt");
        assert_eq!(archive_time("1700000000-dmesg-efi-1.txt"), Some(1700000000));
        assert_eq!(archive_time("notes.txt"), None);
        assert_eq!(archive_time("x-1.txt"), None);
    }

    #[test]
    fn tail_keeps_the_end() {
        assert_eq!(tail_text(b"short", 100), "short");
        let body: String = (0..1000).map(|i| format!("line {i}\n")).collect();
        let t = tail_text(body.as_bytes(), 100);
        assert!(t.starts_with("[… earlier lines left out]\n"));
        assert!(t.trim_end().ends_with("line 999"));
        assert!(t.len() < 160);
    }

    #[test]
    fn list_scrubs_and_orders_and_skips_symlinks() {
        let d = tmp("list");
        std::fs::write(d.join("dmesg-efi-1"), "Kernel panic at ffff8881234567ab\nRIP: 0010:foo+0x1/0x2\n").unwrap();
        std::os::unix::fs::symlink("/etc/hostname", d.join("dmesg-efi-2")).unwrap();
        std::fs::create_dir_all(d.join("sub/001")).unwrap();
        std::fs::write(d.join("sub/001/dmesg.txt"), "oops\n").unwrap();
        let dir = d.to_str().unwrap();
        let v = list_in(&[("pstore", dir), ("systemd", "/nonexistent/centurion-pstore")]);
        assert_eq!(v["available"], true);
        let e = v["entries"].as_array().unwrap();
        assert_eq!(e.len(), 2, "{v}");
        let a = e.iter().find(|x| x["name"] == "dmesg-efi-1").unwrap();
        assert_eq!(a["kind"], "dmesg");
        assert!(a["text"].as_str().unwrap().contains("<addr>") && !a["text"].as_str().unwrap().contains("ffff8881"));
        assert!(e.iter().any(|x| x["name"] == "sub/001/dmesg.txt"));
        assert!(e.iter().all(|x| x["name"] != "dmesg-efi-2"));
        let none = list_in(&[("pstore", "/nonexistent/centurion-pstore")]);
        assert_eq!(none["available"], false);
        assert_eq!(none["entries"].as_array().unwrap().len(), 0);
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn clear_archives_before_it_removes() {
        let (p, a) = (tmp("clr-p"), tmp("clr-a"));
        std::fs::write(p.join("dmesg-efi-1"), "panic ffff8881234567ab\n").unwrap();
        std::fs::write(p.join("dmesg-efi-2"), "second\n").unwrap();
        let (ps, ars) = (p.to_str().unwrap().to_owned(), a.to_str().unwrap().to_owned());
        // secure_dir wants a root-owned directory: unprivileged test runs get the refusal and must remove nothing.
        let mut logged = Vec::<String>::new();
        let r = clear_in(&ps, &ars, &mut |m| logged.push(m.to_owned()));
        if unsafe { libc::geteuid() } == 0 {
            assert_eq!(r["ok"], true, "{r}");
            assert_eq!((r["archived"].as_u64(), r["removed"].as_u64()), (Some(2), Some(2)));
            assert_eq!(logged.len(), 2);
            assert!(!p.join("dmesg-efi-1").exists());
            let names: Vec<String> = std::fs::read_dir(&a).unwrap().flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
            assert_eq!(names.len(), 2);
            let first = names.iter().find(|n| n.contains("dmesg-efi-1")).unwrap();
            let body = std::fs::read_to_string(a.join(first)).unwrap();
            assert!(body.contains("<addr>") && !body.contains("ffff8881"));
        } else {
            assert_eq!(r["ok"], false, "{r}");
            assert!(p.join("dmesg-efi-1").exists() && p.join("dmesg-efi-2").exists(), "nothing may be removed without an archive");
            assert!(logged.is_empty());
        }
        // Nothing to do is not an error and does not need the archive directory.
        let empty = tmp("clr-e");
        assert_eq!(clear_in(empty.to_str().unwrap(), "/nonexistent/centurion-archive", &mut |_| {})["ok"], true);
        for d in [&p, &a, &empty] { let _ = std::fs::remove_dir_all(d); }
    }

    #[test]
    fn a_record_too_large_to_archive_whole_stays() {
        if unsafe { libc::geteuid() } != 0 { return; }  // secure_dir needs root
        let (p, a) = (tmp("big-p"), tmp("big-a"));
        std::fs::write(p.join("dmesg-ramoops-0"), vec![b'x'; MAX_FILE as usize + 1]).unwrap();
        let r = clear_in(p.to_str().unwrap(), a.to_str().unwrap(), &mut |_| {});
        assert_eq!((r["ok"].as_bool(), r["removed"].as_u64()), (Some(false), Some(0)), "{r}");
        assert!(p.join("dmesg-ramoops-0").exists());
        assert_eq!(std::fs::read_dir(&a).unwrap().count(), 0);
        for d in [&p, &a] { let _ = std::fs::remove_dir_all(d); }
    }

    #[test]
    fn archive_is_pruned_to_the_newest() {
        let a = tmp("prune");
        for t in 1..=(KEEP_ARCHIVE as u64 + 5) { std::fs::write(a.join(archive_name(1000 + t, "dmesg-efi-x")), "x").unwrap(); }
        std::fs::write(a.join("README.txt"), "not ours").unwrap();
        let mut n = 0;
        prune_archive(a.to_str().unwrap(), &mut |_| n += 1);
        assert_eq!(n, 5);
        let left = std::fs::read_dir(&a).unwrap().count();
        assert_eq!(left, KEEP_ARCHIVE + 1);
        assert!(a.join("README.txt").exists());
        assert!(!a.join(archive_name(1001, "dmesg-efi-x")).exists());
        let _ = std::fs::remove_dir_all(&a);
    }
}
