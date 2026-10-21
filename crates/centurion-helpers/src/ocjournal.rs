//! Curve journal: which CPU / GPU voltage-frequency curve was in effect from when, per boot.
//!
//! The GUI's Curve stability page (Health) lays NVIDIA Xid and machine-check events from the saved
//! kernel-error log over these intervals, so an unstable curve shows up as "N errors in H hours with
//! profile X" instead of a guess. Root helpers append one line per successful curve write:
//!
//!   /var/lib/centurion/oc-journal.jsonl   (root-owned, world-readable, JSON lines)
//!   {"t": epoch s, "boot": boot_id, "kind": "gpu_curve" | "cpu_co" | "cpu_uv", "state": "reset" | "profile:NAME" | …}
//!
//! A reboot resets every curve (the SMU, the MSRs and the NVIDIA driver forget them), so an interval
//! ends at the next line of its kind or at the end of its boot. The same format is written by
//! nvcurve-root-helper (crates/nvcurve/src/ocjournal.rs). Never fatal: a failed write is ignored.

use std::io::Write;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};

pub const FILE: &str = "/var/lib/centurion/oc-journal.jsonl";
const MAX_BYTES: u64 = 256 * 1024;
const KEEP_BYTES: usize = 128 * 1024;

pub fn record(kind: &str, state: &str) {
    if unsafe { libc::geteuid() } != 0 { return; }
    let _ = std::fs::DirBuilder::new().mode(0o755).recursive(true).create("/var/lib/centurion");
    // Rotation: keep the newest half (whole lines) once the file passes MAX_BYTES.
    if std::fs::symlink_metadata(FILE).map_or(false, |m| m.file_type().is_file() && m.len() > MAX_BYTES) {
        if let Ok(old) = std::fs::read(FILE) {
            let cut = old.len().saturating_sub(KEEP_BYTES);
            let start = old[cut..].iter().position(|b| *b == b'\n').map_or(old.len(), |i| cut + i + 1);
            let tmp = format!("{FILE}.tmp{}", std::process::id());
            if std::fs::write(&tmp, &old[start..]).is_ok() { let _ = std::fs::rename(&tmp, FILE); }
        }
    }
    let line = serde_json::json!({
        "t": std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0),
        "boot": crate::bootguard::boot_id(), "kind": kind, "state": state,
    });
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).mode(0o644)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(FILE) {
        let _ = writeln!(f, "{line}");
    }
}
