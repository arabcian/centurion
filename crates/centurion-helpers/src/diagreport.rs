//! Health → Overview → Create diagnostic report: one .tar.gz for a bug report, built from what the
//! Health pages already keep (classified kernel faults, kernel-log tails, sleeps, cooling, battery,
//! boot record, dGPU usage, the write log) plus a few lines about the machine. Runs as the user;
//! nothing here needs root, and an unreadable file is listed as such instead of failing the report.
//!
//! Every text passes `sanitize` before it is kept: kernel addresses (health::scrub), the value of any
//! line whose key names a serial number, UUID or asset tag (the rule of the GUI's SERIAL_RE: "Serial
//! Number: X", "UUID=X"), MAC addresses and IPv4 addresses. Placeholders that identify nothing
//! ("Not Specified", "None", "0"…) are left as they are.
//!
//! stdin:  {"op":"diag_report"}                        preview: each file's status and text
//!         {"op":"diag_report","out":"/abs/x.tar.gz"}  writes the archive
//!         optional: "version" (the GUI's version string), "dgpu": bool (NVIDIA status when present)

use crate::health;
use crate::overview;
use serde_json::{json, Value};
use std::io::Write;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::Command;

/// Tail kept of each kernel/health log.
const LOG_TAIL: u64 = 512 << 10;
/// Whole-file cap for the JSON records (a larger file is left out).
const JSON_MAX: u64 = 4 << 20;
/// Lines kept of the sleep and write logs.
const LINES: usize = 200;
/// Text shown per file in the preview (the archive keeps everything).
const PREVIEW_BYTES: usize = 64 << 10;

/// Where the inputs live. The system paths are the real ones; tests point them at temp folders.
pub struct Sources {
    pub state: PathBuf,
    pub log_dir: PathBuf,
    pub lib_dir: PathBuf,
    pub dmi: PathBuf,
    pub release: PathBuf,
    pub nvidia_version: PathBuf,
}

impl Sources {
    pub fn system() -> Sources {
        Sources {
            state: overview::state_dir(),
            log_dir: PathBuf::from("/var/log/centurion"),
            lib_dir: PathBuf::from("/var/lib/centurion"),
            dmi: PathBuf::from("/sys/class/dmi/id"),
            release: PathBuf::from("/proc/sys/kernel/osrelease"),
            nvidia_version: PathBuf::from("/sys/module/nvidia/version"),
        }
    }
}

pub struct Entry {
    pub name: String,
    /// "included", "not present", "unreadable" or "too large".
    pub status: &'static str,
    pub text: String,
    pub masked: u64,
}

// ── sanitizing ───────────────────────────────────────────────────────────────

const PLACEHOLDERS: [&str; 10] = ["Not Specified", "Not Present", "None", "Unknown", "N/A", "Not Available",
                                  "To Be Filled By O.E.M.", "Default string", "Default String", "0"];

fn is_placeholder(v: &str) -> bool {
    v.is_empty() || v == "00000000" || PLACEHOLDERS.contains(&v) || v.starts_with("Not ") || v.starts_with("To Be Filled")
}

const IDENTITY_KEYS: [&str; 3] = ["serial", "uuid", "asset tag"];

/// "Serial Number: X", "UUID=X", "POWER_SUPPLY_SERIAL_NUMBER=X", "…  uuid: X": the first word after the
/// ':' or '=' that follows a serial/UUID/asset-tag key becomes <hidden>. Placeholders stay visible.
/// The key may sit anywhere in the line, so a timestamp's colons do not hide it.
fn mask_serials(text: &str) -> (String, u64) {
    let mut out = String::with_capacity(text.len());
    let mut n = 0;
    for line in text.split_inclusive('\n') {
        // Lowercasing ASCII keeps every byte offset, so offsets found here index `line` too.
        let low = line.to_ascii_lowercase();
        let mut spans: Vec<(usize, usize)> = Vec::new();
        for key in IDENTITY_KEYS {
            let mut from = 0;
            while let Some(p) = low[from..].find(key) {
                let after = from + p + key.len();
                from = after;
                let Some(sep) = low[after..].find([':', '=']).map(|i| after + i) else { break };
                // The value ends at a space or at JSON / list punctuation, so a log record stays well formed.
                let rest = line[sep + 1..].trim_start_matches(|c: char| c.is_whitespace() || c == '"' || c == '\'');
                let token_len = rest.find(|c: char| c.is_whitespace() || "\"',;}])|".contains(c)).unwrap_or(rest.len());
                let token = &rest[..token_len];
                let placeholder = is_placeholder(token) || is_placeholder(rest.trim_end())
                    || rest.starts_with("Not ") || rest.starts_with("To Be Filled") || rest.starts_with("Default ");
                if token_len == 0 || placeholder { continue; }
                let start = line.len() - rest.len();
                spans.push((start, start + token_len));
            }
        }
        spans.sort_unstable();
        let mut pos = 0;
        for (s, e) in spans {
            if s < pos { continue; }
            out.push_str(&line[pos..s]);
            out.push_str("<hidden>");
            pos = e;
            n += 1;
        }
        out.push_str(&line[pos..]);
    }
    (out, n)
}

fn is_mac(s: &[u8]) -> bool {
    let sep = s[2];
    (sep == b':' || sep == b'-') && s.iter().enumerate().all(|(k, &c)| if k % 3 == 2 { c == sep } else { c.is_ascii_hexdigit() })
}

/// "00:11:22:33:44:55" or "00-11-22-33-44-55" → <mac>.
fn mask_macs(text: &str) -> (String, u64) {
    let b = text.as_bytes();
    let word = |c: u8| c.is_ascii_alphanumeric() || c == b':' || c == b'-';
    let mut out = String::with_capacity(text.len());
    let (mut i, mut n) = (0, 0);
    while i < b.len() {
        if i + 17 <= b.len() && is_mac(&b[i..i + 17]) && (i == 0 || !word(b[i - 1])) && b.get(i + 17).map_or(true, |&c| !word(c)) {
            out.push_str("<mac>");
            i += 17;
            n += 1;
        } else {
            let ch = text[i..].chars().next().unwrap_or('\0');
            out.push(ch);
            i += ch.len_utf8().max(1);
        }
    }
    (out, n)
}

/// Digits and dots of a dotted-quad starting at `start`; the index after it, or None.
fn ipv4_end(b: &[u8], start: usize) -> Option<usize> {
    let mut i = start;
    for group in 0..4 {
        let s = i;
        while i < b.len() && b[i].is_ascii_digit() && i - s < 3 { i += 1; }
        if i == s { return None; }
        let v: u32 = std::str::from_utf8(&b[s..i]).ok()?.parse().ok()?;
        if v > 255 { return None; }
        if group < 3 {
            if i + 1 < b.len() && b[i] == b'.' && b[i + 1].is_ascii_digit() { i += 1; } else { return None; }
        }
    }
    let more = i < b.len() && (b[i].is_ascii_alphanumeric() || (b[i] == b'.' && i + 1 < b.len() && b[i + 1].is_ascii_digit()));
    (!more).then_some(i)
}

/// 192.168.1.23 → <ip>. Three-part numbers such as kernel versions are left alone.
fn mask_ipv4(text: &str) -> (String, u64) {
    let b = text.as_bytes();
    let mut out = String::with_capacity(text.len());
    let (mut i, mut n) = (0, 0);
    while i < b.len() {
        if b[i].is_ascii_digit() && (i == 0 || !(b[i - 1].is_ascii_alphanumeric() || b[i - 1] == b'.')) {
            if let Some(end) = ipv4_end(b, i) {
                out.push_str("<ip>");
                i = end;
                n += 1;
                continue;
            }
        }
        let ch = text[i..].chars().next().unwrap_or('\0');
        out.push(ch);
        i += ch.len_utf8().max(1);
    }
    (out, n)
}

/// Everything a text goes through before it is kept, with the number of values masked.
pub fn sanitize(text: &str) -> (String, u64) {
    let scrubbed = health::scrub(text);
    let addr = scrubbed.matches("<addr>").count().saturating_sub(text.matches("<addr>").count()) as u64;
    let (a, serials) = mask_serials(&scrubbed);
    let (b, macs) = mask_macs(&a);
    let (c, ips) = mask_ipv4(&b);
    (c, addr + serials + macs + ips)
}

// ── collecting ───────────────────────────────────────────────────────────────

/// (status, text) of one file: the last `lines` lines, or the last `tail` bytes.
fn fetch(path: &Path, tail: u64, lines: Option<usize>) -> (&'static str, Option<String>) {
    match std::fs::metadata(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => ("not present", None),
        Err(_) => ("unreadable", None),
        Ok(m) if !m.is_file() => ("unreadable", None),
        Ok(_) => match overview::read_tail(path, tail) {
            None => ("unreadable", None),
            Some(t) => {
                let t = match lines {
                    Some(n) => { let v: Vec<&str> = t.lines().collect(); v[v.len().saturating_sub(n)..].join("\n") + "\n" }
                    None => t,
                };
                ("included", Some(t))
            }
        },
    }
}

fn json_file(path: &Path) -> (&'static str, Option<String>) {
    match std::fs::metadata(path) {
        Ok(m) if m.len() > JSON_MAX => ("too large", None),
        _ => fetch(path, JSON_MAX, None),
    }
}

fn entry(name: &str, status: &'static str, raw: Option<String>) -> Entry {
    let (text, masked) = raw.map(|r| sanitize(&r)).unwrap_or_default();
    Entry { name: name.to_owned(), status, text, masked }
}

fn read_trim(p: &Path) -> Option<String> {
    std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned()).filter(|s| !s.is_empty())
}

fn system_text(src: &Sources, version: &str, now: i64, offset: i64) -> String {
    let dmi = |f: &str| read_trim(&src.dmi.join(f)).unwrap_or_else(|| "—".to_owned());
    format!(
        "Centurion {version}\nkernel {}\nNVIDIA driver {}\nproduct {} / {}\nboard {}\nsys_vendor {}\nbios {}\ngenerated {} (local date)\n",
        read_trim(&src.release).unwrap_or_else(|| "—".to_owned()),
        read_trim(&src.nvidia_version).unwrap_or_else(|| "not loaded".to_owned()),
        dmi("product_name"), dmi("product_version"), dmi("board_name"), dmi("sys_vendor"), dmi("bios_version"),
        overview::ymd(now, offset),
    )
}

const README: &str = "Centurion diagnostic report
===========================
Made by Health -> Overview -> Create diagnostic report. Everything here was read as the user;
no administrator rights were used.

Files
  system.txt        Centurion, kernel and NVIDIA driver versions; product, board and BIOS names
  health.log(.1)    classified kernel faults (newest in health.log)
  kernel.log(.1)    kernel-log lines saved per boot (boot=... seq=...)
  sleep.log         the last 200 sleeps
  cooling.json      cooling runs and their baselines
  battery.json      battery capacity history and drain per scene
  dgpu-usage.json   dGPU awake time on battery and its last wake-ups
  dgpu.json         NVIDIA runtime power status (only with an NVIDIA GPU)
  exposure.json     boot record: how each boot ended
  writes.log        the last 200 writes Centurion made to sysfs / firmware

Replaced before saving
  serial numbers, UUIDs and asset tags   -> <hidden>
  MAC addresses                          -> <mac>
  IPv4 addresses                         -> <ip>
  kernel addresses                       -> <addr>
Look through the files before you attach them anywhere public.
";

/// Every entry of the report, sanitized. Missing or unreadable files are kept as entries with a status.
pub fn collect(src: &Sources, version: &str, dgpu: bool, now: i64, offset: i64) -> Vec<Entry> {
    let mut v = vec![
        entry("README.txt", "included", Some(README.to_owned())),
        entry("system.txt", "included", Some(system_text(src, version, now, offset))),
    ];
    for name in ["health.log.1", "health.log", "kernel.log.1", "kernel.log"] {
        let (status, raw) = fetch(&src.state.join(name), LOG_TAIL, None);
        v.push(entry(name, status, raw));
    }
    let (status, raw) = fetch(&src.log_dir.join("sleep.log"), u64::MAX / 2, Some(LINES));
    v.push(entry("sleep.log", status, raw));
    for (name, path) in [("cooling.json", src.state.join("cooling.json")), ("battery.json", src.state.join("battery.json")),
                         ("dgpu-usage.json", src.state.join("dgpu.json")), ("exposure.json", src.lib_dir.join("exposure.json"))] {
        let (status, raw) = json_file(&path);
        v.push(entry(name, status, raw));
    }
    let (status, raw) = fetch(&src.log_dir.join("writes.log"), u64::MAX / 2, Some(LINES));
    v.push(entry("writes.log", status, raw));
    if dgpu {
        let text = serde_json::to_string_pretty(&crate::dgpu::status()).ok();
        v.push(entry("dgpu.json", if text.is_some() { "included" } else { "unreadable" }, text));
    }
    v
}

// ── output ───────────────────────────────────────────────────────────────────

/// The file list and each file's text, cut to PREVIEW_BYTES on a character boundary.
fn preview(entries: &[Entry]) -> Value {
    let files: Vec<Value> = entries.iter().map(|e| {
        let cut = e.text.char_indices().map(|(i, _)| i).find(|&i| i >= PREVIEW_BYTES).unwrap_or(e.text.len());
        json!({"name": e.name, "status": e.status, "bytes": e.text.len(), "masked": e.masked, "text": &e.text[..cut]})
    }).collect();
    let masked: u64 = entries.iter().map(|e| e.masked).sum();
    json!({"ok": true, "files": files, "masked": masked})
}

/// Writes the included entries as <root>/<name> in a private staging folder, packs them with tar and
/// copies the archive to `out` (created 0600). Returns the archive size in bytes.
pub fn write_archive(entries: &[Entry], root: &str, out: &Path) -> Result<u64, String> {
    let name = out.file_name().and_then(|n| n.to_str()).unwrap_or("");
    if !out.is_absolute() || !name.ends_with(".tar.gz") {
        return Err("the report must be saved to an absolute path ending in .tar.gz".to_owned());
    }
    let parent = out.parent().ok_or("bad output path")?;
    if !parent.is_dir() { return Err(format!("{}: no such folder", parent.display())); }
    let tar = ["/usr/bin/tar", "/bin/tar"].iter().map(Path::new).find(|p| p.is_file()).ok_or("tar is not installed")?;

    let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_nanos());
    let stage = std::env::temp_dir().join(format!("centurion-report-{}-{nanos}", std::process::id()));
    let dir = stage.join(root);
    std::fs::DirBuilder::new().recursive(true).mode(0o700).create(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let result = pack(entries, root, tar, &stage, &dir, out);
    let _ = std::fs::remove_dir_all(&stage);
    result
}

fn pack(entries: &[Entry], root: &str, tar: &Path, stage: &Path, dir: &Path, out: &Path) -> Result<u64, String> {
    for e in entries.iter().filter(|e| e.status == "included") {
        std::fs::write(dir.join(&e.name), &e.text).map_err(|err| format!("{}: {err}", e.name))?;
    }
    let archive = stage.join("report.tar.gz");
    let status = Command::new(tar).env_clear().env("PATH", "/usr/bin:/bin").env("LC_ALL", "C")
        .arg("-czf").arg(&archive).arg("-C").arg(stage).arg(root)
        .status().map_err(|e| format!("tar: {e}"))?;
    if !status.success() { return Err(format!("tar failed ({status})")); }
    let tmp = out.with_file_name(format!("{}.partial", out.file_name().and_then(|n| n.to_str()).unwrap_or("report.tar.gz")));
    {
        let mut dst = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp)
            .map_err(|e| format!("{}: {e}", tmp.display()))?;
        let mut src = std::fs::File::open(&archive).map_err(|e| format!("{}: {e}", archive.display()))?;
        std::io::copy(&mut src, &mut dst).map_err(|e| format!("{}: {e}", tmp.display()))?;
        dst.flush().map_err(|e| e.to_string())?;
    }
    std::fs::set_permissions(&tmp, std::fs::Permissions::from_mode(0o600)).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, out).map_err(|e| format!("{}: {e}", out.display()))?;
    Ok(std::fs::metadata(out).map_err(|e| e.to_string())?.len())
}

/// Op "diag_report": a preview without "out", the archive with it.
pub fn request(req: &Value) -> Value {
    let src = Sources::system();
    let version = req["version"].as_str().unwrap_or("unknown");
    let (now, offset) = overview::local_clock();
    let entries = collect(&src, version, req["dgpu"].as_bool().unwrap_or(false), now, offset);
    match req["out"].as_str() {
        None => preview(&entries),
        Some(out) => {
            let root = format!("centurion-report-{}", overview::ymd(now, offset));
            match write_archive(&entries, &root, Path::new(out)) {
                Ok(bytes) => json!({"ok": true, "file": out, "bytes": bytes,
                                    "included": entries.iter().filter(|e| e.status == "included").count()}),
                Err(error) => json!({"ok": false, "error": error}),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("centurion-diag-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn identities_go_and_placeholders_and_other_values_stay() {
        let text = "Serial Number: SN-SECRET-1\nUUID: 1234abcd-secret\nID_FS_UUID=abcd-efgh\nPOWER_SUPPLY_SERIAL_NUMBER=BAT-SECRET\n\
                    Asset Tag: AT-SECRET\nSerial Number: Not Specified\nSerial Number: None\n\
                    MAC 00:11:22:33:44:55 and aa-bb-cc-dd-ee-ff\nlink 192.168.1.23 gw 10.0.0.1\n\
                    kernel ffff8881234567ab\nproduct_name: 16ARX8\nkernel 6.18.44\nnvme0: timeout, 0x1 lost\n";
        let (s, n) = sanitize(text);
        for secret in ["SN-SECRET-1", "1234abcd", "abcd-efgh", "BAT-SECRET", "AT-SECRET", "00:11:22", "aa-bb-cc", "192.168.1.23", "10.0.0.1", "ffff8881234567ab"] {
            assert!(!s.contains(secret), "{secret} left in {s}");
        }
        assert!(s.contains("Serial Number: Not Specified") && s.contains("Serial Number: None"), "placeholders stay: {s}");
        assert!(s.contains("product_name: 16ARX8"));
        assert!(s.contains("kernel 6.18.44"), "a version is not an address");
        assert!(s.contains("nvme0: timeout, 0x1 lost"));
        assert!(n >= 8, "masked count {n}");
    }

    #[test]
    fn mac_and_ip_boundaries() {
        assert_eq!(mask_macs("x 00:11:22:33:44:55 y").0, "x <mac> y");
        assert_eq!(mask_macs("00:11:22:33:44:5").0, "00:11:22:33:44:5", "too short");
        assert_eq!(mask_macs("a00:11:22:33:44:55").0, "a00:11:22:33:44:55", "part of a longer token");
        assert_eq!(mask_ipv4("ip 10.1.2.3, 300.1.1.1 1.2.3.4.5 v1.2.3").0, "ip <ip>, 300.1.1.1 1.2.3.4.5 v1.2.3");
    }

    #[test]
    fn collected_package_has_no_identifiers() {
        let root = temp("collect");
        let (state, logs, lib, dmi) = (root.join("state"), root.join("logs"), root.join("lib"), root.join("dmi"));
        for d in [&state, &logs, &lib, &dmi] { std::fs::create_dir_all(d).unwrap(); }
        std::fs::write(state.join("health.log"), "2026-10-29 10:00:00  boot=abc seq=1  ERROR   NVME   Serial Number: SN-SECRET-2  |  uuid: uuid-secret-9 from 203.0.113.7\n").unwrap();
        std::fs::write(state.join("kernel.log"), "boot=abc seq=2 ether aa:bb:cc:dd:ee:ff ok\n").unwrap();
        std::fs::write(logs.join("sleep.log"), "{\"start\": 1, \"end\": 2, \"woke_by\": \"serial=SN-SECRET-3\"}\n").unwrap();
        std::fs::write(logs.join("writes.log"), "Asset Tag: AT-SECRET-4\n").unwrap();
        std::fs::write(lib.join("exposure.json"), "{\"boots\": [{\"boot\": \"x\", \"start\": 1, \"note\": \"UUID: uuid-secret-5\"}]}").unwrap();
        std::fs::write(dmi.join("product_name"), "16ARX8\n").unwrap();
        std::fs::write(dmi.join("product_serial"), "SN-SECRET-6\n").unwrap();
        let src = Sources { state, log_dir: logs, lib_dir: lib, dmi, release: root.join("none"), nvidia_version: root.join("none") };

        let entries = collect(&src, "2.0.0", false, 1_000, 0);
        let names: Vec<&str> = entries.iter().map(|e| e.name.as_str()).collect();
        assert!(names.contains(&"system.txt") && names.contains(&"exposure.json"));
        assert!(!names.contains(&"dgpu.json"), "no dGPU section unless asked for");
        assert_eq!(entries.iter().find(|e| e.name == "battery.json").unwrap().status, "not present");
        for e in &entries {
            for secret in ["SN-SECRET", "uuid-secret", "aa:bb:cc:dd:ee:ff", "203.0.113.7", "AT-SECRET"] {
                assert!(!e.text.contains(secret), "{} still has {secret}", e.name);
            }
        }
        let system = &entries.iter().find(|e| e.name == "system.txt").unwrap().text;
        assert!(system.contains("product 16ARX8"), "{system}");
        assert!(!system.contains("SN-SECRET"), "the product_serial file is not read at all");
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn archive_is_written_privately_and_holds_no_identifiers() {
        let root = temp("archive");
        let out = root.join("centurion-report-test.tar.gz");
        let entries = vec![
            Entry { name: "system.txt".into(), status: "included", text: "Serial Number: SN-SECRET-7\nproduct 16ARX8\n".into(), masked: 0 },
            Entry { name: "health.log".into(), status: "not present", text: String::new(), masked: 0 },
        ];
        let text_entries: Vec<Entry> = entries.into_iter().map(|e| { let (t, m) = sanitize(&e.text); Entry { text: t, masked: m, ..e } }).collect();
        let bytes = write_archive(&text_entries, "centurion-report-test", &out).unwrap();
        assert!(bytes > 0);
        let mode = std::fs::metadata(&out).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600);
        let listing = Command::new("/usr/bin/tar").arg("-tzf").arg(&out).output().unwrap();
        let names = String::from_utf8_lossy(&listing.stdout).to_string();
        assert!(names.contains("centurion-report-test/system.txt") && !names.contains("health.log"), "{names}");
        let body = Command::new("/usr/bin/tar").arg("-xzOf").arg(&out).output().unwrap();
        let body = String::from_utf8_lossy(&body.stdout).to_string();
        assert!(body.contains("product 16ARX8") && !body.contains("SN-SECRET"), "{body}");
        assert!(write_archive(&text_entries, "x", Path::new("relative.tar.gz")).is_err());
        assert!(write_archive(&text_entries, "x", &root.join("report.zip")).is_err());
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn preview_lists_every_file_with_its_status() {
        let entries = vec![
            Entry { name: "a.txt".into(), status: "included", text: "é".repeat(PREVIEW_BYTES), masked: 2 },
            Entry { name: "b.log".into(), status: "unreadable", text: String::new(), masked: 0 },
        ];
        let p = preview(&entries);
        assert_eq!(p["masked"], 2);
        assert_eq!(p["files"][1]["status"], "unreadable");
        let shown = p["files"][0]["text"].as_str().unwrap();
        assert!(shown.len() <= PREVIEW_BYTES && shown.is_char_boundary(shown.len()));
    }
}
