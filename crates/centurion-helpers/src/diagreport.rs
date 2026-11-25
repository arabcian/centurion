//! Health → Overview → Create diagnostic report: one .tar.gz for a bug report, built from what the
//! Health pages already keep (classified kernel faults, kernel-log tails, sleeps, cooling, battery,
//! boot record, dGPU usage, the write log) plus a few lines about the machine. Runs as the user;
//! nothing here needs root, and an unreadable file is listed as such instead of failing the report.
//!
//! Every text passes `sanitize` before it is kept: kernel addresses (health::scrub), the value of any
//! line whose key names a serial number, UUID or asset tag (the rule of the GUI's SERIAL_RE: "Serial
//! Number: X", "UUID=X"), MAC addresses, IPv4 and IPv6 addresses, and the user name in /home/<name>
//! paths. Saved log boot IDs and known structured identity and credential fields are also masked.
//! Placeholders that identify nothing ("Not Specified", "None", "0"…) are left as they are.
//!
//! stdin:  {"op":"diag_report"}                        preview: each file's status and text
//!         {"op":"diag_report","out":"/abs/x.tar.gz"}  writes the archive
//!         optional: "snapshot" (GUI-owned path to preserve preview bytes across requests)
//!         optional: "replace_existing": false (publish only if the output name is free)
//!         optional: "version" (the GUI's version string), "dgpu": bool (NVIDIA status when present)
//!   {"op":"diag_report_page","snapshot":"/abs/snapshot.json","preview_schema":2,
//!    "selected":[{"name":"kernel.log","sha256":"..."}],"page":1}  reads one reviewed file page

use crate::health;
use crate::overview;
use serde_json::{json, Value};
use std::ffi::CString;
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt};
#[cfg(test)]
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Tail kept of each kernel/health log.
const LOG_TAIL: u64 = 512 << 10;
/// Whole-file cap for the JSON records (a larger file is left out).
const JSON_MAX: u64 = 4 << 20;
/// Lines kept of the sleep and write logs.
const LINES: usize = 200;
/// Serialized size limit for the GUI-owned report snapshot.
const SNAPSHOT_MAX: u64 = 32 << 20;
/// Text shown per file in the preview (the archive keeps everything).
const PREVIEW_BYTES: usize = 32 << 10;

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
                let word = |s: &str| s.find(|c: char| c.is_whitespace() || "\"',;}])|".contains(c)).unwrap_or(s.len());
                let mut token_len = word(rest);
                let token = &rest[..token_len];
                let placeholder = is_placeholder(token) || is_placeholder(rest.trim_end())
                    || rest.starts_with("Not ") || rest.starts_with("To Be Filled") || rest.starts_with("Default ");
                if token_len == 0 || placeholder { continue; }
                // "Serial Number: ABC DEF": after ':' the value runs on over single spaces, up to a double
                // space, punctuation or the next "key:" / "key=". After '=' (cmdline, uevent) it is one word.
                if line.as_bytes()[sep] == b':' {
                    while let Some(next) = rest[token_len..].strip_prefix(' ').filter(|n| !n.starts_with(char::is_whitespace)) {
                        let w = &next[..word(next)];
                        if w.is_empty() || w.ends_with(':') || w.contains('=') { break; }
                        token_len += 1 + w.len();
                    }
                }
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

/// 2001:db8::1, fe80::1c2d:3e4f:5a6b:7c8d → <ip>. A token of hex digits and colons that parses as an IPv6
/// address; clock times ("10:00:00") and PCI addresses ("0000:01:00.0") have too few groups to be one.
/// Runs after mask_macs and mask_ipv4, so what is left of a MAC or an IPv4-mapped address is not looked at.
fn mask_ipv6(text: &str) -> (String, u64) {
    let b = text.as_bytes();
    let part = |c: u8| c.is_ascii_hexdigit() || c == b':';
    let mut out = String::with_capacity(text.len());
    let (mut i, mut n) = (0, 0);
    while i < b.len() {
        if part(b[i]) && (i == 0 || !(b[i - 1].is_ascii_alphanumeric() || b[i - 1] == b'.')) {
            let end = i + b[i..].iter().take_while(|&&c| part(c)).count();
            // Hex digits and colons only: ASCII, so `end` is a character boundary.
            let token = &text[i..end];
            let ends = b.get(end).map_or(true, |c| !c.is_ascii_alphanumeric());
            let digits = token.bytes().any(|c| c != b':');  // a bare "::" is punctuation, not an address
            if ends && digits && token.matches(':').count() >= 2 && token.parse::<std::net::Ipv6Addr>().is_ok() {
                out.push_str("<ip>");
                n += 1;
            } else {
                out.push_str(token);
            }
            i = end;
            continue;
        }
        let ch = text[i..].chars().next().unwrap_or('\0');
        out.push(ch);
        i += ch.len_utf8().max(1);
    }
    (out, n)
}

/// /home/alice/Games/x.exe → /home/<user>/Games/x.exe: process names and commands in the logs carry the
/// account name with them. Only the path component after /home/ goes; the rest of the path stays readable.
fn mask_home(text: &str) -> (String, u64) {
    const HOME: &str = "/home/";
    let mut out = String::with_capacity(text.len());
    let (mut rest, mut n) = (text, 0);
    while let Some(p) = rest.find(HOME) {
        let after = &rest[p + HOME.len()..];
        let len = after.find(|c: char| c == '/' || c.is_whitespace() || "\"',;:)]}|".contains(c)).unwrap_or(after.len());
        out.push_str(&rest[..p + HOME.len()]);
        if len > 0 && &after[..len] != "<user>" {
            out.push_str("<user>");
            n += 1;
        } else {
            out.push_str(&after[..len]);
        }
        rest = &after[len..];
    }
    out.push_str(rest);
    (out, n)
}

// Saved health/kernel lines carry the boot identifier as a separate field.
fn mask_boot_tags(text: &str) -> (String, u64) {
    let mut out = String::with_capacity(text.len());
    let mut cursor = 0;
    let mut count = 0;
    for (key, tag) in text.match_indices("boot=") {
        if key != 0 && !text[..key].ends_with(char::is_whitespace) { continue; }
        let start = key + tag.len();
        let rest = &text[start..];
        let len = rest.find(char::is_whitespace).unwrap_or(rest.len());
        let value = &rest[..len];
        if value.is_empty() || is_placeholder(value) || value == "<hidden>" { continue; }
        out.push_str(&text[cursor..start]);
        out.push_str("<hidden>");
        cursor = start + len;
        count += 1;
    }
    out.push_str(&text[cursor..]);
    (out, count)
}

fn private_field_name(key: &str) -> bool {
    // Exact canonical names cover snake_case, camelCase, and labels from
    // udev without hiding unrelated measurements such as serialEnabled.
    let name: String = key.chars().filter(|c| !matches!(*c, '_' | '-' | ' '))
        .map(|c| c.to_ascii_lowercase()).collect();
    matches!(name.as_str(),
        "hostname" | "username" | "machineid" | "bootid" | "bootuuid" | "boot" |
        "ssid" | "bssid" | "mac" | "macaddress" | "permanentmacaddress" | "ipaddress" |
        "ipv4address" | "ipv6address" | "serial" | "serialnumber" | "deviceserial" |
        "diskserial" | "idserial" | "idserialshort" | "uuid" | "gpuuuid" | "diskuuid" |
        "filesystemuuid" | "partuuid" | "assettag" | "wwid" | "idwwn" | "mountpoint" |
        "password" | "passwd" | "apikey" | "accesstoken" | "refreshtoken" | "authorization" | "psk")
}

// Structured records can carry private fields without a textual label delimiter.
fn mask_json_fields(value: &mut Value) -> u64 {
    match value {
        Value::Object(fields) => fields.iter_mut().map(|(key, value)| {
            if private_field_name(key) && !value.is_null() && value.as_str() != Some("<hidden>") {
                *value = Value::String("<hidden>".to_owned());
                1
            } else {
                mask_json_fields(value)
            }
        }).sum(),
        Value::Array(items) => items.iter_mut().map(mask_json_fields).sum(),
        Value::String(text) => {
            let (masked, count) = sanitize(text);
            *text = masked;
            count
        }
        _ => 0,
    }
}

fn mask_structured(raw: &str) -> Option<(String, u64)> {
    let mut json = serde_json::from_str::<Value>(raw).ok()?;
    let count = mask_json_fields(&mut json);
    Some((serde_json::to_string(&json).ok()?, count))
}

fn mask_sleep_log(raw: &str) -> Option<(String, u64)> {
    let mut out = String::new();
    let mut count = 0;
    for line in raw.lines() {
        // There is no fallback to raw text for a malformed structured record.
        let Ok(mut record @ Value::Object(_)) = serde_json::from_str::<Value>(line) else { continue };
        count += mask_json_fields(&mut record);
        out.push_str(&serde_json::to_string(&record).ok()?);
        out.push('\n');
    }
    (raw.trim().is_empty() || !out.is_empty()).then_some((out, count))
}

/// Everything a text goes through before it is kept, with the number of values masked.
pub fn sanitize(text: &str) -> (String, u64) {
    let scrubbed = health::scrub(text);
    let addr = scrubbed.matches("<addr>").count().saturating_sub(text.matches("<addr>").count()) as u64;
    let (tagged, boots) = mask_boot_tags(&scrubbed);
    let (a, serials) = mask_serials(&tagged);
    let (b, macs) = mask_macs(&a);
    let (c, ips) = mask_ipv4(&b);
    let (d, ips6) = mask_ipv6(&c);
    let (e, homes) = mask_home(&d);
    (e, addr + boots + serials + macs + ips + ips6 + homes)
}

// ── collecting ───────────────────────────────────────────────────────────────

/// (status, text) of one file: the last `lines` lines, or the last `tail` bytes.
// Open the actual descriptor first; reject symlinks, FIFOs and changing whole-file captures.
fn fetch(path: &Path, tail: u64, lines: Option<usize>) -> (&'static str, Option<String>) {
    read_bounded(path, tail, lines, false)
}
fn json_file(path: &Path) -> (&'static str, Option<String>) {
    read_bounded(path, JSON_MAX, None, true)
}
fn read_bounded(path: &Path, cap: u64, lines: Option<usize>, whole: bool) -> (&'static str, Option<String>) {
    let mut file = match std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK).open(path) {
        Ok(f) => f,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return ("not present", None),
        Err(_) => return ("unreadable", None),
    };
    let Ok(before) = file.metadata() else { return ("unreadable", None); };
    if !before.is_file() { return ("unreadable", None); }
    if whole && before.len() > cap { return ("too large", None); }
    let start = before.len().saturating_sub(cap);
    let mut partial_first_line = false;
    if start > 0 {
        let mut previous = [0];
        if file.seek(SeekFrom::Start(start - 1)).is_err() || file.read_exact(&mut previous).is_err() {
            return ("unreadable", None);
        }
        partial_first_line = previous[0] != b'\n';
    }
    let limit = if whole { cap + 1 } else { before.len() - start };
    let mut bytes = Vec::new();
    if (&mut file).take(limit).read_to_end(&mut bytes).is_err() { return ("unreadable", None); }
    if whole {
        let Ok(after) = file.metadata() else { return ("unreadable", None); };
        if bytes.len() as u64 > cap { return ("too large", None); }
        if bytes.len() as u64 != before.len() || before.len() != after.len() || before.modified().ok() != after.modified().ok() {
            return ("unreadable", None);
        }
    } else {
        if bytes.len() as u64 != limit { return ("unreadable", None); }
        if partial_first_line {
            // Never export a partial first log line whose identity key may have been cut away.
            let Some(newline) = bytes.iter().position(|&b| b == b'\n') else { return ("too large", None); };
            bytes.drain(..=newline);
        }
        // Ignore an incomplete final line as well (concurrent append / cap).
        if !bytes.is_empty() && bytes.last() != Some(&b'\n') {
            let Some(newline) = bytes.iter().rposition(|&b| b == b'\n') else { return ("unreadable", None); };
            bytes.truncate(newline + 1);
        }
    }
    let Ok(text) = String::from_utf8(bytes) else { return ("unreadable", None); };
    let text = match lines {
        Some(n) => { let v: Vec<&str> = text.lines().collect(); v[v.len().saturating_sub(n)..].join("\n") + "\n" }
        None => text,
    };
    ("included", Some(text))
}

fn entry(name: &str, status: &'static str, raw: Option<String>) -> Entry {
    let (text, masked) = match raw {
        Some(raw) if name.ends_with(".json") || name == "sleep.log" => {
            let masked = if name == "sleep.log" { mask_sleep_log(&raw) } else { mask_structured(&raw) };
            match masked {
                Some((text, count)) if text.len() as u64 <= JSON_MAX => (text, count),
                other => return Entry { name: name.to_owned(), status: if other.is_some() { "too large" } else { "unreadable" }, text: String::new(), masked: 0 },
            }
        }
        Some(raw) => sanitize(&raw),
        None => (String::new(), 0),
    };
    Entry { name: name.to_owned(), status, text, masked }
}

fn read_trim(p: &Path) -> Option<String> {
    let file = std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK).open(p).ok()?;
    if !file.metadata().ok()?.is_file() { return None; }
    let mut data = Vec::new();
    file.take(4097).read_to_end(&mut data).ok()?;
    if data.len() > 4096 { return None; }
    String::from_utf8(data).ok().map(|s| s.trim().to_owned()).filter(|s| !s.is_empty())
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

Best-effort redaction before saving (not a guarantee of anonymity)
  boot IDs and known private fields      -> <hidden>
  serial numbers, UUIDs and asset tags   -> <hidden>
  MAC addresses                          -> <mac>
  IPv4 and IPv6 addresses                -> <ip>
  user names in home-folder paths        -> <user>
  kernel addresses                       -> <addr>
Only selected files with matching preview digests are archived.
Logs are bounded tails. Malformed sleep records are omitted.
Long files are shown in pages when a saved snapshot is available.
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
    let (status, raw) = fetch(&src.log_dir.join("sleep.log"), LOG_TAIL, Some(LINES));
    v.push(entry("sleep.log", status, raw));
    for (name, path) in [("cooling.json", src.state.join("cooling.json")), ("battery.json", src.state.join("battery.json")),
                         ("dgpu-usage.json", src.state.join("dgpu.json")), ("exposure.json", src.lib_dir.join("exposure.json"))] {
        let (status, raw) = json_file(&path);
        v.push(entry(name, status, raw));
    }
    let (status, raw) = fetch(&src.log_dir.join("writes.log"), LOG_TAIL, Some(LINES));
    v.push(entry("writes.log", status, raw));
    if dgpu {
        let text = serde_json::to_string_pretty(&crate::dgpu::status()).ok();
        v.push(entry("dgpu.json", if text.is_some() { "included" } else { "unreadable" }, text));
    }
    v
}

// ── output ───────────────────────────────────────────────────────────────────

fn preview_boundary(text: &str, mut offset: usize) -> usize {
    while !text.is_char_boundary(offset) { offset -= 1; }
    offset
}

/// The file list and each file's text, cut to PREVIEW_BYTES on a character boundary.
fn preview(entries: &[Entry]) -> Value {
    let files: Vec<Value> = entries.iter().map(|e| {
        let cut = preview_boundary(&e.text, e.text.len().min(PREVIEW_BYTES));
        json!({"name": e.name, "status": e.status, "bytes": e.text.len(), "masked": e.masked,
               "sha256": content_digest(&e.text), "truncated": cut < e.text.len(), "text": &e.text[..cut]})
    }).collect();
    let masked: u64 = entries.iter().map(|e| e.masked).sum();
    json!({"ok": true, "preview_schema": 2, "files": files, "masked": masked})
}

fn content_digest(text: &str) -> String {
    crate::bootlogo::sha256(text.as_bytes()).iter().map(|b| format!("{b:02x}")).collect::<Vec<_>>().join("")
}

// The GUI approves a set of named, sanitized byte streams. A changed source must be reviewed again.
// The caller never supplies archive entry names or contents directly to the packer.
fn reviewed_entries(entries: Vec<Entry>, req: &Value) -> Result<Vec<Entry>, &'static str> {
    if req["preview_schema"].as_u64() != Some(2) { return Err("review the report with the current preview before saving"); }
    let selection = req["selected"].as_array().ok_or("missing report selection")?;
    if selection.is_empty() || selection.len() > entries.len() { return Err("invalid report selection"); }
    let mut selected = std::collections::BTreeMap::new();
    for value in selection {
        let name = value["name"].as_str().ok_or("invalid selected file")?;
        let digest = value["sha256"].as_str().ok_or("missing preview digest")?;
        if name.len() > 128 || digest.len() != 64 || !digest.bytes().all(|b| b.is_ascii_hexdigit()) || selected.insert(name, digest).is_some() {
            return Err("invalid or duplicate selected file");
        }
    }
    let mut result = Vec::new();
    for entry in entries {
        if let Some(digest) = selected.remove(entry.name.as_str()) {
            if entry.status != "included" || content_digest(&entry.text) != digest {
                return Err("report contents changed after preview; reopen the report and review again");
            }
            result.push(entry);
        }
    }
    if !selected.is_empty() { return Err("selected report file unavailable"); }
    Ok(result)
}

/// Writes the included entries as <root>/<name> in a private staging folder, packs them with tar and
/// copies the archive to `out` (created 0600). Returns the archive size in bytes.
pub fn write_archive(entries: &[Entry], root: &str, out: &Path) -> Result<u64, String> {
    write_archive_with_policy(entries, root, out, true)
}

fn write_archive_with_policy(entries: &[Entry], root: &str, out: &Path, replace_existing: bool) -> Result<u64, String> {
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
    std::fs::DirBuilder::new().mode(0o700).create(&stage).map_err(|e| format!("{}: {e}", stage.display()))?;
    let result = std::fs::create_dir(&dir).map_err(|e| e.to_string())
        .and_then(|_| pack(entries, root, tar, &stage, &dir, out, replace_existing));
    let _ = std::fs::remove_dir_all(&stage);
    result
}

fn publish_archive(tmp: &Path, out: &Path, replace_existing: bool) -> std::io::Result<()> {
    if replace_existing { return std::fs::rename(tmp, out); }
    let from = CString::new(tmp.as_os_str().as_bytes())?;
    let to = CString::new(out.as_os_str().as_bytes())?;
    // Both paths live in the destination directory. This checks for existence
    // in the same operation that publishes the completed file.
    let result = unsafe {
        libc::syscall(libc::SYS_renameat2, libc::AT_FDCWD, from.as_ptr(), libc::AT_FDCWD,
            to.as_ptr(), libc::RENAME_NOREPLACE)
    };
    if result == 0 { return Ok(()); }
    let error = std::io::Error::last_os_error();
    if !matches!(error.raw_os_error(), Some(libc::ENOSYS | libc::EINVAL | libc::EOPNOTSUPP)) {
        return Err(error);
    }
    std::fs::hard_link(tmp, out)?;
    let _ = std::fs::remove_file(tmp);
    Ok(())
}

fn pack(entries: &[Entry], root: &str, tar: &Path, stage: &Path, dir: &Path, out: &Path, replace_existing: bool) -> Result<u64, String> {
    for e in entries.iter().filter(|e| e.status == "included") {
        std::fs::write(dir.join(&e.name), &e.text).map_err(|err| format!("{}: {err}", e.name))?;
    }
    let archive = stage.join("report.tar.gz");
    let status = Command::new(tar).env_clear().env("PATH", "/usr/bin:/bin").env("LC_ALL", "C")
        .arg("-czf").arg(&archive).arg("-C").arg(stage).arg(root)
        .status().map_err(|e| format!("tar: {e}"))?;
    if !status.success() { return Err(format!("tar failed ({status})")); }
    let tmp = out.with_file_name(format!(".centurion-report-{}-{}.partial", std::process::id(),
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_nanos())));
    let mut dst = std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(&tmp)
        .map_err(|e| format!("{}: {e}", tmp.display()))?;
    let result = (|| {
        let mut src = std::fs::File::open(&archive).map_err(|e| format!("{}: {e}", archive.display()))?;
        let bytes = std::io::copy(&mut src, &mut dst).map_err(|e| format!("{}: {e}", tmp.display()))?;
        dst.flush().map_err(|e| e.to_string())?;
        dst.sync_all().map_err(|e| e.to_string())?;
        publish_archive(&tmp, out, replace_existing).map_err(|e| format!("{}: {e}", out.display()))?;
        Ok(bytes)
    })();
    if result.is_err() { let _ = std::fs::remove_file(&tmp); }
    result
}

// Only included, already sanitized entries are needed by the save request.
fn freeze_entries(path: &Path, entries: &[Entry]) -> std::io::Result<()> {
    let included: Vec<_> = entries.iter().filter(|e| e.status == "included")
        .map(|e| (&e.name, &e.text)).collect();
    let bytes = serde_json::to_vec(&included)?;
    if !path.is_absolute() || bytes.len() as u64 > SNAPSHOT_MAX {
        return Err(std::io::Error::new(std::io::ErrorKind::InvalidInput, "invalid or oversized report snapshot"));
    }
    let mut file = std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(path)?;
    file.write_all(&bytes)
}

fn thaw_entries(path: &Path) -> Option<Vec<Entry>> {
    if !path.is_absolute() { return None; }
    let file = std::fs::OpenOptions::new().read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK).open(path).ok()?;
    let before = file.metadata().ok()?;
    if !before.is_file() || before.uid() != unsafe { libc::geteuid() }
        || before.mode() & 0o077 != 0 || before.nlink() != 1 || before.len() > SNAPSHOT_MAX {
        return None;
    }
    let mut bytes = Vec::new();
    let mut bounded = file.take(before.len() + 1);
    bounded.read_to_end(&mut bytes).ok()?;
    let after = bounded.get_ref().metadata().ok()?;
    if bytes.len() as u64 != before.len() || before.len() != after.len()
        || before.modified().ok() != after.modified().ok() {
        return None;
    }
    let entries: Vec<(String, String)> = serde_json::from_slice(&bytes).ok()?;
    if entries.len() > 32 { return None; }
    let mut names = std::collections::BTreeSet::new();
    for (name, _) in &entries {
        if name.is_empty() || name.len() > 128 || name.starts_with('.')
            || !name.bytes().all(|b| b.is_ascii_alphanumeric() || b".-".contains(&b))
            || !names.insert(name) {
            return None;
        }
    }
    Some(entries.into_iter().map(|(name, text)| Entry { name, text, status: "included", masked: 0 }).collect())
}

/// Return one page from the same private snapshot and digest used for export.
pub fn page_request(req: &Value) -> Value {
    let error = |message| json!({"ok": false, "error": message});
    if unsafe { libc::geteuid() } == 0 {
        return error("the report snapshot is read as the user");
    }
    let Some(path) = req["snapshot"].as_str() else { return error("missing report snapshot"); };
    let Some(page) = req["page"].as_u64().and_then(|n| usize::try_from(n).ok()) else {
        return error("invalid preview page");
    };
    if req["selected"].as_array().map(Vec::len) != Some(1) {
        return error("a preview page requires one selected file");
    }
    let Some(entries) = thaw_entries(Path::new(path)) else { return error("invalid or missing report snapshot"); };
    let entries = match reviewed_entries(entries, req) {
        Ok(entries) => entries,
        Err(message) => return error(message),
    };
    let text = &entries[0].text;
    let page_count = text.len().div_ceil(PREVIEW_BYTES).max(1);
    if page >= page_count { return error("preview page out of range"); }
    let start = preview_boundary(text, page * PREVIEW_BYTES);
    let end = preview_boundary(text, ((page + 1) * PREVIEW_BYTES).min(text.len()));
    json!({"ok": true, "page": page, "page_count": page_count, "text": &text[start..end]})
}

/// Op "diag_report": a preview without "out", the archive with it.
pub fn request(req: &Value) -> Value {
    let out = req["out"].as_str();
    let snapshot = req["snapshot"].as_str().map(Path::new);
    // The op is served before tune-helper's root check.
    if (out.is_some() || snapshot.is_some()) && unsafe { libc::geteuid() } == 0 {
        return json!({"ok": false, "error": "the report is written as the user, not as root"});
    }
    let (now, offset) = overview::local_clock();
    let entries = if let (Some(_), Some(snapshot)) = (out, snapshot) {
        match thaw_entries(snapshot) {
            Some(entries) => entries,
            None => return json!({"ok": false, "error": "invalid or missing report snapshot"}),
        }
    } else {
        let version = req["version"].as_str().unwrap_or("unknown");
        collect(&Sources::system(), version, req["dgpu"].as_bool().unwrap_or(false), now, offset)
    };
    match out {
        None => {
            let mut response = preview(&entries);
            if let Some(snapshot) = snapshot {
                if let Err(error) = freeze_entries(snapshot, &entries) {
                    return json!({"ok": false, "error": format!("could not keep report snapshot: {error}")});
                }
                response["frozen"] = json!(true);
                response["page_bytes"] = json!(PREVIEW_BYTES);
            }
            response
        }
        Some(out) => {
            let entries = match reviewed_entries(entries, req) {
                Ok(entries) => entries,
                Err(error) => return json!({"ok": false, "error": error}),
            };
            let root = format!("centurion-report-{}", overview::ymd(now, offset));
            let output = Path::new(out);
            let archive = match req.get("replace_existing") {
                None | Some(Value::Bool(true)) => write_archive(&entries, &root, output),
                Some(Value::Bool(false)) => write_archive_with_policy(&entries, &root, output, false),
                Some(_) => return json!({"ok": false, "error": "replace_existing must be a boolean"}),
            };
            match archive {
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
    fn multi_word_serials_are_hidden_whole() {
        assert_eq!(mask_serials("Serial Number: ABC DEF\n").0, "Serial Number: <hidden>\n");
        assert_eq!(mask_serials("{\"serial\": \"AB CD\", \"x\": 1}").0, "{\"serial\": \"<hidden>\", \"x\": 1}");
        assert_eq!(mask_serials("serial: AB CD  |  model: X").0, "serial: <hidden>  |  model: X");
        assert_eq!(mask_serials("serial: AB model: X").0, "serial: <hidden> model: X");
        // '=' values stay one word, so the rest of a command line survives.
        assert_eq!(mask_serials("root=UUID=12ab-34cd ro quiet").0, "root=UUID=<hidden> ro quiet");
    }

    #[test]
    fn mac_and_ip_boundaries() {
        assert_eq!(mask_macs("x 00:11:22:33:44:55 y").0, "x <mac> y");
        assert_eq!(mask_macs("00:11:22:33:44:5").0, "00:11:22:33:44:5", "too short");
        assert_eq!(mask_macs("a00:11:22:33:44:55").0, "a00:11:22:33:44:55", "part of a longer token");
        assert_eq!(mask_ipv4("ip 10.1.2.3, 300.1.1.1 1.2.3.4.5 v1.2.3").0, "ip <ip>, 300.1.1.1 1.2.3.4.5 v1.2.3");
    }

    #[test]
    fn ipv6_addresses_go_and_look_alikes_stay() {
        assert_eq!(mask_ipv6("from 2001:db8:85a3::8a2e:370:7334 port 22").0, "from <ip> port 22");
        assert_eq!(mask_ipv6("fe80::1c2d:3e4f:5a6b:7c8d%wlan0, ::1").0, "<ip>%wlan0, <ip>");
        assert_eq!(mask_ipv6("[2001:db8::1]:443").0, "[<ip>]:443");
        assert_eq!(mask_ipv6("2001:0db8:0000:0000:0000:ff00:0042:8329").1, 1);
        for keep in ["2026-10-29 10:00:00 up", "nvidia 0000:01:00.0: Xid", "RIP: 0010:foo+0x1/0x2", "std::io and a::b::c", "ratio 16:9",
                     "CPU:2 (1a:44:0) MC1_STATUS", "boot=abcdef12 seq=7", "::", "dead:beef", "ünïcode: ok"] {
            assert_eq!(mask_ipv6(keep), (keep.to_owned(), 0), "{keep}");
        }
        // Through the whole chain: an IPv4-mapped address leaves nothing of the IPv4 part behind.
        let (s, n) = sanitize("peer ::ffff:192.168.1.23 via fe80::1");
        assert!(!s.contains("192.168") && !s.contains("fe80"), "{s}");
        assert_eq!(n, 2);
    }

    #[test]
    fn home_paths_lose_the_user_name_only() {
        assert_eq!(mask_home("exe=/home/alice/Games/x.exe arg").0, "exe=/home/<user>/Games/x.exe arg");
        assert_eq!(mask_home("\"/home/alice\", '/home/bob' /home/çağrı/x").0, "\"/home/<user>\", '/home/<user>' /home/<user>/x");
        assert_eq!(mask_home("cd /home/alice").0, "cd /home/<user>");
        assert_eq!(mask_home("ls /home/ and /homework/alice"), ("ls /home/ and /homework/alice".to_owned(), 0));
        // Already masked (a second pass, or the README's own text): not counted again.
        assert_eq!(mask_home("/home/<user>/x"), ("/home/<user>/x".to_owned(), 0));
        assert_eq!(sanitize(README).1, 0, "the README names the placeholders; none of them is a masked value");
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
    fn a_planted_fixed_partial_link_is_not_followed() {
        let root = temp("partial-link");
        let victim = root.join("victim.conf");
        std::fs::write(&victim, "ORIGINAL").unwrap();
        std::fs::set_permissions(&victim, std::fs::Permissions::from_mode(0o644)).unwrap();
        std::os::unix::fs::symlink(&victim, root.join("r.tar.gz.partial")).unwrap();
        let entries = vec![Entry { name: "a.txt".into(), status: "included", text: "x".into(), masked: 0 }];
        write_archive(&entries, "r", &root.join("r.tar.gz")).unwrap();
        assert_eq!(std::fs::read_to_string(&victim).unwrap(), "ORIGINAL");
        assert_eq!(std::fs::metadata(&victim).unwrap().permissions().mode() & 0o777, 0o644);
        assert!(std::fs::symlink_metadata(root.join("r.tar.gz")).unwrap().is_file());
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn root_never_writes_the_archive() {
        if unsafe { libc::geteuid() } != 0 { return; }
        let root = temp("as-root");
        let out = root.join("r.tar.gz");
        let r = request(&json!({"op": "diag_report", "out": out}));
        assert_eq!(r["ok"], false, "{r}");
        assert!(!out.exists());
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn structured_and_bounded_capture() {
        let (text, count) = mask_structured(r#"{"boot":"secret-boot","nested":[{"hostname":"secret-host","ssid":"secret-wifi","value":3}]}"#).unwrap();
        assert!(serde_json::from_str::<Value>(&text).is_ok());
        assert!(mask_structured("{broken").is_none());
        assert_eq!(count, 3); assert!(!text.contains("secret-")); assert!(text.contains("3"));
        let root = temp("bounded-capture"); let path = root.join("log");
        std::fs::write(&path, "serial=long-secret-without-newline").unwrap();
        assert_ne!(fetch(&path, 8, None).0, "included");
        std::fs::write(&path, "secret-prefix\ncomplete\npartial").unwrap();
        assert_eq!(fetch(&path, 18, None).1.as_deref(), Some("complete\n"));
        let complete = "first\nlast\n";
        std::fs::write(&path, format!("prior\n{complete}")).unwrap();
        assert_eq!(fetch(&path, complete.len() as u64, None).1.as_deref(), Some(complete));
        assert_eq!(fetch(&path, complete.len() as u64 - 1, None).1.as_deref(), Some("last\n"));
        let link = root.join("link"); std::os::unix::fs::symlink(&path, &link).unwrap();
        assert_eq!(fetch(&link, 32, None).0, "unreadable");
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn reviewed_selection_rejects_changed_duplicate_and_unknown_entries() {
        let capture = || vec![Entry { name: "a.txt".into(), status: "included", text: "first".into(), masked: 0 },
            Entry { name: "b.txt".into(), status: "included", text: "second".into(), masked: 0 }];
        let mut req = json!({"preview_schema": 2, "selected": [{"name": "a.txt", "sha256": content_digest("first")}]});
        assert_eq!(reviewed_entries(capture(), &req).unwrap().len(), 1);
        let mut changed = capture(); changed[0].text.push('!');
        assert!(reviewed_entries(changed, &req).is_err());
        let item = req["selected"][0].clone(); req["selected"].as_array_mut().unwrap().push(item);
        assert!(reviewed_entries(capture(), &req).is_err());
        req["selected"] = json!([{"name": "../secret", "sha256": content_digest("first")}]);
        assert!(reviewed_entries(capture(), &req).is_err());
        assert!(reviewed_entries(capture(), &json!({})).is_err());
    }

    #[test]
    fn frozen_report_archives_the_reviewed_capture() {
        let root = temp("frozen");
        let snapshot = root.join("snapshot.json");
        let source = root.join("source.log");
        std::fs::write(&source, "UUID=private-value\noriginal\n").unwrap();
        let (status, raw) = fetch(&source, LOG_TAIL, None);
        let entries = [entry("source.log", status, raw),
            entry("README.txt", "included", Some(README.to_owned()))];
        let response = preview(&entries);
        freeze_entries(&snapshot, &entries).unwrap();
        assert!(freeze_entries(&snapshot, &entries).is_err());

        std::fs::write(&source, "new content\n").unwrap();
        let out = root.join("report.tar.gz");
        let req = json!({"op": "diag_report", "out": out, "snapshot": snapshot,
            "preview_schema": 2, "selected": [{"name": "source.log", "sha256": response["files"][0]["sha256"]}]});
        if unsafe { libc::geteuid() } != 0 {
            let reply = request(&req);
            assert_eq!(reply["ok"], true, "{reply}");
            let body = Command::new("/usr/bin/tar").arg("-xzOf").arg(&out).output().unwrap();
            assert!(body.status.success());
            assert_eq!(body.stdout, entries[0].text.as_bytes());
            assert!(!String::from_utf8_lossy(&body.stdout).contains("private-value"));
        }

        let link = root.join("link");
        std::os::unix::fs::symlink(&snapshot, &link).unwrap();
        assert!(thaw_entries(&link).is_none());
        std::fs::write(&snapshot, br#"[["../escape","file"]]"#).unwrap();
        assert!(thaw_entries(&snapshot).is_none());
        std::fs::write(&snapshot, br#"[["source.log","tampered"]]"#).unwrap();
        assert!(reviewed_entries(thaw_entries(&snapshot).unwrap(), &req).is_err());
        std::fs::remove_dir_all(root).unwrap();
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
        assert_eq!(p["files"][0]["truncated"], true);
        assert_eq!(p["files"][0]["sha256"], content_digest(&entries[0].text));
    }
}
