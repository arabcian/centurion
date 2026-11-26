//! System changes from Health → System tools and the Health pages. The request format and every check are
//! in centurion_helpers::sysadmin; this binary only reads, dispatches and answers.
//!
//! Built twice from this file (Cargo.toml), role fixed by CARGO_BIN_NAME, because polkit pins one action per
//! binary path:
//!   sysadmin-helper         runtime actions (TRIM, drop caches, SMART test, rfkill, runtime PM, sysfs knobs) and
//!                           the backup list: com.centurion.sysadmin, the password then polkit's keep window
//!   sysadmin-config-helper  sysadmin::CONFIG_OPS: configuration files, module load/unload, NVIDIA driver options.
//!                           These can run a command as root (modprobe install, udev RUN, fstab), so
//!                           com.centurion.sysadmin.config asks for the password every time (no keep window).

use centurion_helpers::*;
use serde_json::json;

/// A configuration file of at most 256 KiB, JSON-escaped.
const MAX_STDIN_BYTES: usize = 1 << 20;

fn config_role() -> bool { env!("CARGO_BIN_NAME") == "sysadmin-config-helper" }

pub(crate) fn main() {
    init();
    // fstrim over every filesystem may take minutes; everything else returns in seconds.
    watchdog(1000);
    let reply = match read_request(MAX_STDIN_BYTES) {
        Ok(req) => {
            let op = req.get("op").and_then(|v| v.as_str()).unwrap_or("");
            let config_op = sysadmin::CONFIG_OPS.contains(&op);
            if config_role() && !config_op && op != "config_backups" {
                json!({"ok": false, "error": format!("{op} is handled by sysadmin-helper")})
            } else if !config_role() && config_op {
                json!({"ok": false, "error": format!("{op} is handled by sysadmin-config-helper (administrator password every time)")})
            } else {
                sysadmin::handle(&req)
            }
        }
        Err(e) => e,
    };
    std::process::exit(finish(reply));
}
