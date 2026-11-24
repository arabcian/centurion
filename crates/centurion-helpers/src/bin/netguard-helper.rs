//! Helper of Health → Network (same contract as the other helpers: one JSON
//! request on stdin, one JSON line on stdout).
//!   any user:  {"op":"list"}  {"op":"status"}  {"op":"wifi"}
//!     {"op":"whois", "query": "address | cidr | AS123 | domain", "asn": bool}   asn: add routing data
//!   root (pkexec):
//!     {"op":"set", "guard": bool, "allow_lan": bool, "log_conns": bool, "traffic": bool}
//!     {"op":"blacklist", "add": ["ip | cidr | first - last"], "label": "group", "remove": [...], "remove_all": bool,
//!      "replace": bool, "public_only": bool, "drop_groups": ["group"]}
//!                         replace: the group's old entries are dropped first (a subscription update)
//!                         public_only: entries reaching local or reserved addresses are skipped (a published list)
//!                         drop_groups: those groups let go of their entries; one another group still lists stays
//!                                                                live sockets to added addresses are aborted
//!     {"op":"whitelist", "add": ["path | dir/ | name.exe"], "remove": [...]}
//!     {"op":"kill", "proto": "tcp|udp", "lport": n, "remote": "ip", "rport": n}
//!     {"op":"firewall", "enabled": bool, "inbound"|"outbound": "allow|block|reject", "ping_in"|"ping_out": bool,
//!      "add": [rule], "front": bool, "update": [rule with id], "remove": [id], "move": {"id": n, "by": ±1},
//!      "port": {"proto": "tcp|udp", "port": n, "state": "open|closed|lan|default"}}   every field optional
//!       optional expected_firewall: exact saved configuration snapshot; stale requests are refused under the lock.
//!       rule: {"dir": "in|out", "action": "allow|block|reject", "proto": "tcp|udp|any", "ports": "80, 8000-8100",
//!              "remote": "lan | address, cidr, range", "iface": "wg*", "note": text, "enabled": bool}
//!     {"op":"fw_inspect"}   the live table, hit counters and other filter chains on the same hooks
//!     {"op":"clear_log"}  {"op":"clear_conn_log"}
//! Rule changes are written to /etc/centurion/netguard.json and the
//! running daemon is told to reload (SIGHUP).

use centurion_helpers::*;

fn main() {
    init();
    // A blocklist subscription may be 8 MiB of text (BLOCKLIST_MAX_BYTES), plus its JSON quoting.
    let out = match read_request(16 << 20) {
        Ok(req) => netguard::handle(&req),
        Err(e) => e,
    };
    std::process::exit(finish(out));
}
