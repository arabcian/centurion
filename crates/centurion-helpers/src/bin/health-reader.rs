//! health-reader — root-only, read-only kernel-log scan for the Health tab.
//!
//! With kernel.dmesg_restrict=1 the GUI cannot read /dev/kmsg itself. It used
//! to go through tune-profile-helper for that, which asks for the password at
//! security levels 2/3 — at start-up and every five minutes, with the window
//! hidden in the tray. This helper can do exactly one thing: return the
//! *classified* hardware events (NVIDIA Xid, MCE, AER, lockups…) newer than a
//! sequence number, plus the AER counters. It never returns raw log lines and
//! writes nothing, so its polkit action is silent for the active local session
//! at every level (like the sensor streams).
//!
//! stdin:  {"since": N}          (optional; default 0)
//! stdout: one JSON line, see centurion_helpers::health::scan.

fn main() {
    centurion_helpers::init();
    let since = centurion_helpers::read_request(4096).ok().and_then(|v| v["since"].as_u64()).unwrap_or(0);
    std::process::exit(centurion_helpers::finish(centurion_helpers::health::scan(since)));
}
