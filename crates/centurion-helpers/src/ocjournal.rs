//! Curve journal: which CPU / GPU voltage-frequency curve was in effect from when, per boot.
//!
//! The GUI's Curve stability page (Health) lays NVIDIA Xid and machine-check events from the saved
//! kernel-error log over these intervals, so an unstable curve shows up as "N errors in H hours with
//! profile X" instead of a guess. Root helpers append one line per successful curve write:
//!
//!   /var/lib/centurion/oc-journal.jsonl   (root-owned, world-readable, JSON lines)
//!   {"t": epoch s, "boot": boot_id, "kind": "gpu_curve" | "cpu_co" | "cpu_uv", "state": "reset" | "profile:NAME" | …,
//!    "values"?: what was written (Curve Optimizer: {"ccd.ccx.core": offset} or {"all": offset}), "fp"?: 8 hex digits}
//!
//! "values" and "fp" (a fingerprint of the values) tell two curves with the same name apart: a profile
//! edited and applied again under its old name gets another fp. Lines without them come from older versions.
//!
//! A reboot resets every curve (the SMU, the MSRs and the NVIDIA driver forget them), so an interval
//! ends at the next line of its kind or at the end of its boot. The same format is written by
//! nvcurve-root-helper (crates/nvcurve/src/ocjournal.rs). Never fatal: a failed write is ignored.

use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;

pub const FILE: &str = "/var/lib/centurion/oc-journal.jsonl";
const MAX_BYTES: u64 = 256 * 1024;
const KEEP_BYTES: usize = 128 * 1024;

/// FNV-1a 64 of the compact JSON (serde_json's map is sorted, so equal values give equal text), 8 hex digits.
pub fn fingerprint(values: &serde_json::Value) -> String {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in values.to_string().bytes() { h ^= u64::from(b); h = h.wrapping_mul(0x0100_0000_01b3); }
    format!("{:08x}", (h >> 32) as u32 ^ h as u32)
}

pub fn record(kind: &str, state: &str) { record_values(kind, state, None) }

/// A journal line with the values written. Values over 8 KiB keep only their fingerprint.
pub fn record_values(kind: &str, state: &str, values: Option<&serde_json::Value>) {
    if unsafe { libc::geteuid() } != 0 { return; }
    // The directory must be root-owned and not writable by others: a user-owned one would let that user
    // plant symlinks that the rotation below would follow as root.
    if crate::secure_dir("/var/lib/centurion").is_err() { return; }
    // Rotation: keep the newest half (whole lines) once the file passes MAX_BYTES.
    if std::fs::symlink_metadata(FILE).is_ok_and(|m| m.file_type().is_file() && m.len() > MAX_BYTES) {
        if let Some(old) = crate::read_root_file(FILE, 4 * MAX_BYTES) {
            let old = old.as_bytes();
            let cut = old.len().saturating_sub(KEEP_BYTES);
            let start = old[cut..].iter().position(|b| *b == b'\n').map_or(old.len(), |i| cut + i + 1);
            let _ = crate::write_root_file(FILE, &old[start..]);  // O_EXCL tmp, atomic rename
        }
    }
    let mut line = serde_json::json!({
        "t": std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0),
        "boot": crate::bootguard::boot_id(), "kind": kind, "state": state,
    });
    if let Some(v) = values.filter(|v| !v.is_null()) {
        line["fp"] = serde_json::json!(fingerprint(v));
        if v.to_string().len() <= 8192 { line["values"] = v.clone(); }
    }
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).mode(0o644)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(FILE) {
        let _ = writeln!(f, "{line}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fingerprint_tells_values_apart() {
        let a = serde_json::json!({"0.0.1": -20, "0.0.0": -25});
        let b = serde_json::json!({"0.0.0": -25, "0.0.1": -20});
        let c = serde_json::json!({"0.0.0": -25, "0.0.1": -15});
        assert_eq!(fingerprint(&a), fingerprint(&b));
        assert_ne!(fingerprint(&a), fingerprint(&c));
        assert_eq!(fingerprint(&a).len(), 8);
    }
}
