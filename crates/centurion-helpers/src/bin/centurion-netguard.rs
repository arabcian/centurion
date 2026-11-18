//! centurion-netguard — network guard daemon of Centurion (see netguard.rs).
//!
//!   centurion-netguard daemon    enforce the IP blacklist and block non-whitelisted
//!                          Wine/.exe programs (root; OpenRC/systemd service)
//!   centurion-netguard status    configuration + daemon state (JSON)
//!   centurion-netguard list      current TCP/UDP sockets with their processes (JSON)
//!   centurion-netguard whois X   registry record of an address, CIDR block, AS123 or domain (JSON)

use centurion_helpers::netguard;

fn main() {
    centurion_helpers::init();
    let code = match std::env::args().nth(1).as_deref() {
        Some("daemon") => netguard::run_daemon(),
        Some("status") => centurion_helpers::finish(netguard::status(true)),
        Some("list") => centurion_helpers::finish(netguard::list()),
        Some("whois") => centurion_helpers::finish(netguard::lookup(&std::env::args().nth(2).unwrap_or_default(), true)),
        _ => { eprintln!("usage: centurion-netguard daemon | status | list | whois IP|CIDR|ASn|DOMAIN"); 2 }
    };
    std::process::exit(code);
}
