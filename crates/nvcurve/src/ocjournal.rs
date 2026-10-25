//! Curve journal line for the NVIDIA curve — same file and format as
//! centurion_helpers::ocjournal (see there): {"t", "boot", "kind": "gpu_curve", "state"}.
//! Root only, never fatal.

use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;

pub const FILE: &str = "/var/lib/centurion/oc-journal.jsonl";
const MAX_BYTES: u64 = 256 * 1024;
const KEEP_BYTES: usize = 128 * 1024;

pub fn record(state: &str) {
    if unsafe { libc::geteuid() } != 0 { return; }
    // Root-owned directory only (ensure_dir refuses any other owner), then an atomic O_EXCL tmp + rename:
    // a user-owned directory must not be able to redirect the rotation through a planted symlink.
    if crate::atomicio::ensure_dir(std::path::Path::new("/var/lib/centurion"), 0o755).is_err() { return; }
    if std::fs::symlink_metadata(FILE).map_or(false, |m| m.file_type().is_file() && m.len() > MAX_BYTES) {
        if let Ok(old) = crate::atomicio::read_regular(std::path::Path::new(FILE), 4 * MAX_BYTES) {
            let old = old.as_bytes();
            let cut = old.len().saturating_sub(KEEP_BYTES);
            let start = old[cut..].iter().position(|b| *b == b'\n').map_or(old.len(), |i| cut + i + 1);
            let _ = crate::atomicio::write_bytes(std::path::Path::new(FILE), &old[start..], 0o644);
        }
    }
    let boot = std::fs::read_to_string("/proc/sys/kernel/random/boot_id").map(|s| s.trim().to_owned()).unwrap_or_default();
    let line = serde_json::json!({
        "t": std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0),
        "boot": boot, "kind": "gpu_curve", "state": state,
    });
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).mode(0o644)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(FILE) {
        let _ = writeln!(f, "{line}");
    }
}
