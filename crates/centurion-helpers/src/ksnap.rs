//! ksnap — bounded, read-only dump of kernel tunables under /sys, debugfs included.
//!
//! debugfs (/sys/kernel/debug) is root-only, so the GUI cannot read most scheduler files itself.
//! `health-reader` runs as root under a silent, read-only polkit action and answers
//! `{"op":"ksnap","roots":["/sys/kernel/debug/sched", ...]}` with every readable text file below the
//! roots (default: all of /sys), each as `{"p": path, "v": value, "w": writable}`. Nothing is written
//! and nothing path-like is acted on except as a place to look: the request can only choose where
//! to read, and what may be read is fixed here.
//!
//! What is never read, whatever the request says:
//!   * symlinks (the walk never leaves the tree it was pointed at and cannot loop), devices, pipes;
//!   * write-only files (they are triggers, not values) and anything that is not UTF-8 text;
//!   * debugfs outside [`DEBUGFS_ALLOW`]: a root-only read of some debugfs files has side effects
//!     (amdgpu_gpu_recover resets the GPU, regmap and i2c read hardware registers, tracing/trace_pipe
//!     blocks), so debugfs is opened up subtree by subtree, never as a whole;
//!   * the subtrees in [`DENY_PREFIXES`] and the file names in [`DENY_NAMES`] (serial numbers, kernel
//!     addresses, crash dumps, process lists).
//! Values are passed through `health::scrub` (kernel addresses become `<addr>`).
//!
//! Bounds: 4 KiB per value, [`MAX_ENTRIES`] entries, [`MAX_BYTES`] bytes of values, [`MAX_DEPTH`] levels
//! and [`BUDGET`] of wall time; hitting one ends the walk with `"truncated": true` and the reason.

use serde_json::{json, Value};
use std::io::Read;
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub const MAX_VALUE: usize = 4096;
pub const MAX_ENTRIES: usize = 60_000;
pub const MAX_BYTES: usize = 12 << 20;
pub const MAX_DEPTH: usize = 14;
pub const BUDGET: Duration = Duration::from_secs(8);
const MAX_ROOTS: usize = 16;

/// Subtrees of /sys/kernel/debug that may be read (everything else in debugfs is refused).
pub const DEBUGFS_ALLOW: &[&str] = &["sched", "x86", "pm_qos", "extfrag"];

/// Paths relative to /sys that are never read (and never entered).
pub const DENY_PREFIXES: &[&str] = &[
    "kernel/security", "kernel/tracing", "kernel/slab", "kernel/config", "kernel/btf", "kernel/vmcoreinfo", "kernel/notes",
    "fs/cgroup", "fs/bpf", "fs/pstore", "fs/selinux", "firmware/efi/efivars", "firmware/acpi/tables", "firmware/dmi",
];

/// File or directory names that are never read, anywhere (identifiers of the machine, kernel addresses, noise).
pub const DENY_NAMES: &[&str] = &[
    "product_serial", "product_uuid", "board_serial", "chassis_serial", "chassis_asset_tag", "board_asset_tag",
    "serial", "serial_number", "sections", "notes", "uevent", "modalias",
];

pub struct Limits { pub entries: usize, pub bytes: usize, pub budget: Duration }

impl Default for Limits {
    fn default() -> Self { Limits { entries: MAX_ENTRIES, bytes: MAX_BYTES, budget: BUDGET } }
}

#[derive(Default)]
struct Out {
    entries: Vec<Value>,
    bytes: usize,
    binary: usize,
    unreadable: usize,
    denied: usize,
    truncated: Option<&'static str>,
}

/// True if `p` (inside `sys`) must not be read or entered.
fn denied(sys: &Path, p: &Path) -> bool {
    let Ok(rel) = p.strip_prefix(sys) else { return true };
    let comps: Vec<&str> = rel.components().filter_map(|c| c.as_os_str().to_str()).collect();
    if comps.len() != rel.components().count() { return true; }  // not UTF-8
    if comps.iter().any(|c| DENY_NAMES.contains(c)) { return true; }
    let rel_s = comps.join("/");
    if DENY_PREFIXES.iter().any(|d| rel_s.strip_prefix(d).is_some_and(|r| r.is_empty() || r.starts_with('/'))) { return true; }
    // /sys/kernel/debug itself may be listed; below it only the allowlisted subtrees.
    if comps.len() >= 3 && comps[0] == "kernel" && comps[1] == "debug" && !DEBUGFS_ALLOW.contains(&comps[2]) { return true; }
    false
}

/// The text of one regular file, or None. Opened non-blocking and without following links, so a file
/// that would wait (a pipe-like debugfs entry) fails at once instead of stalling the helper.
fn read_text(p: &Path) -> Result<Option<String>, ()> {
    let mut f = std::fs::OpenOptions::new().read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC | libc::O_NOCTTY)
        .open(p).map_err(|_| ())?;
    let mut buf = Vec::with_capacity(256);
    (&mut f).take(MAX_VALUE as u64 + 1).read_to_end(&mut buf).map_err(|_| ())?;
    if buf.len() > MAX_VALUE { buf.truncate(MAX_VALUE); }
    if buf.contains(&0) { return Ok(None); }
    match String::from_utf8(buf) {
        Ok(s) => Ok(Some(s)),
        // A multi-byte character cut by the size limit is text, not binary: retry without the cut tail.
        Err(e) if e.utf8_error().error_len().is_none() => {
            let valid = e.utf8_error().valid_up_to();
            Ok(Some(String::from_utf8_lossy(&e.into_bytes()[..valid]).into_owned()))
        }
        Err(_) => Ok(None),
    }
}

fn add_file(sys: &Path, p: &Path, mode: u32, out: &mut Out, lim: &Limits) {
    if mode & 0o444 == 0 { return; }  // write-only trigger
    if denied(sys, p) { out.denied += 1; return; }
    match read_text(p) {
        Err(()) => out.unreadable += 1,
        Ok(None) => out.binary += 1,
        Ok(Some(v)) => {
            let v = crate::health::scrub(v.trim_end_matches('\n'));
            out.bytes += v.len() + p.as_os_str().len();
            out.entries.push(json!({"p": p.to_string_lossy(), "v": v, "w": mode & 0o222 != 0}));
            if out.entries.len() >= lim.entries { out.truncated = Some("entries"); }
            else if out.bytes >= lim.bytes { out.truncated = Some("bytes"); }
        }
    }
}

fn walk(sys: &Path, dir: &Path, depth: usize, started: Instant, out: &mut Out, lim: &Limits) {
    if out.truncated.is_some() { return; }
    if started.elapsed() > lim.budget { out.truncated = Some("time"); return; }
    if depth > MAX_DEPTH { return; }
    let Ok(rd) = std::fs::read_dir(dir) else { out.unreadable += 1; return };
    let mut kids: Vec<(PathBuf, std::fs::FileType)> = rd.flatten().filter_map(|e| Some((e.path(), e.file_type().ok()?))).collect();
    kids.sort_by(|a, b| a.0.cmp(&b.0));
    for (p, ft) in kids {
        if out.truncated.is_some() { return; }
        if ft.is_symlink() { continue; }
        if denied(sys, &p) { out.denied += 1; continue; }
        if ft.is_dir() { walk(sys, &p, depth + 1, started, out, lim); }
        else if ft.is_file() {
            if started.elapsed() > lim.budget { out.truncated = Some("time"); return; }
            if let Ok(md) = std::fs::symlink_metadata(&p) { add_file(sys, &p, md.mode(), out, lim); }
        }
    }
}

/// Walks `roots` (each inside `sys`) and returns the reply object. `roots` must already be canonical.
pub fn scan_in(sys: &Path, roots: &[PathBuf], lim: &Limits) -> Value {
    let started = Instant::now();
    let mut out = Out::default();
    for r in roots {
        if out.truncated.is_some() { break; }
        if denied(sys, r) && r != sys { out.denied += 1; continue; }
        let Ok(md) = std::fs::symlink_metadata(r) else { out.unreadable += 1; continue };
        if md.is_dir() { walk(sys, r, 0, started, &mut out, lim); }
        else if md.is_file() { add_file(sys, r, md.mode(), &mut out, lim); }
    }
    json!({
        "ok": true, "entries": out.entries, "count": out.entries.len(),
        "truncated": out.truncated.is_some(), "reason": out.truncated,
        "skipped": {"binary": out.binary, "unreadable": out.unreadable, "denied": out.denied},
        "debugfs_mounted": crate::tune::debugfs_mounted(), "elapsed_ms": started.elapsed().as_millis() as u64,
    })
}

/// `{"roots":[abs path, ..]}` (optional; default ["/sys"]). A root that is not under /sys after
/// resolving links, or that is a denied path, is an error for the whole request.
pub fn scan(req: &Value) -> Value {
    let sys = Path::new("/sys");
    let mut roots: Vec<PathBuf> = Vec::new();
    match req.get("roots") {
        None | Some(Value::Null) => roots.push(sys.to_path_buf()),
        Some(Value::Array(a)) if !a.is_empty() && a.len() <= MAX_ROOTS => {
            for r in a {
                let Some(s) = r.as_str().filter(|s| s.len() <= 256 && s.starts_with('/')) else {
                    return json!({"ok": false, "error": "ksnap: roots must be absolute paths"});
                };
                if Path::new(s).components().any(|c| matches!(c, std::path::Component::ParentDir)) {
                    return json!({"ok": false, "error": format!("ksnap: {s}: no '..' allowed")});
                }
                let Some(real) = crate::canonical_in_sysfs(Path::new(s)).or_else(|| (s.trim_end_matches('/') == "/sys").then(|| sys.to_path_buf())) else {
                    return json!({"ok": false, "error": format!("ksnap: {s}: does not exist or is not under /sys")});
                };
                if real != sys && denied(sys, &real) {
                    return json!({"ok": false, "error": format!("ksnap: {s}: not readable through ksnap")});
                }
                roots.push(real);
            }
        }
        _ => return json!({"ok": false, "error": "ksnap: roots must be a list of 1-16 paths"}),
    }
    let mut lim = Limits::default();
    if let Some(n) = req.get("max_entries").and_then(Value::as_u64) { lim.entries = (n as usize).clamp(1, MAX_ENTRIES); }
    scan_in(sys, &roots, &lim)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    struct Tmp(PathBuf);
    impl Tmp {
        fn new(tag: &str) -> Tmp {
            let p = std::env::temp_dir().join(format!("ksnap-{tag}-{}-{}", std::process::id(),
                std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()));
            std::fs::create_dir_all(&p).unwrap();
            Tmp(p)
        }
        fn file(&self, rel: &str, body: &[u8], mode: u32) {
            let p = self.0.join(rel);
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(&p, body).unwrap();
            std::fs::set_permissions(&p, std::fs::Permissions::from_mode(mode)).unwrap();
        }
    }
    impl Drop for Tmp { fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.0); } }

    fn paths(v: &Value, sys: &Path) -> Vec<String> {
        v["entries"].as_array().unwrap().iter()
            .map(|e| Path::new(e["p"].as_str().unwrap()).strip_prefix(sys).unwrap().to_string_lossy().into_owned()).collect()
    }

    #[test]
    fn reads_values_and_marks_writable() {
        let t = Tmp::new("rw");
        t.file("kernel/mm/a", b"7\n", 0o644);
        t.file("kernel/mm/b", b"x y\n", 0o444);
        let r = scan_in(&t.0, std::slice::from_ref(&t.0), &Limits::default());
        assert_eq!(paths(&r, &t.0), ["kernel/mm/a", "kernel/mm/b"]);
        let e = r["entries"].as_array().unwrap();
        assert_eq!((e[0]["v"].as_str(), e[0]["w"].as_bool()), (Some("7"), Some(true)));
        assert_eq!((e[1]["v"].as_str(), e[1]["w"].as_bool()), (Some("x y"), Some(false)));
    }

    #[test]
    fn skips_write_only_binary_symlinks_and_denied_names() {
        let t = Tmp::new("skip");
        t.file("d/trigger", b"", 0o200);
        t.file("d/blob", &[0x7f, b'E', 0, 1, 2], 0o444);
        t.file("d/latin", &[0xff, 0xfe, b'a'], 0o444);
        t.file("class/dmi/id/product_serial", b"SN123\n", 0o444);
        t.file("class/dmi/id/bios_version", b"N1\n", 0o444);
        t.file("d/uevent", b"DEVTYPE=x\n", 0o444);
        t.file("module/m/sections/.text", b"0xffffffffc0000000\n", 0o444);
        std::os::unix::fs::symlink(t.0.join("d"), t.0.join("d/loop")).unwrap();
        let r = scan_in(&t.0, std::slice::from_ref(&t.0), &Limits::default());
        assert_eq!(paths(&r, &t.0), ["class/dmi/id/bios_version"]);
        assert_eq!(r["skipped"]["binary"], 2);
    }

    #[test]
    fn debugfs_only_in_allowlisted_subtrees() {
        let t = Tmp::new("dbg");
        t.file("kernel/debug/sched/base_slice_ns", b"1600000\n", 0o644);
        t.file("kernel/debug/x86/sched_itmt_enabled", b"1\n", 0o644);
        t.file("kernel/debug/dri/0/amdgpu_gpu_recover", b"0\n", 0o444);
        t.file("kernel/debug/tracing/trace", b"x\n", 0o444);
        t.file("kernel/security/lsm", b"x\n", 0o444);
        t.file("fs/cgroup/cgroup.procs", b"1\n", 0o444);
        let all = scan_in(&t.0, std::slice::from_ref(&t.0), &Limits::default());
        assert_eq!(paths(&all, &t.0), ["kernel/debug/sched/base_slice_ns", "kernel/debug/x86/sched_itmt_enabled"]);
        // Asking for a refused subtree by name reads nothing from it.
        let d = scan_in(&t.0, &[t.0.join("kernel/debug/dri")], &Limits::default());
        assert!(paths(&d, &t.0).is_empty());
        // A single allowed file as the root.
        let one = scan_in(&t.0, &[t.0.join("kernel/debug/sched/base_slice_ns")], &Limits::default());
        assert_eq!(paths(&one, &t.0), ["kernel/debug/sched/base_slice_ns"]);
    }

    #[test]
    fn scrubs_kernel_addresses() {
        let t = Tmp::new("scrub");
        t.file("kernel/x", b"ptr ffffffffc0123456 ok 1600000\n", 0o444);
        let r = scan_in(&t.0, std::slice::from_ref(&t.0), &Limits::default());
        assert_eq!(r["entries"][0]["v"], "ptr <addr> ok 1600000");
    }

    #[test]
    fn bounds_end_the_walk_with_a_reason() {
        let t = Tmp::new("lim");
        for i in 0..10 { t.file(&format!("k/f{i}"), b"1\n", 0o644); }
        let r = scan_in(&t.0, std::slice::from_ref(&t.0), &Limits { entries: 4, ..Limits::default() });
        assert_eq!((r["count"].as_u64(), r["truncated"].as_bool(), r["reason"].as_str()), (Some(4), Some(true), Some("entries")));
        let r = scan_in(&t.0, std::slice::from_ref(&t.0), &Limits { budget: Duration::ZERO, ..Limits::default() });
        assert_eq!(r["reason"], "time");
    }

    #[test]
    fn a_long_value_is_cut_not_dropped() {
        let t = Tmp::new("long");
        t.file("k/even", "é".repeat(5000).as_bytes(), 0o444);  // the 4096-byte cut falls between two characters
        t.file("k/odd", format!("a{}", "é".repeat(3000)).as_bytes(), 0o444);  // the cut lands inside a character
        let r = scan_in(&t.0, std::slice::from_ref(&t.0), &Limits::default());
        assert_eq!(r["count"], 2);
        assert_eq!(r["skipped"]["binary"], 0);
    }

    #[test]
    fn request_validation() {
        assert_eq!(scan(&json!({"roots": ["/etc"]}))["ok"], false);
        assert_eq!(scan(&json!({"roots": ["/sys/../etc"]}))["ok"], false);
        assert_eq!(scan(&json!({"roots": "/sys"}))["ok"], false);
        assert_eq!(scan(&json!({"roots": []}))["ok"], false);
        assert_eq!(scan(&json!({"roots": ["relative"]}))["ok"], false);
        assert_eq!(scan(&json!({"roots": ["/sys/kernel/debug/dri"]}))["ok"], false);
        assert_eq!(scan(&json!({"roots": ["/sys/fs/cgroup"]}))["ok"], false);
    }
}
