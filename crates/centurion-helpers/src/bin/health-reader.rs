//! health-reader — root-only, read-only kernel-log scan for the Health tab.
//!
//! With kernel.dmesg_restrict=1 the GUI cannot read /dev/kmsg itself. It used
//! to go through tune-profile-helper for that, which asks for the password at
//! security levels 2/3 — at start-up and every five minutes, with the window
//! hidden in the tray. This helper can do exactly one thing: return the
//! *classified* hardware events (NVIDIA Xid, MCE, AER, lockups…) newer than a
//! sequence number, plus the AER counters. The scan never returns raw log lines
//! and writes nothing, so its polkit action is silent for the active local
//! session at every level (like the sensor streams).
//!
//! stdin:  {"since": N}          (optional; default 0)
//! stdout: one JSON line, see centurion_helpers::health::scan.
//!
//! stdin:  {"op": "boots"}       Health → Boot history: every boot with how it ended, its events and
//!                               curve states, plus the pstore records of past crashes (root-only
//!                               files; their text is clipped and kernel addresses are scrubbed, the
//!                               same `health::scrub` the Kernel log page applies). Also read-only.
//!                               See centurion_helpers::boothist::build.
//!
//! stdin:  {"op": "sleep_diag"}  Health → Sleep → "Why doesn't the hardware sleep?": fixed sysfs and
//!                               debugfs paths (the PMC counters need root), findings as codes. Read-only.
//!                               See centurion_helpers::sleepdiag::run.
//!
//! stdin:  {"op": "battery_info"} Health → Battery: maker, hardware id and manufacture date from the
//!                               firmware's WMI data block (one query per field through acpi_call; no
//!                               module is loaded from here). See centurion_helpers::legion_wmi::battery_info.

fn main() {
    centurion_helpers::init();
    let req = centurion_helpers::read_request(4096).unwrap_or(serde_json::Value::Null);
    if req["op"].as_str() == Some("boots") {
        std::process::exit(centurion_helpers::finish(centurion_helpers::boothist::load()));
    }
    if req["op"].as_str() == Some("sleep_diag") {
        std::process::exit(centurion_helpers::finish(centurion_helpers::sleepdiag::run(std::path::Path::new("/"))));
    }
    if req["op"].as_str() == Some("battery_info") {
        std::process::exit(centurion_helpers::finish(centurion_helpers::legion_wmi::battery_info()));
    }
    let since = req["since"].as_u64().unwrap_or(0);
    std::process::exit(centurion_helpers::finish(centurion_helpers::health::scan(since)));
}
