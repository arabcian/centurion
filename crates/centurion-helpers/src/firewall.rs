//! Firewall of the network guard (Health → Network → Firewall): what this machine accepts and sends
//! by default, plus an ordered list of port rules. Part of netguard's nftables table, so it is applied,
//! reloaded and removed together with the blacklist.
//!
//! Order of a packet through the table:
//!   loopback accept → blacklist drop → established/related accept → `fw_in` / `fw_out` → guard and log queue
//! A firewall "allow" is a `return` from `fw_*`, not an accept: the packet still reaches the Wine guard
//! and the connection log. The first matching rule decides, the default policy comes last.
//!
//! Limits worth knowing: only traffic of this machine is filtered (input/output, not forward), and an
//! allow here cannot override a drop in another nftables table (`inspect` in netguard.rs reports those).
//! The text of a rule never reaches nft unparsed: every field is validated into a typed value first.

use crate::netguard::{is_lan, parse_nets, Net};
use serde_json::{json, Value};
use std::net::IpAddr;

pub const MAX_RULES: usize = 512;
const MAX_PORT_ITEMS: usize = 32;
const MAX_NETS: usize = 64;
const TCP: u8 = 6;

// Not 100.64.0.0/10: carrier-grade NAT puts other customers of the provider there.
const LAN4: &str = "10.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.168.0.0/16";
const LAN6: &str = "fc00::/7, fe80::/10";
const MCAST4: &str = "224.0.0.0/4, 255.255.255.255";
const MCAST6: &str = "ff00::/8";

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Verdict { Allow, Block, Reject }

impl Verdict {
    fn parse(s: &str) -> Option<Verdict> {
        match s { "allow" => Some(Verdict::Allow), "block" => Some(Verdict::Block), "reject" => Some(Verdict::Reject), _ => None }
    }
    pub fn name(self) -> &'static str {
        match self { Verdict::Allow => "allow", Verdict::Block => "block", Verdict::Reject => "reject" }
    }
    fn nft(self) -> &'static str {
        match self { Verdict::Allow => "return", Verdict::Block => "jump fw_drop", Verdict::Reject => "jump fw_reject" }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Proto { Tcp, Udp, Any }

impl Proto {
    fn parse(s: &str) -> Option<Proto> {
        match s { "tcp" => Some(Proto::Tcp), "udp" => Some(Proto::Udp), "any" => Some(Proto::Any), _ => None }
    }
    fn name(self) -> &'static str {
        match self { Proto::Tcp => "tcp", Proto::Udp => "udp", Proto::Any => "any" }
    }
    fn covers(self, other: Proto) -> bool { self == Proto::Any || self == other }
    fn matches(self, is_tcp: bool) -> bool {
        match self { Proto::Any => true, Proto::Tcp => is_tcp, Proto::Udp => !is_tcp }
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Remote { Any, Lan, Nets(Vec<Net>) }

impl Remote {
    fn parse(s: &str) -> Result<Remote, String> {
        let s = s.trim();
        if s.is_empty() || s == "any" { return Ok(Remote::Any); }
        if s == "lan" { return Ok(Remote::Lan); }
        let mut nets: Vec<Net> = Vec::new();
        for item in s.split(',').map(str::trim).filter(|i| !i.is_empty()) {
            let parsed = parse_nets(item).ok_or_else(|| format!("not an address, CIDR block or range: {item}"))?;
            for n in parsed { if !nets.contains(&n) { nets.push(n); } }
        }
        if nets.is_empty() { return Ok(Remote::Any); }
        if nets.len() > MAX_NETS { return Err(format!("at most {MAX_NETS} address blocks per rule")); }
        Ok(Remote::Nets(nets))
    }
    fn text(&self) -> String {
        match self {
            Remote::Any => String::new(),
            Remote::Lan => "lan".to_owned(),
            Remote::Nets(n) => n.iter().map(Net::to_string).collect::<Vec<_>>().join(", "),
        }
    }
    fn matches(&self, ip: IpAddr) -> bool {
        match self { Remote::Any => true, Remote::Lan => is_lan(ip) && !is_cgnat(ip), Remote::Nets(n) => n.iter().any(|x| x.contains(ip)) }
    }
}

fn is_cgnat(ip: IpAddr) -> bool {
    matches!(ip, IpAddr::V4(v) if v.octets()[0] == 100 && v.octets()[1] & 0xc0 == 64)
}

/// "80, 443, 8000-8100" → sorted ranges with overlaps merged (nft refuses overlapping set elements).
fn parse_ports(s: &str) -> Result<Vec<(u16, u16)>, String> {
    let mut out: Vec<(u16, u16)> = Vec::new();
    for item in s.split(',').map(str::trim).filter(|i| !i.is_empty()) {
        let num = |t: &str| t.trim().parse::<u16>().ok().filter(|&p| p != 0).ok_or_else(|| format!("not a port number: {item}"));
        let (lo, hi) = match item.split_once('-') { Some((a, b)) => (num(a)?, num(b)?), None => { let p = num(item)?; (p, p) } };
        if lo > hi { return Err(format!("port range is backwards: {item}")); }
        out.push((lo, hi));
    }
    if out.len() > MAX_PORT_ITEMS { return Err(format!("at most {MAX_PORT_ITEMS} ports or ranges per rule")); }
    out.sort_unstable();
    let mut merged: Vec<(u16, u16)> = Vec::with_capacity(out.len());
    for (lo, hi) in out {
        match merged.last_mut() {
            Some(l) if u32::from(lo) <= u32::from(l.1) + 1 => l.1 = l.1.max(hi),
            _ => merged.push((lo, hi)),
        }
    }
    Ok(merged)
}

fn ports_text(p: &[(u16, u16)]) -> String {
    p.iter().map(|&(a, b)| if a == b { a.to_string() } else { format!("{a}-{b}") }).collect::<Vec<_>>().join(", ")
}

/// A kernel interface name, optionally ending in `*` (`wg*`). Goes into a quoted nft string.
fn valid_iface(s: &str) -> bool {
    let name = s.strip_suffix('*').unwrap_or(s);
    !name.is_empty() && s.len() <= 15 && name != "lo" && name.bytes().all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b))
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Rule {
    pub id: u32,
    pub enabled: bool,
    pub inbound: bool,
    pub action: Verdict,
    pub proto: Proto,
    pub ports: Vec<(u16, u16)>,
    pub remote: Remote,
    pub iface: String,
    pub note: String,
}

impl Rule {
    pub fn from_json(v: &Value, id: u32) -> Result<Rule, String> {
        let field = |k: &str| v[k].as_str().unwrap_or("").trim();
        let inbound = match field("dir") { "in" => true, "out" => false, d => return Err(format!("unknown direction: {d}")) };
        let action = Verdict::parse(field("action")).ok_or_else(|| format!("unknown action: {}", field("action")))?;
        let proto = Proto::parse(field("proto")).ok_or_else(|| format!("unknown protocol: {}", field("proto")))?;
        let ports = parse_ports(field("ports"))?;
        let remote = Remote::parse(field("remote"))?;
        let iface = field("iface").to_owned();
        if !iface.is_empty() && !valid_iface(&iface) { return Err(format!("not a network interface name: {iface}")); }
        if proto == Proto::Any && ports.is_empty() && remote == Remote::Any && iface.is_empty() {
            return Err("this rule matches everything: set the default policy instead".to_owned());
        }
        let note: String = field("note").chars().filter(|c| !c.is_control()).take(120).collect();
        Ok(Rule { id, enabled: v["enabled"].as_bool().unwrap_or(true), inbound, action, proto, ports, remote, iface, note })
    }

    pub fn to_json(&self) -> Value {
        json!({"id": self.id, "enabled": self.enabled, "dir": if self.inbound { "in" } else { "out" }, "action": self.action.name(),
               "proto": self.proto.name(), "ports": ports_text(&self.ports), "remote": self.remote.text(), "iface": self.iface, "note": self.note})
    }

    /// What the rule does, without its id and note: two rules with the same signature are the same rule.
    fn signature(&self) -> String {
        format!("{}|{}|{}|{}|{}|{}", self.inbound, self.action.name(), self.proto.name(), ports_text(&self.ports), self.remote.text(), self.iface)
    }

    fn port_matches(&self, port: u16) -> bool { self.ports.is_empty() || self.ports.iter().any(|&(a, b)| (a..=b).contains(&port)) }

    /// A connection of this direction. Rules bound to an interface cannot be judged from a socket.
    fn matches(&self, inbound: bool, is_tcp: bool, port: u16, remote: IpAddr) -> bool {
        self.enabled && self.inbound == inbound && self.iface.is_empty() && self.proto.matches(is_tcp) && self.port_matches(port) && self.remote.matches(remote)
    }

    /// Every packet this rule matches is also matched by `self`.
    fn covers(&self, o: &Rule) -> bool {
        self.inbound == o.inbound && self.proto.covers(o.proto) && (self.iface.is_empty() || self.iface == o.iface)
            && (self.remote == Remote::Any || self.remote == o.remote)
            && (self.ports.is_empty() || (!o.ports.is_empty() && o.ports.iter().all(|&(a, b)| self.ports.iter().any(|&(x, y)| x <= a && b <= y))))
    }

    /// One nft rule per address family the rule names. `forwarded`: for a connection DNAT sent on to a
    /// container or VM, whose port has already been rewritten: the port it was addressed to is matched.
    fn nft_lines(&self, forwarded: bool) -> Vec<String> {
        let dir = if self.inbound { "saddr" } else { "daddr" };
        let set = |fam: &str, elems: String| format!("{fam} {dir} {{ {elems} }} ");
        let addrs: Vec<String> = match &self.remote {
            Remote::Any => vec![String::new()],
            Remote::Lan if self.inbound => vec![set("ip", LAN4.to_owned()), set("ip6", LAN6.to_owned())],
            Remote::Lan => vec![set("ip", format!("{LAN4}, {MCAST4}")), set("ip6", format!("{LAN6}, {MCAST6}"))],
            Remote::Nets(nets) => {
                let of = |v4: bool| nets.iter().filter(|n| n.is_ipv4() == v4).map(Net::to_string).collect::<Vec<_>>().join(", ");
                let (v4, v6) = (of(true), of(false));
                [("ip", v4), ("ip6", v6)].into_iter().filter(|(_, e)| !e.is_empty()).map(|(f, e)| set(f, e)).collect()
            }
        };
        let ports = ports_text(&self.ports);
        let l4 = match (self.proto, ports.is_empty()) {
            (Proto::Any, false) if forwarded => format!("meta l4proto {{ tcp, udp }} ct original proto-dst {{ {ports} }} "),
            (p, false) if forwarded => format!("meta l4proto {} ct original proto-dst {{ {ports} }} ", p.name()),
            (Proto::Any, true) => String::new(),
            (Proto::Any, false) => format!("meta l4proto {{ tcp, udp }} th dport {{ {ports} }} "),
            (p, true) => format!("meta l4proto {} ", p.name()),
            (p, false) => format!("{} dport {{ {ports} }} ", p.name()),
        };
        let iface = if self.iface.is_empty() { String::new() } else { format!("{} \"{}\" ", if self.inbound { "iifname" } else { "oifname" }, self.iface) };
        addrs.iter().map(|a| format!("\t\t{iface}{a}{l4}counter {} comment \"r{}\"\n", self.action.nft(), self.id)).collect()
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Firewall {
    pub enabled: bool,
    pub inbound: Verdict,
    pub outbound: Verdict,
    pub ping_in: bool,
    pub ping_out: bool,
    pub rules: Vec<Rule>,
}

impl Default for Firewall {
    fn default() -> Self {
        Firewall { enabled: false, inbound: Verdict::Allow, outbound: Verdict::Allow, ping_in: true, ping_out: true, rules: Vec::new() }
    }
}

impl Firewall {
    pub fn from_json(v: &Value) -> Result<Firewall, String> {
        if v.is_null() { return Ok(Firewall::default()); }
        let d = Firewall::default();
        let policy = |k: &str, def: Verdict| match v[k].as_str() {
            None => Ok(def),
            Some(s) => Verdict::parse(s).ok_or_else(|| format!("firewall: unknown policy {s}")),
        };
        let mut rules = Vec::new();
        for r in v["rules"].as_array().map_or(&[][..], Vec::as_slice).iter().take(MAX_RULES) {
            let id = r["id"].as_u64().and_then(|i| u32::try_from(i).ok()).filter(|&i| i != 0).ok_or("firewall: rule without an id")?;
            rules.push(Rule::from_json(r, id).map_err(|e| format!("firewall rule {id}: {e}"))?);
        }
        Ok(Firewall { enabled: v["enabled"].as_bool().unwrap_or(d.enabled), inbound: policy("inbound", d.inbound)?, outbound: policy("outbound", d.outbound)?,
                      ping_in: v["ping_in"].as_bool().unwrap_or(d.ping_in), ping_out: v["ping_out"].as_bool().unwrap_or(d.ping_out), rules })
    }

    pub fn to_json(&self) -> Value {
        json!({"enabled": self.enabled, "inbound": self.inbound.name(), "outbound": self.outbound.name(), "ping_in": self.ping_in, "ping_out": self.ping_out,
               "rules": self.rules.iter().map(Rule::to_json).collect::<Vec<_>>()})
    }

    /// Anything to enforce? With the defaults there are no chains at all and the table stays as it always was.
    pub fn active(&self) -> bool {
        self.enabled && (self.inbound != Verdict::Allow || self.outbound != Verdict::Allow || !self.ping_in || !self.ping_out || self.rules.iter().any(|r| r.enabled))
    }

    pub fn uses_reject(&self) -> bool {
        self.active() && (self.inbound == Verdict::Reject || self.outbound == Verdict::Reject || self.rules.iter().any(|r| r.enabled && r.action == Verdict::Reject))
    }

    fn next_id(&self) -> u32 { self.rules.iter().map(|r| r.id).max().unwrap_or(0).saturating_add(1) }

    fn pos(&self, id: u64) -> Result<usize, String> {
        self.rules.iter().position(|r| u64::from(r.id) == id).ok_or_else(|| format!("no firewall rule {id}"))
    }

    /// Applies one `firewall` request of the helper. Nothing is kept when it fails: the caller saves only on Ok.
    pub fn apply(&mut self, req: &Value) -> Result<(), String> {
        if let Some(b) = req["enabled"].as_bool() { self.enabled = b; }
        if let Some(b) = req["ping_in"].as_bool() { self.ping_in = b; }
        if let Some(b) = req["ping_out"].as_bool() { self.ping_out = b; }
        for (k, slot) in [("inbound", &mut self.inbound), ("outbound", &mut self.outbound)] {
            if let Some(s) = req[k].as_str() { *slot = Verdict::parse(s).ok_or_else(|| format!("unknown policy: {s}"))?; }
        }
        for id in req["remove"].as_array().map_or(&[][..], Vec::as_slice).iter().filter_map(Value::as_u64) {
            let i = self.pos(id)?;
            self.rules.remove(i);
        }
        for r in req["update"].as_array().map_or(&[][..], Vec::as_slice) {
            let i = self.pos(r["id"].as_u64().unwrap_or(0))?;
            self.rules[i] = Rule::from_json(r, self.rules[i].id)?;
        }
        let new = req["add"].as_array().map_or(&[][..], Vec::as_slice);
        self.add_rules(new, req["front"] == true)?;
        if !req["port"].is_null() { self.set_port(&req["port"])?; }
        if let Some(id) = req["move"]["id"].as_u64() {
            let i = self.pos(id)?;
            let to = i as i64 + req["move"]["by"].as_i64().unwrap_or(0);
            if to >= 0 && (to as usize) < self.rules.len() { let r = self.rules.remove(i); self.rules.insert(to as usize, r); }
        }
        if self.rules.len() > MAX_RULES { return Err(format!("at most {MAX_RULES} firewall rules")); }
        Ok(())
    }

    /// Adds rules, skipping any that already exist. `front`: before all others, in the given order.
    fn add_rules(&mut self, new: &[Value], front: bool) -> Result<(), String> {
        let mut at = 0;
        for r in new {
            let mut rule = Rule::from_json(r, self.next_id())?;
            match self.rules.iter().position(|x| x.signature() == rule.signature()) {
                // Already there, perhaps switched off: asking for it again means it should work.
                Some(i) if !front => { self.rules[i].enabled = rule.enabled; continue; }
                Some(i) => rule.id = self.rules.remove(i).id,
                None => {}
            }
            if front { self.rules.insert(at, rule); at += 1; } else { self.rules.push(rule); }
        }
        Ok(())
    }

    /// Open ports tab: one inbound port is open, closed, LAN only, or back to the default policy.
    /// Earlier plain rules for exactly this port are replaced, so the new state cannot be shadowed by them.
    fn set_port(&mut self, p: &Value) -> Result<(), String> {
        let proto = match p["proto"].as_str() { Some("tcp") => "tcp", Some("udp") => "udp", _ => return Err("port: protocol must be tcp or udp".to_owned()) };
        let port = p["port"].as_u64().and_then(|n| u16::try_from(n).ok()).filter(|&n| n != 0).ok_or("port: not a port number")?;
        let plain = (Proto::parse(proto).unwrap_or(Proto::Any), vec![(port, port)]);
        self.rules.retain(|r| !(r.inbound && r.iface.is_empty() && r.proto == plain.0 && r.ports == plain.1 && matches!(r.remote, Remote::Any | Remote::Lan)));
        let rule = |action: &str, remote: &str| json!({"dir": "in", "action": action, "proto": proto, "ports": port.to_string(), "remote": remote});
        let add = match p["state"].as_str().unwrap_or("") {
            "open" => vec![rule("allow", "")],
            "closed" => vec![rule("block", "")],
            "lan" => vec![rule("allow", "lan"), rule("block", "")],
            "default" => vec![],
            s => return Err(format!("port: unknown state {s}")),
        };
        self.add_rules(&add, true)
    }

    /// Signatures of the rules that stop traffic right now.
    pub fn blocking(&self) -> Vec<String> {
        if !self.enabled { return Vec::new(); }
        self.rules.iter().filter(|r| r.enabled && r.action != Verdict::Allow).map(Rule::signature).collect()
    }

    /// Would a rule from `fresh` (signatures) be what decides this live connection?
    pub fn newly_blocks(&self, fresh: &[String], inbound: bool, proto: u8, port: u16, remote: IpAddr) -> bool {
        self.rules.iter().find(|r| r.matches(inbound, proto == TCP, port, remote))
            .is_some_and(|r| r.action != Verdict::Allow && fresh.contains(&r.signature()))
    }

    /// The rule that decides a new connection to a local port from a local network or from elsewhere;
    /// `conditional` is set when a narrower rule was passed over on the way.
    fn deciding_rule(&self, is_tcp: bool, port: u16, lan: bool, conditional: &mut bool) -> Option<&Rule> {
        for r in self.rules.iter().filter(|r| r.enabled && r.inbound && r.proto.matches(is_tcp) && r.port_matches(port)) {
            if !r.iface.is_empty() || matches!(r.remote, Remote::Nets(_)) { *conditional = true; continue; }
            if r.remote == Remote::Lan && !lan { continue; }
            return Some(r);
        }
        None
    }

    /// Who can reach a listening port: (open | lan | blocked | off, depends on a narrower rule).
    pub fn listen_state(&self, is_tcp: bool, port: u16) -> (&'static str, bool) {
        if !self.active() { return ("off", false); }
        let mut conditional = false;
        let mut allowed = |lan: bool| self.deciding_rule(is_tcp, port, lan, &mut conditional).map_or(self.inbound, |r| r.action) == Verdict::Allow;
        let (wan, lan) = (allowed(false), allowed(true));
        (if wan { "open" } else if lan { "lan" } else { "blocked" }, conditional)
    }

    /// No rule speaks for this port from somewhere: blocking incoming by default would close it there.
    pub fn relies_on_default(&self, is_tcp: bool, port: u16) -> bool {
        let mut ignored = false;
        [false, true].into_iter().any(|lan| self.deciding_rule(is_tcp, port, lan, &mut ignored).is_none())
    }

    /// Rules that can never fire because an earlier rule already decides everything they match.
    pub fn shadowed(&self) -> Vec<Value> {
        let on: Vec<&Rule> = self.rules.iter().filter(|r| r.enabled).collect();
        on.iter().enumerate().filter_map(|(i, r)| {
            on[..i].iter().find(|e| e.covers(r)).map(|e| json!({"id": r.id, "by": e.id, "same": e.action == r.action}))
        }).collect()
    }

    /// The two jumps of the `in` / `out` base chains (nothing when the firewall has nothing to do).
    pub fn hook(&self, inbound: bool) -> String {
        if !self.active() { return String::new(); }
        format!("\t\tct state established,related accept\n\t\tjump {}\n", if inbound { "fw_in" } else { "fw_out" })
    }

    fn chain(&self, inbound: bool) -> String {
        let (name, policy, ping) = if inbound { ("fw_in", self.inbound, self.ping_in) } else { ("fw_out", self.outbound, self.ping_out) };
        let mut s = format!("\tchain {name} {{\n");
        // What a network needs to work at all. Before the invalid check: conntrack does not track neighbour discovery.
        s += if inbound {
            "\t\ticmpv6 type { destination-unreachable, packet-too-big, time-exceeded, parameter-problem, nd-router-advert, nd-neighbor-solicit, nd-neighbor-advert, mld-listener-query } return\n\
             \t\tudp sport 67 udp dport 68 return\n\t\tip6 saddr fe80::/10 udp sport 547 udp dport 546 return\n"
        } else {
            "\t\ticmpv6 type { nd-router-solicit, nd-neighbor-solicit, nd-neighbor-advert, mld-listener-report, mld-listener-done, mld2-listener-report } return\n\
             \t\tudp sport 68 udp dport 67 return\n\t\tudp sport 546 udp dport 547 return\n"
        };
        if policy != Verdict::Allow { s += "\t\tct state invalid drop\n"; }
        for r in self.rules.iter().filter(|r| r.enabled && r.inbound == inbound) { s.extend(r.nft_lines(false)); }
        let echo = if ping { "return" } else { "jump fw_drop" };
        s += &format!("\t\ticmp type echo-request {echo}\n\t\ticmpv6 type echo-request {echo}\n");
        s += &format!("\t\tcounter {}comment \"default\"\n\t}}\n", if policy == Verdict::Allow { String::new() } else { format!("{} ", policy.nft()) });
        s
    }

    /// Connections DNAT passes on (published container ports) never reach `input`: they are incoming
    /// connections all the same, so the incoming rules and policy apply here too.
    fn forwarded_chains(&self) -> String {
        let mut s = String::from("\tchain fw_dnat {\n");
        for r in self.rules.iter().filter(|r| r.enabled && r.inbound) { s.extend(r.nft_lines(true)); }
        if self.inbound != Verdict::Allow { s += &format!("\t\tcounter {} comment \"default\"\n", self.inbound.nft()); }
        s + "\t}\n\tchain dnat_in {\n\t\ttype filter hook forward priority 0; policy accept;\n\t\tct status dnat ct state new jump fw_dnat\n\t}\n"
    }

    /// The firewall's chains. `log_group`: blocked packets are also sent there (rate limited) for the connection log.
    pub fn chains(&self, log_group: Option<u16>) -> String {
        if !self.active() { return String::new(); }
        let log = log_group.map_or(String::new(), |g| format!("\t\tmeta pkttype host limit rate 20/second burst 40 packets log group {g}\n"));
        format!("{}{}{}\tchain fw_drop {{\n{log}\t\tdrop\n\t}}\n\tchain fw_reject {{\n{log}\t\tmeta l4proto tcp reject with tcp reset\n\t\treject\n\t}}\n",
                self.chain(true), self.chain(false), self.forwarded_chains())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rule(dir: &str, action: &str, proto: &str, ports: &str, remote: &str) -> Value {
        json!({"dir": dir, "action": action, "proto": proto, "ports": ports, "remote": remote})
    }
    fn fw(rules: &[Value]) -> Firewall {
        let mut f = Firewall { enabled: true, ..Firewall::default() };
        f.apply(&json!({"add": rules})).unwrap();
        f
    }

    #[test]
    fn ports_are_merged_and_checked() {
        assert_eq!(parse_ports("443, 80,81, 79-80,1000-2000,1500").unwrap(), vec![(79, 81), (443, 443), (1000, 2000)]);
        assert_eq!(parse_ports("65535,65534").unwrap(), vec![(65534, 65535)]);
        for bad in ["0", "70000", "90-80", "a", "1-", "22;drop"] { assert!(parse_ports(bad).is_err(), "{bad}"); }
    }

    #[test]
    fn rule_fields_cannot_inject_nft() {
        let base = |k: &str, v: &str| { let mut r = rule("in", "block", "tcp", "22", ""); r[k] = json!(v); Rule::from_json(&r, 1) };
        for (k, v) in [("iface", "eth0\" accept"), ("iface", "lo"), ("iface", "a b"), ("iface", "0123456789abcdefg"), ("remote", "1.2.3.4 } accept"),
                       ("ports", "22 } accept"), ("proto", "tcp accept"), ("action", "accept"), ("dir", "forward")] {
            assert!(base(k, v).is_err(), "{k}={v}");
        }
        assert!(base("iface", "wg*").is_ok());
        // The note is stored for the user and never written into the ruleset.
        let r = base("note", "x\" accept #").unwrap();
        assert!(!r.nft_lines(false).concat().contains("accept"));
        assert!(base("iface", "123456789012345").is_ok() && base("iface", "123456789012345*").is_err(), "nft allows 15 characters with the *");
        assert!(Rule::from_json(&rule("in", "block", "any", "", ""), 1).is_err(), "match-everything rule");
    }

    #[test]
    fn nft_text() {
        let f = fw(&[rule("in", "allow", "tcp", "22, 80-81", "192.168.1.0/24, 2001:db8::/32"), rule("out", "reject", "any", "25", ""), rule("in", "block", "udp", "", "lan")]);
        let r = &f.rules;
        assert_eq!(r[0].nft_lines(false), ["\t\tip saddr { 192.168.1.0/24 } tcp dport { 22, 80-81 } counter return comment \"r1\"\n",
                                      "\t\tip6 saddr { 2001:db8::/32 } tcp dport { 22, 80-81 } counter return comment \"r1\"\n"]);
        assert!(r[0].nft_lines(true)[0].contains("meta l4proto tcp ct original proto-dst { 22, 80-81 }"));
        assert_eq!(r[1].nft_lines(false), ["\t\tmeta l4proto { tcp, udp } th dport { 25 } counter jump fw_reject comment \"r2\"\n"]);
        assert_eq!(r[2].nft_lines(false).len(), 2);
        assert!(!r[2].nft_lines(false)[0].contains("100.64"), "carrier-grade NAT is not a local network");
        assert!(f.chains(None).contains("ct status dnat ct state new jump fw_dnat"));
        let s = f.chains(None);
        assert!(s.contains("chain fw_in") && s.contains("chain fw_out") && !s.contains("log group"));
        assert!(f.chains(Some(7)).contains("log group 7"));
        let (rule_at, ping_at) = (s.find("comment \"r1\"").unwrap(), s.find("icmp type echo-request").unwrap());
        assert!(s.find("nd-neighbor-solicit").unwrap() < rule_at && rule_at < ping_at && ping_at < s.find("comment \"default\"").unwrap());
    }

    #[test]
    fn inactive_means_no_rules() {
        let mut f = Firewall::default();
        assert!(!f.active() && f.chains(None).is_empty() && f.hook(true).is_empty());
        f.inbound = Verdict::Block;
        assert!(!f.active(), "switched off: the policy is kept but not applied");
        f.enabled = true;
        assert!(f.active() && f.hook(true).contains("jump fw_in"));
        assert!(f.chains(None).contains("counter jump fw_drop comment \"default\""));
        let mut g = fw(&[rule("in", "allow", "tcp", "22", "")]);
        g.rules[0].enabled = false;
        assert!(!g.active());
    }

    #[test]
    fn requests_edit_the_list() {
        let mut f = fw(&[rule("in", "allow", "tcp", "22", ""), rule("in", "block", "tcp", "23", "")]);
        f.apply(&json!({"add": [rule("in", "allow", "tcp", "22", "")]})).unwrap();
        assert_eq!(f.rules.len(), 2, "same rule is not added twice");
        f.apply(&json!({"add": [rule("in", "block", "tcp", "24", "")], "front": true, "move": {"id": 1, "by": 1}})).unwrap();
        assert_eq!(f.rules.iter().map(|r| r.id).collect::<Vec<_>>(), [3, 2, 1]);
        let mut upd = rule("out", "reject", "udp", "53", ""); upd["id"] = json!(2);
        f.apply(&json!({"update": [upd], "remove": [3], "inbound": "reject", "ping_in": false})).unwrap();
        assert_eq!((f.rules.len(), f.rules[0].id, f.rules[0].inbound, f.inbound, f.ping_in), (2, 2, false, Verdict::Reject, false));
        assert!(f.apply(&json!({"remove": [99]})).is_err() && f.apply(&json!({"outbound": "maybe"})).is_err());
        let mut off = rule("in", "allow", "tcp", "9", ""); off["enabled"] = json!(false);
        f.apply(&json!({"add": [off]})).unwrap();
        f.apply(&json!({"add": [rule("in", "allow", "tcp", "9", "")]})).unwrap();
        assert!(f.rules.iter().find(|r| r.ports == [(9, 9)]).unwrap().enabled, "asking again switches it back on");
        let again = Firewall::from_json(&f.to_json()).unwrap();
        assert_eq!(again, f);
    }

    #[test]
    fn port_states() {
        let mut f = Firewall { enabled: true, inbound: Verdict::Block, ..Firewall::default() };
        assert_eq!(f.listen_state(true, 22), ("blocked", false));
        f.apply(&json!({"port": {"proto": "tcp", "port": 22, "state": "open"}})).unwrap();
        assert_eq!(f.listen_state(true, 22), ("open", false));
        assert_eq!(f.listen_state(false, 22), ("blocked", false), "udp is a different port");
        f.apply(&json!({"port": {"proto": "tcp", "port": 22, "state": "lan"}})).unwrap();
        assert_eq!((f.rules.len(), f.listen_state(true, 22)), (2, ("lan", false)));
        f.apply(&json!({"port": {"proto": "tcp", "port": 22, "state": "closed"}})).unwrap();
        assert_eq!((f.rules.len(), f.listen_state(true, 22)), (1, ("blocked", false)));
        f.apply(&json!({"port": {"proto": "tcp", "port": 22, "state": "default"}})).unwrap();
        assert!(f.rules.is_empty());
        f.apply(&json!({"add": [rule("in", "allow", "tcp", "8080", "203.0.113.5")]})).unwrap();
        assert_eq!(f.listen_state(true, 8080), ("blocked", true), "one address may still get in");
        assert!(f.apply(&json!({"port": {"proto": "icmp", "port": 1, "state": "open"}})).is_err());
        assert!(f.relies_on_default(true, 22));
        f.apply(&json!({"port": {"proto": "tcp", "port": 22, "state": "open"}})).unwrap();
        assert!(!f.relies_on_default(true, 22), "a rule keeps it open whatever the default is");
        f.apply(&json!({"port": {"proto": "tcp", "port": 22, "state": "lan"}})).unwrap();
        assert!(!f.relies_on_default(true, 22));
    }

    #[test]
    fn first_match_decides_live_connections() {
        let f = fw(&[rule("out", "allow", "tcp", "443", "203.0.113.0/24"), rule("out", "block", "tcp", "443", "")]);
        let fresh = f.blocking();
        let ip = |s: &str| s.parse::<IpAddr>().unwrap();
        assert!(!f.newly_blocks(&fresh, false, 6, 443, ip("203.0.113.9")));
        assert!(f.newly_blocks(&fresh, false, 6, 443, ip("8.8.8.8")));
        assert!(!f.newly_blocks(&fresh, false, 17, 443, ip("8.8.8.8")) && !f.newly_blocks(&fresh, true, 6, 443, ip("8.8.8.8")));
        assert!(!f.newly_blocks(&[], false, 6, 443, ip("8.8.8.8")), "a rule that already stood is not news");
    }

    #[test]
    fn shadowed_rules_are_found() {
        let f = fw(&[rule("in", "block", "any", "1-1024", ""), rule("in", "allow", "tcp", "22", ""), rule("in", "allow", "tcp", "8080", ""),
                     rule("in", "block", "udp", "53", "lan"), rule("out", "allow", "tcp", "22", "")]);
        let s = f.shadowed();
        assert_eq!(s.len(), 2);
        assert_eq!((s[0]["id"].as_u64(), s[0]["by"].as_u64(), s[0]["same"].as_bool()), (Some(2), Some(1), Some(false)));
        assert_eq!((s[1]["id"].as_u64(), s[1]["same"].as_bool()), (Some(4), Some(true)));
    }
}
