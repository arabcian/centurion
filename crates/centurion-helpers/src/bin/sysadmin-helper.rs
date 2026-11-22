//! System changes from Health → System tools and the Health pages (polkit com.centurion.sysadmin:
//! the administrator password, then polkit's keep window). The request format and every check are
//! in centurion_helpers::sysadmin; this binary only reads, dispatches and answers.

use centurion_helpers::*;

/// A configuration file of at most 256 KiB, JSON-escaped.
const MAX_STDIN_BYTES: usize = 1 << 20;

fn main() {
    init();
    // fstrim over every filesystem may take minutes; everything else returns in seconds.
    watchdog(1000);
    let reply = match read_request(MAX_STDIN_BYTES) {
        Ok(req) => sysadmin::handle(&req),
        Err(e) => e,
    };
    std::process::exit(finish(reply));
}
