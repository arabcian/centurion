//! Health → Sleep → "Can this machine hibernate?": read-only checks of what a hibernate needs. Called as
//! the user through tune-profile-helper (op "hibernate_check"); every file read is world-readable.
//!
//! Only fixed paths below `root` (production: "/") are read; nothing is written. `analyse` turns the
//! readings into finding codes and the GUI words them, as in sleepdiag.

use serde_json::{json, Value};
use std::fs;
use std::path::Path;

/// systemd's EFI variable for resuming without `resume=` (systemd 255+, `HibernateLocation`).
const HIBERNATE_LOCATION: &str = "sys/firmware/efi/efivars/HibernateLocation-8cf2644b-4b0b-428f-9387-6d876050dc67";

fn text(p: &Path) -> Option<String> {
    let s = fs::read_to_string(p).ok()?;
    let s = s.trim().to_owned();
    (!s.is_empty()).then_some(s)
}

/// A "Name:   1234 kB" line of /proc/meminfo, in KiB.
fn meminfo_kib(meminfo: &str, key: &str) -> Option<u64> {
    meminfo.lines().find_map(|l| l.strip_prefix(key)?.strip_prefix(':')?.split_whitespace().next()?.parse().ok())
}

/// The dm-crypt flavour of a device-mapper swap ("CRYPT-LUKS2-…" → "luks", "CRYPT-PLAIN-…" → "plain").
fn crypt_kind(root: &Path, dev: &str) -> Option<&'static str> {
    let dm = if let Some(n) = dev.strip_prefix("/dev/dm-") {
        format!("dm-{n}")
    } else {
        let name = dev.strip_prefix("/dev/mapper/")?;
        fs::read_dir(root.join("sys/block")).ok()?.flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .find(|b| b.starts_with("dm-") && text(&root.join(format!("sys/block/{b}/dm/name"))).as_deref() == Some(name))?
    };
    let uuid = text(&root.join(format!("sys/block/{dm}/dm/uuid")))?;
    if uuid.starts_with("CRYPT-LUKS") { Some("luks") } else if uuid.starts_with("CRYPT-") { Some("plain") } else { None }
}

pub fn readings(root: &Path) -> Value {
    let meminfo = text(&root.join("proc/meminfo")).unwrap_or_default();
    // /proc/swaps: "Filename Type Size Used Priority", sizes in KiB. zram lives in RAM: it cannot hold the image.
    let swaps: Vec<Value> = text(&root.join("proc/swaps")).unwrap_or_default().lines().skip(1).filter_map(|l| {
        let f: Vec<&str> = l.split_whitespace().collect();
        let (name, kind, size) = (*f.first()?, *f.get(1)?, f.get(2)?.parse::<u64>().ok()?);
        let name = name.replace("\\040", " ");
        Some(json!({"name": name, "type": kind, "kib": size, "zram": name.starts_with("/dev/zram"), "crypt": crypt_kind(root, &name)}))
    }).collect();
    let cmdline = text(&root.join("proc/cmdline")).unwrap_or_default();
    let resume = cmdline.split_whitespace().find_map(|a| a.strip_prefix("resume=")).map(str::to_owned);
    // "none [integrity] confidentiality": the bracketed one is in force.
    let lockdown = text(&root.join("sys/kernel/security/lockdown"))
        .and_then(|l| l.split_whitespace().find(|w| w.starts_with('[')).map(|w| w.trim_matches(['[', ']']).to_owned()));
    json!({
        "mem_kib": meminfo_kib(&meminfo, "MemTotal"),
        "swaps": swaps,
        "resume": resume,
        "hibernate_location": root.join(HIBERNATE_LOCATION).exists(),
        "lockdown": lockdown,
        "power_state": text(&root.join("sys/power/state")),
    })
}

/// Finding codes, most important first. "ready" only when nothing blocks.
pub fn analyse(r: &Value) -> Vec<Value> {
    let mut out = Vec::new();
    if !r["power_state"].as_str().is_some_and(|s| s.split_whitespace().any(|w| w == "disk")) {
        out.push(finding("no_disk_state", "warn", Value::Null));
    }
    if let Some(l) = r["lockdown"].as_str().filter(|l| *l != "none") {
        out.push(finding("lockdown", "warn", json!(l)));
    }
    let swaps = r["swaps"].as_array().cloned().unwrap_or_default();
    let disk: Vec<&Value> = swaps.iter().filter(|s| s["zram"] != true).collect();
    let swap_kib: u64 = disk.iter().filter_map(|s| s["kib"].as_u64()).sum();
    let zram = swaps.iter().any(|s| s["zram"] == true);
    if disk.is_empty() {
        out.push(finding("no_swap", "warn", json!({"zram": zram})));
    } else if let Some(mem) = r["mem_kib"].as_u64().filter(|&m| swap_kib < m) {
        out.push(finding("swap_small", "warn", json!({"swap_kib": swap_kib, "mem_kib": mem})));
    }
    if r["resume"].is_null() && r["hibernate_location"] != true {
        out.push(finding("no_resume", "warn", Value::Null));
    }
    for s in &disk {
        match s["crypt"].as_str() {
            Some("plain") => out.push(finding("swap_plain_crypt", "warn", s["name"].clone())),
            Some("luks") => out.push(finding("swap_luks", "info", s["name"].clone())),
            _ => {}
        }
    }
    if !out.iter().any(|f| f["severity"] == "warn") {
        out.push(finding("ready", "info", Value::Null));
    }
    out
}

fn finding(code: &str, severity: &str, detail: Value) -> Value {
    json!({"code": code, "severity": severity, "detail": detail})
}

/// tune-profile-helper op "hibernate_check": {"ok", "readings", "findings"}. Read-only.
pub fn run(root: &Path) -> Value {
    let r = readings(root);
    let findings = analyse(&r);
    json!({"ok": true, "readings": r, "findings": findings})
}

#[cfg(test)]
mod tests {
    use super::*;

    fn codes(r: &Value) -> Vec<String> {
        analyse(r).iter().map(|f| f["code"].as_str().unwrap().to_owned()).collect()
    }

    fn put(root: &Path, rel: &str, body: &str) {
        let p = root.join(rel);
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(p, body).unwrap();
    }

    #[test]
    fn a_ready_machine() {
        let dir = std::env::temp_dir().join(format!("centurion-hib-ok-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        put(&dir, "proc/meminfo", "MemTotal:       16000000 kB\nSwapTotal:      20000000 kB\n");
        put(&dir, "proc/swaps", "Filename\tType\tSize\tUsed\tPriority\n/dev/zram0 partition 8000000 0 100\n/dev/nvme0n1p3 partition 20000000 0 -2\n");
        put(&dir, "proc/cmdline", "root=/dev/nvme0n1p2 resume=UUID=abcd quiet\n");
        put(&dir, "sys/kernel/security/lockdown", "[none] integrity confidentiality\n");
        put(&dir, "sys/power/state", "freeze mem disk\n");
        let r = readings(&dir);
        assert_eq!(r["resume"], "UUID=abcd");
        assert_eq!(r["lockdown"], "none");
        assert_eq!(codes(&r), ["ready"]);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn what_blocks_a_hibernate() {
        let dir = std::env::temp_dir().join(format!("centurion-hib-bad-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        put(&dir, "proc/meminfo", "MemTotal:       16000000 kB\n");
        put(&dir, "proc/swaps", "Filename\tType\tSize\tUsed\tPriority\n/dev/zram0 partition 8000000 0 100\n");
        put(&dir, "proc/cmdline", "quiet\n");
        put(&dir, "sys/kernel/security/lockdown", "none [integrity] confidentiality\n");
        put(&dir, "sys/power/state", "freeze mem\n");
        // zram alone is no swap for hibernation; no resume= and no HibernateLocation.
        assert_eq!(codes(&readings(&dir)), ["no_disk_state", "lockdown", "no_swap", "no_resume"]);
        // a small swap on a random-key dm-crypt mapping (found by its /dev/mapper name)
        put(&dir, "proc/swaps", "Filename\tType\tSize\tUsed\tPriority\n/dev/mapper/cswap partition 4000000 0 -2\n");
        put(&dir, "sys/block/dm-1/dm/name", "cswap\n");
        put(&dir, "sys/block/dm-1/dm/uuid", "CRYPT-PLAIN-cswap\n");
        put(&dir, HIBERNATE_LOCATION, "x");
        let c = codes(&readings(&dir));
        assert!(c.contains(&"swap_small".into()) && c.contains(&"swap_plain_crypt".into()) && !c.contains(&"no_resume".into()), "{c:?}");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn nothing_readable_is_not_ready() {
        let c = codes(&readings(Path::new("/nonexistent-centurion")));
        assert!(c.contains(&"no_disk_state".into()) && !c.contains(&"ready".into()));
    }
}
