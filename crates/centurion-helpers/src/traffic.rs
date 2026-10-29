//! Traffic per program — Health → Network → Traffic.
//!
//! The netguard daemon, while traffic accounting is on, receives the kernel's conntrack NEW and
//! DESTROY events (ctnetlink). At NEW the connection is tied to the program owning its socket
//! (the socket still exists then); at DESTROY its byte counters are added to that program's total
//! for the local day. Nothing is sampled: a connection still open is counted when it closes.
//!
//! This module holds the pure parts: decoding a conntrack message, the day ledger and its file.
//! Totals: /var/lib/centurion/traffic.json, 90 days, 256 programs per day (the rest under "(other)").
//! 0640 root:wheel like connections.log: the program names are personal data.

use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::io::Write;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::io::AsRawFd;

pub const FILE: &str = "/var/lib/centurion/traffic.json";
pub const DAYS_KEPT: usize = 90;
pub const PROGRAMS_PER_DAY: usize = 256;
/// Programs beyond PROGRAMS_PER_DAY on one day.
pub const OTHER: &str = "(other)";
/// A new connection whose socket was found but whose owner had already exited.
pub const UNKNOWN: &str = "(unknown)";
const MAX_FILE: u64 = 8 << 20;
const MAX_NAME: usize = 256;

// conntrack message types (NFNL_SUBSYS_CTNETLINK << 8 | IPCTNL_MSG_CT_*)
pub const CT_NEW: u16 = 1 << 8;
pub const CT_DELETE: u16 = (1 << 8) | 2;

const CTA_TUPLE_ORIG: u16 = 1;
const CTA_COUNTERS_ORIG: u16 = 9;
const CTA_COUNTERS_REPLY: u16 = 10;
const CTA_ID: u16 = 12;
const CTA_TUPLE_IP: u16 = 1;
const CTA_TUPLE_PROTO: u16 = 2;
const CTA_IP_V4_SRC: u16 = 1;
const CTA_IP_V4_DST: u16 = 2;
const CTA_IP_V6_SRC: u16 = 3;
const CTA_IP_V6_DST: u16 = 4;
const CTA_PROTO_NUM: u16 = 1;
const CTA_PROTO_SRC_PORT: u16 = 2;
const CTA_PROTO_DST_PORT: u16 = 3;
const CTA_COUNTERS_BYTES: u16 = 2;

/// One conntrack NEW or DELETE event for a TCP or UDP flow, in its original direction.
#[derive(Debug, PartialEq)]
pub struct CtEvent {
    pub delete: bool,
    pub id: u32,
    pub proto: u8,
    pub src: IpAddr,
    pub sport: u16,
    pub dst: IpAddr,
    pub dport: u16,
    /// Bytes sent in the original direction / in the reply direction (0 unless accounting is on).
    pub orig_bytes: u64,
    pub reply_bytes: u64,
}

/// Netlink attributes of a payload: (type without the nested/byte-order flags, data).
fn attrs(mut b: &[u8]) -> impl Iterator<Item = (u16, &[u8])> {
    std::iter::from_fn(move || {
        if b.len() < 4 { return None; }
        let len = u16::from_ne_bytes([b[0], b[1]]) as usize;
        if len < 4 || len > b.len() { return None; }
        let ty = u16::from_ne_bytes([b[2], b[3]]) & 0x3fff;
        let data = &b[4..len];
        b = &b[((len + 3) & !3).min(b.len())..];
        Some((ty, data))
    })
}

fn be16(d: &[u8]) -> Option<u16> { Some(u16::from_be_bytes(d.get(..2)?.try_into().ok()?)) }
fn be32(d: &[u8]) -> Option<u32> { Some(u32::from_be_bytes(d.get(..4)?.try_into().ok()?)) }
fn be64(d: &[u8]) -> Option<u64> { Some(u64::from_be_bytes(d.get(..8)?.try_into().ok()?)) }

/// Byte count of a CTA_COUNTERS_* nest.
fn counter_bytes(nest: &[u8]) -> u64 {
    attrs(nest).find(|(t, _)| *t == CTA_COUNTERS_BYTES).and_then(|(_, d)| be64(d)).unwrap_or(0)
}

/// Fills addresses, protocol and ports of CTA_TUPLE_ORIG.
fn tuple(nest: &[u8]) -> Option<(IpAddr, IpAddr, u8, u16, u16)> {
    let (mut src, mut dst, mut proto, mut sport, mut dport) = (None, None, None, 0u16, 0u16);
    for (t, d) in attrs(nest) {
        match t {
            CTA_TUPLE_IP => for (k, v) in attrs(d) {
                match (k, v.len()) {
                    (CTA_IP_V4_SRC, 4) => src = Some(IpAddr::V4(Ipv4Addr::new(v[0], v[1], v[2], v[3]))),
                    (CTA_IP_V4_DST, 4) => dst = Some(IpAddr::V4(Ipv4Addr::new(v[0], v[1], v[2], v[3]))),
                    (CTA_IP_V6_SRC, 16) => src = Some(IpAddr::V6(Ipv6Addr::from(<[u8; 16]>::try_from(v).ok()?))),
                    (CTA_IP_V6_DST, 16) => dst = Some(IpAddr::V6(Ipv6Addr::from(<[u8; 16]>::try_from(v).ok()?))),
                    _ => {}
                }
            },
            CTA_TUPLE_PROTO => for (k, v) in attrs(d) {
                match k {
                    CTA_PROTO_NUM => proto = v.first().copied(),
                    CTA_PROTO_SRC_PORT => sport = be16(v)?,
                    CTA_PROTO_DST_PORT => dport = be16(v)?,
                    _ => {}
                }
            },
            _ => {}
        }
    }
    Some((src?, dst?, proto?, sport, dport))
}

/// Decodes a netlink message of conntrack type `ty`; `payload` is what follows the 16-byte netlink
/// header (the nfgenmsg header first). Other message types, and TCP/UDP-less flows, give None.
pub fn parse_ct(ty: u16, payload: &[u8]) -> Option<CtEvent> {
    if ty != CT_NEW && ty != CT_DELETE { return None; }
    let mut ev = CtEvent { delete: ty == CT_DELETE, id: 0, proto: 0, src: IpAddr::V4(Ipv4Addr::UNSPECIFIED), sport: 0,
                           dst: IpAddr::V4(Ipv4Addr::UNSPECIFIED), dport: 0, orig_bytes: 0, reply_bytes: 0 };
    let mut have = false;
    for (t, d) in attrs(payload.get(4..)?) {  // skip the nfgenmsg header
        match t {
            CTA_TUPLE_ORIG => {
                let (src, dst, proto, sport, dport) = tuple(d)?;
                (ev.src, ev.dst, ev.proto, ev.sport, ev.dport) = (src, dst, proto, sport, dport);
                have = true;
            }
            CTA_COUNTERS_ORIG => ev.orig_bytes = counter_bytes(d),
            CTA_COUNTERS_REPLY => ev.reply_bytes = counter_bytes(d),
            CTA_ID => ev.id = be32(d)?,
            _ => {}
        }
    }
    (have && (ev.proto == 6 || ev.proto == 17)).then_some(ev)
}

/// Local calendar day (YYYY-MM-DD) of a unix time, as the rest of the machine counts days.
pub fn local_day(secs: u64) -> String {
    unsafe {
        let t = secs as libc::time_t;
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&t, &mut tm);
        format!("{:04}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Tot {
    pub tx: u64,
    pub rx: u64,
    pub conns: u64,
}

/// day → program → totals.
#[derive(Debug, Default, PartialEq)]
pub struct Ledger {
    days: BTreeMap<String, BTreeMap<String, Tot>>,
}

impl Ledger {
    /// Adds one closed connection to `day` under `program` (cut to MAX_NAME characters).
    pub fn add(&mut self, day: &str, program: &str, tx: u64, rx: u64) {
        let name: String = program.chars().take(MAX_NAME).collect();
        let progs = self.days.entry(day.to_owned()).or_default();
        let key = if progs.contains_key(&name) || progs.len() < PROGRAMS_PER_DAY { name } else { OTHER.to_owned() };
        let t = progs.entry(key).or_default();
        t.tx = t.tx.saturating_add(tx);
        t.rx = t.rx.saturating_add(rx);
        t.conns += 1;
        while self.days.len() > DAYS_KEPT {
            let oldest = self.days.keys().next().cloned().unwrap_or_default();
            self.days.remove(&oldest);
        }
    }

    pub fn to_json(&self) -> Value {
        let days: serde_json::Map<String, Value> = self.days.iter().map(|(day, progs)| {
            let p: serde_json::Map<String, Value> = progs.iter()
                .map(|(name, t)| (name.clone(), json!({"tx": t.tx, "rx": t.rx, "conns": t.conns}))).collect();
            (day.clone(), Value::Object(p))
        }).collect();
        json!({"version": 1, "days": days})
    }

    pub fn from_json(text: &str) -> Ledger {
        let mut l = Ledger::default();
        let Ok(v) = serde_json::from_str::<Value>(text) else { return l };
        for (day, progs) in v["days"].as_object().into_iter().flatten() {
            for (name, t) in progs.as_object().into_iter().flatten() {
                let tot = Tot { tx: t["tx"].as_u64().unwrap_or(0), rx: t["rx"].as_u64().unwrap_or(0), conns: t["conns"].as_u64().unwrap_or(0) };
                let days = l.days.entry(day.clone()).or_default();
                if days.len() < PROGRAMS_PER_DAY || days.contains_key(name) {
                    let e = days.entry(name.chars().take(MAX_NAME).collect()).or_default();
                    e.tx = e.tx.saturating_add(tot.tx);
                    e.rx = e.rx.saturating_add(tot.rx);
                    e.conns += tot.conns;
                }
            }
        }
        while l.days.len() > DAYS_KEPT {
            let oldest = l.days.keys().next().cloned().unwrap_or_default();
            l.days.remove(&oldest);
        }
        l
    }

    /// The saved totals; a missing or unreadable file starts an empty ledger.
    pub fn load() -> Ledger {
        crate::read_root_file(FILE, MAX_FILE).map(|s| Ledger::from_json(&s)).unwrap_or_default()
    }

    /// Writes the file 0640 root:`gid` (0600 when there is no such group), atomically.
    pub fn save(&self, gid: Option<u32>) -> Result<(), String> {
        crate::secure_dir("/var/lib/centurion")?;
        let body = self.to_json().to_string();
        if body.len() as u64 > MAX_FILE { return Err("traffic totals exceed the size limit".into()); }
        let tmp = format!("{FILE}.{}.tmp", std::process::id());
        let _ = std::fs::remove_file(&tmp);
        let mode = if gid.is_some() { 0o640 } else { 0o600 };
        let mut f = std::fs::OpenOptions::new().write(true).create_new(true).mode(mode)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(&tmp).map_err(|e| format!("{tmp}: {e}"))?;
        if let Some(g) = gid { unsafe { libc::fchown(f.as_raw_fd(), 0, g); } }
        let res = f.write_all(body.as_bytes()).and_then(|_| f.sync_all());
        drop(f);
        res.and_then(|_| std::fs::rename(&tmp, FILE)).map_err(|e| {
            let _ = std::fs::remove_file(&tmp);
            format!("{FILE}: {e}")
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// One netlink attribute, padded to 4 bytes.
    fn a(ty: u16, data: &[u8]) -> Vec<u8> {
        let mut v = ((4 + data.len()) as u16).to_ne_bytes().to_vec();
        v.extend_from_slice(&ty.to_ne_bytes());
        v.extend_from_slice(data);
        while v.len() % 4 != 0 { v.push(0); }
        v
    }

    /// Payload of a conntrack message: nfgenmsg, then the attributes.
    fn msg(attrs: &[Vec<u8>]) -> Vec<u8> {
        let mut v = vec![2u8, 0, 0, 0];
        for x in attrs { v.extend_from_slice(x); }
        v
    }

    fn tuple(src: [u8; 4], dst: [u8; 4], proto: u8, sport: u16, dport: u16) -> Vec<u8> {
        let ip = [a(CTA_IP_V4_SRC, &src), a(CTA_IP_V4_DST, &dst)].concat();
        let pr = [a(CTA_PROTO_NUM, &[proto]), a(CTA_PROTO_SRC_PORT, &sport.to_be_bytes()), a(CTA_PROTO_DST_PORT, &dport.to_be_bytes())].concat();
        a(CTA_TUPLE_ORIG, &[a(CTA_TUPLE_IP, &ip), a(CTA_TUPLE_PROTO, &pr)].concat())
    }

    fn counters(ty: u16, packets: u64, bytes: u64) -> Vec<u8> {
        a(ty, &[a(1, &packets.to_be_bytes()), a(CTA_COUNTERS_BYTES, &bytes.to_be_bytes())].concat())
    }

    #[test]
    fn decodes_new_and_delete() {
        let new = msg(&[tuple([192, 168, 1, 10], [93, 184, 216, 34], 6, 50000, 443), a(CTA_ID, &7u32.to_be_bytes())]);
        let ev = parse_ct(CT_NEW, &new).unwrap();
        assert_eq!(ev, CtEvent { delete: false, id: 7, proto: 6, src: "192.168.1.10".parse().unwrap(), sport: 50000,
                                 dst: "93.184.216.34".parse().unwrap(), dport: 443, orig_bytes: 0, reply_bytes: 0 });

        let del = msg(&[tuple([192, 168, 1, 10], [93, 184, 216, 34], 17, 5353, 53), a(CTA_ID, &9u32.to_be_bytes()),
                        counters(CTA_COUNTERS_ORIG, 3, 210), counters(CTA_COUNTERS_REPLY, 2, 480)]);
        let ev = parse_ct(CT_DELETE, &del).unwrap();
        assert!(ev.delete && ev.id == 9 && ev.proto == 17 && ev.dport == 53);
        assert_eq!((ev.orig_bytes, ev.reply_bytes), (210, 480));
    }

    #[test]
    fn ignores_other_messages_and_protocols() {
        let icmp = msg(&[tuple([10, 0, 0, 1], [10, 0, 0, 2], 1, 0, 0), a(CTA_ID, &1u32.to_be_bytes())]);
        assert!(parse_ct(CT_NEW, &icmp).is_none());
        let tcp = msg(&[tuple([10, 0, 0, 1], [10, 0, 0, 2], 6, 1, 2)]);
        assert!(parse_ct(CT_NEW, &tcp).is_some());
        assert!(parse_ct(CT_NEW + 1, &tcp).is_none());   // UPDATE events are not used
        assert!(parse_ct(CT_NEW, &[0u8; 2]).is_none());   // too short
        assert!(parse_ct(CT_NEW, &msg(&[a(CTA_TUPLE_ORIG, &[1, 2, 3])])).is_none());  // truncated tuple
    }

    #[test]
    fn ledger_adds_prunes_and_caps_programs() {
        let mut l = Ledger::default();
        l.add("2026-10-29", "firefox", 100, 900);
        l.add("2026-10-29", "firefox", 1, 2);
        assert_eq!(l.days["2026-10-29"]["firefox"], Tot { tx: 101, rx: 902, conns: 2 });
        for i in 0..PROGRAMS_PER_DAY + 5 { l.add("2026-10-30", &format!("app{i}"), 1, 0); }
        assert!(l.days["2026-10-30"].contains_key(OTHER));
        assert_eq!(l.days["2026-10-30"].len(), PROGRAMS_PER_DAY + 1);
        for d in 1..=DAYS_KEPT + 3 { l.add(&format!("2027-01-{d:02}"), "x", 1, 1); }
        assert_eq!(l.days.len(), DAYS_KEPT);
        assert!(!l.days.contains_key("2026-10-29"));  // oldest days go first
    }

    #[test]
    fn ledger_round_trips_and_tolerates_damage() {
        let mut l = Ledger::default();
        l.add("2026-10-29", "game.exe", 5, 7);
        let back = Ledger::from_json(&l.to_json().to_string());
        assert_eq!(back, l);
        assert_eq!(Ledger::from_json("not json"), Ledger::default());
        assert_eq!(Ledger::from_json(r#"{"days": {"d": {"p": {"tx": "x"}}}}"#).days["d"]["p"], Tot::default());
    }

    #[test]
    fn local_day_is_a_calendar_date() {
        let d = local_day(1_700_000_000);
        assert_eq!(d.len(), 10);
        assert!(d.starts_with("2023-"));
    }
}
