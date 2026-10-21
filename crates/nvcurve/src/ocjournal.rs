//! Curve journal line for the NVIDIA curve — same file and format as
//! centurion_helpers::ocjournal (see there): {"t", "boot", "kind": "gpu_curve", "state"}.
//! Root only, never fatal.

use std::io::Write;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};

pub const FILE: &str = "/var/lib/centurion/oc-journal.jsonl";
const MAX_BYTES: u64 = 256 * 1024;
const KEEP_BYTES: usize = 128 * 1024;

pub fn record(state: &str) {
    if unsafe { libc::geteuid() } != 0 { return; }
    let _ = std::fs::DirBuilder::new().mode(0o755).recursive(true).create("/var/lib/centurion");
    if std::fs::symlink_metadata(FILE).map_or(false, |m| m.file_type().is_file() && m.len() > MAX_BYTES) {
        if let Ok(old) = std::fs::read(FILE) {
            let cut = old.len().saturating_sub(KEEP_BYTES);
            let start = old[cut..].iter().position(|b| *b == b'\n').map_or(old.len(), |i| cut + i + 1);
            let tmp = format!("{FILE}.tmp{}", std::process::id());
            if std::fs::write(&tmp, &old[start..]).is_ok() { let _ = std::fs::rename(&tmp, FILE); }
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
