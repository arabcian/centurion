//! Network guard — Health → Network.
//!
//! Five parts, one module:
//!   * connection list   sock_diag (the kernel interface behind `ss`) + /proc/<pid>/fd
//!   * IP blacklist      nftables table `inet centurion_netguard` (drop in + out)
//!   * firewall          default inbound / outbound policy and port rules (firewall.rs), same table
//!   * app block list    native Linux programs named in `block_apps` (exact path, directory or file name):
//!    the same queue finds the owning process; if its executable is listed: DROP, socket destroyed, logged.
//!    Default allow (a list of what to block), unlike the Wine guard's default deny.
//!   * Wine/.exe guard   the first packet of every new outbound flow is queued
//!    (NFQUEUE) to the daemon; it finds the owning socket
//!    and process, and if that is a Wine program which is
//!    not whitelisted: verdict DROP, socket destroyed
//!    (SOCK_DESTROY), one line in the block log.
//!   * traffic           switchable (`traffic`): conntrack NEW/DESTROY events are matched to the owning program
//!    and its byte counters are added to a per-day ledger (traffic.rs). Nothing is sampled.
//!   * connection log    switchable (`log_conns`): the first packet of every new
//!    flow, outbound and inbound, goes through the same queue,
//!    is accepted at once and written to the connection log
//!    with its program. Plus an on-demand whois client.
//!
//! Config   /etc/centurion/netguard.json   (root-owned, world-readable)
//! Log      /var/log/centurion/netguard.log (JSON lines, rotated at 1 MiB)
//! Traffic  /var/lib/centurion/traffic.json (per-day totals, written at most every TRAFFIC_FLUSH)
//!          /var/log/centurion/connections.log (JSON lines, rotated at 4 MiB)
//! Daemon   centurion-netguard daemon; holds a lock on /run/centurion/netguard.pid,
//!          reloads on SIGHUP (sent by the helper after every rule change).
//!
//! Cost model: nothing is polled. With the guard and the connection log off
//! there is no queue rule at all; with one on, only the first packet(s) of a
//! new non-loopback flow reach the daemon (ct state new), and a flow that does
//! not belong to a Wine process costs one exact socket lookup plus a readlink
//! per known Wine pid. The connection log names the owning process from /proc
//! after the verdict went out, batched: at most one pass per FLUSH_GAP, the
//! processes that opened the last connections first.
//! With Wine guard enabled, the outbound queue deliberately fails closed if
//! the daemon disappears; connection-log-only queues retain `bypass`. The
//! firewall and blacklist rules still take precedence over both queues.
//!
//! Kernel: NETFILTER_NETLINK_QUEUE, NF_TABLES, NF_TABLES_INET, NFT_QUEUE,
//! NF_CONNTRACK, NFT_CT, INET_DIAG, INET_TCP_DIAG, INET_UDP_DIAG,
//! INET_DIAG_DESTROY. Userspace: nft (net-firewall/nftables).

use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::io::{self, BufRead, Read, Write};
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt};
use std::os::unix::io::{AsRawFd, FromRawFd, OwnedFd};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::time::{Duration, Instant};
use crate::firewall::{Firewall, Verdict};
use crate::traffic;

pub const CONF_DIR: &str = "/etc/centurion";
pub const CONF_FILE: &str = "/etc/centurion/netguard.json";
/// Above what the limits allow: MAX_BLACKLIST entries, each with a 64-character label, plus all rules.
const CONF_MAX: u64 = 64 << 20;
const RUN_DIR: &str = "/run/centurion";
const LOCK_FILE: &str = "/run/centurion/netguard.pid";
const STATE_FILE: &str = "/run/centurion/netguard.state";
const LOG_DIR: &str = "/var/log/centurion";
pub const LOG_FILE: &str = "/var/log/centurion/netguard.log";
const LOG_MAX: u64 = 1 << 20;
pub const CONN_LOG: &str = "/var/log/centurion/connections.log";
const CONN_LOG_MAX: u64 = 4 << 20;
const FLUSH_GAP: Duration = Duration::from_millis(200);
const HOOK_LOCAL_IN: u8 = 1;  // NF_INET_LOCAL_IN
const QUEUE_NUM: u16 = 19536;
const TABLE: &str = "centurion_netguard";
/// Entries in the IP blacklist. Large enough for a published netset (a subscription, Rules → Subscriptions…).
const MAX_BLACKLIST: usize = 100_000;
const MAX_WHITELIST: usize = 1024;
const NETLINK_NETFILTER: libc::c_int = 12;
const NFNLGRP_CONNTRACK_NEW: u32 = 1;
const NFNLGRP_CONNTRACK_DESTROY: u32 = 3;
const NFNLGRP_NFTABLES: u32 = 7;
const NFT_MSG_DELTABLE: u16 = 10 << 8 | 2;  // NFNL_SUBSYS_NFTABLES << 8
const NFPROTO_INET: u8 = 1;
const NFTA_TABLE_NAME: u16 = 1;
const CT_GET: u16 = 1 << 8 | 1;             // NFNL_SUBSYS_CTNETLINK << 8 | IPCTNL_MSG_CT_GET
const NF_ACCT_SYSCTL: &str = "/proc/sys/net/netfilter/nf_conntrack_acct";
const TRAFFIC_FLUSH: Duration = Duration::from_secs(10);
/// nfnetlink_log group of packets the firewall stopped (shown in the connection log).
const NFLOG_GROUP: u16 = QUEUE_NUM;

pub const KCONFIG: &[&str] = &["NETFILTER_NETLINK_QUEUE", "NF_TABLES", "NF_TABLES_INET", "NFT_QUEUE", "NF_CONNTRACK",
                               "NFT_CT", "INET_DIAG", "INET_TCP_DIAG", "INET_UDP_DIAG", "INET_DIAG_DESTROY"];

/// Needed only by the firewall features that use them.
const KCONFIG_REJECT: &[&str] = &["NFT_REJECT", "NFT_REJECT_INET"];
const KCONFIG_FW_LOG: &[&str] = &["NFT_LOG", "NETFILTER_NETLINK_LOG"];

const AF_INET: u8 = libc::AF_INET as u8;
const AF_INET6: u8 = libc::AF_INET6 as u8;
const TCP: u8 = 6;
const UDP: u8 = 17;

// ── networks ───────────────────────────────────────────────────────────────

/// An address or CIDR block, host bits cleared.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Net { addr: IpAddr, prefix: u8 }

fn unmap(ip: IpAddr) -> IpAddr {
    match ip { IpAddr::V6(v6) => v6.to_ipv4_mapped().map_or(ip, IpAddr::V4), v4 => v4 }
}

impl Net {
    pub fn parse(s: &str) -> Option<Net> {
        let (a, p) = match s.trim().split_once('/') { Some((a, p)) => (a, Some(p)), None => (s.trim(), None) };
        let mut addr = unmap(a.parse().ok()?);
        let max = if addr.is_ipv4() { 32 } else { 128 };
        let prefix: u8 = match p { Some(p) => p.parse().ok()?, None => max };
        if prefix == 0 || prefix > max { return None; }  // /0 would cut the machine off
        addr = match addr {
            IpAddr::V4(v) => IpAddr::V4(Ipv4Addr::from(u32::from(v) & (!0u32 << (32 - prefix)))),
            IpAddr::V6(v) => IpAddr::V6(Ipv6Addr::from(u128::from(v) & (!0u128 << (128 - prefix)))),
        };
        Some(Net { addr, prefix })
    }

    pub fn is_ipv4(&self) -> bool { self.addr.is_ipv4() }

    pub fn contains(&self, ip: IpAddr) -> bool {
        match (self.addr, unmap(ip)) {
            (IpAddr::V4(n), IpAddr::V4(i)) => (u32::from(i) ^ u32::from(n)) >> (32 - self.prefix) == 0,
            (IpAddr::V6(n), IpAddr::V6(i)) => (u128::from(i) ^ u128::from(n)) >> (128 - self.prefix) == 0,
            _ => false,
        }
    }
}

/// One blacklist entry as typed: an address, a CIDR block, or a range
/// "first - last" (split into the CIDR blocks that cover exactly it).
pub fn parse_nets(s: &str) -> Option<Vec<Net>> {
    let Some((a, b)) = s.split_once('-') else { return Net::parse(s).map(|n| vec![n]) };
    let (bits, lo, hi) = match (unmap(a.trim().parse().ok()?), unmap(b.trim().parse().ok()?)) {
        (IpAddr::V4(a), IpAddr::V4(b)) => (32u32, u128::from(u32::from(a)), u128::from(u32::from(b))),
        (IpAddr::V6(a), IpAddr::V6(b)) => (128, u128::from(a), u128::from(b)),
        _ => return None,
    };
    if lo > hi { return None; }
    let last_of = |cur: u128, host: u32| if host >= 128 { u128::MAX } else { cur | ((1u128 << host) - 1) };
    let (mut out, mut cur) = (Vec::new(), lo);
    loop {
        // The largest aligned block that starts here and does not pass the end.
        let mut host = if cur == 0 { bits } else { cur.trailing_zeros().min(bits) };
        while host > 0 && last_of(cur, host) > hi { host -= 1; }
        if host == bits { return None; }  // the whole address space
        let addr = if bits == 32 { IpAddr::V4(Ipv4Addr::from(cur as u32)) } else { IpAddr::V6(Ipv6Addr::from(cur)) };
        out.push(Net { addr, prefix: (bits - host) as u8 });
        if last_of(cur, host) >= hi { return Some(out); }
        cur = last_of(cur, host) + 1;
    }
}

/// Private, CGNAT, link-local, multicast, broadcast: the guard's "LAN exempt" set.
pub fn is_lan(ip: IpAddr) -> bool {
    match unmap(ip) {
        IpAddr::V4(v) => {
            let o = v.octets();
            v.is_private() || v.is_link_local() || v.is_multicast() || v.is_broadcast() || (o[0] == 100 && o[1] & 0xc0 == 64)
        }
        IpAddr::V6(v) => { let s = v.segments()[0]; s & 0xfe00 == 0xfc00 || s & 0xffc0 == 0xfe80 || s & 0xff00 == 0xff00 }
    }
}

impl std::fmt::Display for Net {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.prefix == if self.addr.is_ipv4() { 32 } else { 128 } { write!(f, "{}", self.addr) }
        else { write!(f, "{}/{}", self.addr, self.prefix) }
    }
}

// ── config ─────────────────────────────────────────────────────────────────

#[derive(Clone, Debug)]
pub struct Config {
    /// Block Wine programs that are not on the whitelist.
    pub guard: bool,
    /// Private / link-local / multicast destinations are not "internet".
    pub allow_lan: bool,
    /// Log every new connection, outbound and inbound, to CONN_LOG.
    pub log_conns: bool,
    /// Count bytes per program per day (traffic.rs).
    pub traffic: bool,
    /// Exact path, directory prefix (trailing '/') or bare file name (case-insensitive).
    pub whitelist: Vec<String>,
    /// Native (non-Wine) programs kept off the internet: same entry forms as the whitelist, matched on the executable.
    pub apps: Vec<String>,
    pub blacklist: Vec<Net>,
    /// Blacklist entry (as printed) → group name; entries banned together are removed together.
    pub labels: HashMap<String, String>,
    /// Blacklist entry → the other groups that list it too ("" = also added by hand): it stays until all let go.
    pub shared: HashMap<String, Vec<String>>,
    /// Default policies and port rules.
    pub fw: Firewall,
}

impl Default for Config {
    fn default() -> Self { Config { guard: false, allow_lan: true, log_conns: false, traffic: false, whitelist: Vec::new(), apps: Vec::new(), blacklist: Vec::new(), labels: HashMap::new(), shared: HashMap::new(), fw: Firewall::default() } }
}

fn basename(s: &str) -> &str { s.rsplit(['/', '\\']).next().unwrap_or(s) }

/// Exact path, directory prefix (trailing '/') or bare file name (case-insensitive).
fn list_matches(list: &[String], ident: &str) -> bool {
    let base = basename(ident).to_lowercase();
    list.iter().any(|w| {
        if w.ends_with('/') { ident.starts_with(w.as_str()) }
        else if w.contains('/') || w.contains('\\') { w == ident }
        else if w.is_ascii() { w.eq_ignore_ascii_case(&base) }
        else { w.to_lowercase() == base }
    })
}

fn valid_entry(s: &str) -> bool { !s.is_empty() && s.len() <= 512 && !s.chars().any(char::is_control) }

impl Config {
    pub fn load() -> Result<Config, String> {
        if !Path::new(CONF_FILE).exists() { return Ok(Config::default()); }
        let text = crate::read_root_file(CONF_FILE, CONF_MAX).ok_or_else(|| format!("{CONF_FILE} is not a root-owned regular file"))?;
        let v: Value = serde_json::from_str(&text).map_err(|e| format!("{CONF_FILE}: {e}"))?;
        let strs = |k: &str| v[k].as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(str::to_owned)).collect::<Vec<_>>()).unwrap_or_default();
        let blacklist: Vec<Net> = strs("blacklist").iter().filter_map(|s| Net::parse(s)).take(MAX_BLACKLIST).collect();
        let have: HashSet<String> = blacklist.iter().map(Net::to_string).collect();
        let labels = v["labels"].as_object().map(|m| m.iter().filter(|(k, _)| have.contains(*k))
            .filter_map(|(k, l)| Some((k.clone(), l.as_str()?.to_owned()))).collect()).unwrap_or_default();
        let shared = v["shared_labels"].as_object().map(|m| m.iter().filter(|(k, _)| have.contains(*k))
            .map(|(k, l)| (k.clone(), strings(l))).collect()).unwrap_or_default();
        Ok(Config {
            guard: v["guard"].as_bool().unwrap_or(false),
            allow_lan: v["allow_lan"].as_bool().unwrap_or(true),
            log_conns: v["log_conns"].as_bool().unwrap_or(false),
            traffic: v["traffic"].as_bool().unwrap_or(false),
            whitelist: strs("whitelist").into_iter().filter(|s| valid_entry(s)).take(MAX_WHITELIST).collect(),
            apps: strs("block_apps").into_iter().filter(|s| valid_entry(s)).take(MAX_WHITELIST).collect(),
            blacklist,
            labels,
            shared,
            fw: Firewall::from_json(&v["firewall"]).map_err(|e| format!("{CONF_FILE}: {e}"))?,
        })
    }

    pub fn to_json(&self) -> Value {
        json!({"guard": self.guard, "allow_lan": self.allow_lan, "log_conns": self.log_conns, "traffic": self.traffic, "whitelist": self.whitelist,
               "block_apps": self.apps, "blacklist": self.blacklist.iter().map(Net::to_string).collect::<Vec<_>>(), "labels": self.labels, "shared_labels": self.shared, "firewall": self.fw.to_json()})
    }

    /// What the daemon enforces when the saved rules cannot be read: nothing new gets in, and the machine stays online.
    fn safe_mode() -> Config {
        Config { fw: Firewall { enabled: true, inbound: Verdict::Block, ..Firewall::default() }, ..Config::default() }
    }

    pub fn save(&self) -> Result<(), String> {
        crate::secure_dir(CONF_DIR)?;
        crate::write_root_file(CONF_FILE, &serde_json::to_vec_pretty(&self.to_json()).map_err(|e| e.to_string())?)
    }

    /// An entry without a path separator matches the program's *file name*, so a program renamed to a listed
    /// name matches too; an entry with a path (or a trailing '/') is compared as a whole. Whitelist full paths
    /// for anything that must not be spoofable by renaming.
    pub fn whitelisted(&self, ident: &str) -> bool { list_matches(&self.whitelist, ident) }

    /// A native program on the block list (same entry forms as the whitelist, compared with its executable).
    pub fn app_blocked(&self, exe: &str) -> bool { list_matches(&self.apps, exe) }

    /// Outbound flows go through the daemon for a verdict: the Wine guard, or a non-empty app block list.
    pub fn guarding(&self) -> bool { self.guard || !self.apps.is_empty() }

    pub fn blacklisted(&self, ip: IpAddr) -> bool { self.blacklist.iter().any(|n| n.contains(ip)) }
}

// ── netlink plumbing ───────────────────────────────────────────────────────

const NLM_F_REQUEST: u16 = 1;
const NLM_F_ACK: u16 = 4;
const NLM_F_DUMP: u16 = 0x300;
const NLMSG_ERROR: u16 = 2;
const NLMSG_DONE: u16 = 3;

fn nl_open(proto: libc::c_int) -> io::Result<OwnedFd> {
    let raw = unsafe { libc::socket(libc::AF_NETLINK, libc::SOCK_RAW | libc::SOCK_CLOEXEC, proto) };
    if raw < 0 { return Err(io::Error::last_os_error()); }
    let fd = unsafe { OwnedFd::from_raw_fd(raw) };
    let mut sa: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
    sa.nl_family = libc::AF_NETLINK as libc::sa_family_t;
    let r = unsafe { libc::bind(raw, &sa as *const _ as *const libc::sockaddr, std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t) };
    if r < 0 { return Err(io::Error::last_os_error()); }
    Ok(fd)
}

fn nl_msg(ty: u16, flags: u16, body: &[u8]) -> Vec<u8> {
    let mut m = Vec::with_capacity(16 + body.len() + 3);
    m.extend_from_slice(&((16 + body.len()) as u32).to_ne_bytes());
    m.extend_from_slice(&ty.to_ne_bytes());
    m.extend_from_slice(&flags.to_ne_bytes());
    m.extend_from_slice(&[0u8; 8]);  // seq, pid
    m.extend_from_slice(body);
    while m.len() % 4 != 0 { m.push(0); }
    m
}

pub(crate) fn nl_attr(out: &mut Vec<u8>, ty: u16, data: &[u8]) {
    out.extend_from_slice(&((4 + data.len()) as u16).to_ne_bytes());
    out.extend_from_slice(&ty.to_ne_bytes());
    out.extend_from_slice(data);
    while out.len() % 4 != 0 { out.push(0); }
}

fn nl_send(fd: &OwnedFd, msg: &[u8]) -> io::Result<()> {
    loop {
        let n = unsafe { libc::send(fd.as_raw_fd(), msg.as_ptr().cast(), msg.len(), 0) };
        if n >= 0 { return Ok(()); }
        let e = io::Error::last_os_error();
        if e.kind() != io::ErrorKind::Interrupted { return Err(e); }
    }
}

fn nl_recv(fd: &OwnedFd, buf: &mut [u8], flags: libc::c_int) -> io::Result<usize> {
    let n = unsafe { libc::recv(fd.as_raw_fd(), buf.as_mut_ptr().cast(), buf.len(), flags) };
    if n < 0 { Err(io::Error::last_os_error()) } else { Ok(n as usize) }
}

/// Calls `f(type, payload)` for every netlink message in one datagram.
fn nl_each(mut buf: &[u8], mut f: impl FnMut(u16, &[u8])) {
    while buf.len() >= 16 {
        let len = u32::from_ne_bytes([buf[0], buf[1], buf[2], buf[3]]) as usize;
        if len < 16 || len > buf.len() { break; }
        f(u16::from_ne_bytes([buf[4], buf[5]]), &buf[16..len]);
        buf = &buf[((len + 3) & !3).min(buf.len())..];
    }
}

/// errno of an NLMSG_ERROR payload (0 = plain ACK).
fn nl_errno(p: &[u8]) -> i32 { if p.len() >= 4 { -i32::from_ne_bytes([p[0], p[1], p[2], p[3]]) } else { libc::EIO } }

// ── sock_diag ──────────────────────────────────────────────────────────────

const SOCK_DIAG_BY_FAMILY: u16 = 20;
const SOCK_DESTROY: u16 = 21;
const TCP_LISTEN: u8 = 10;

#[derive(Clone, Debug)]
pub struct Sock {
    pub family: u8,
    pub proto: u8,
    pub state: u8,
    pub src: IpAddr,
    pub sport: u16,
    pub dst: IpAddr,
    pub dport: u16,
    pub uid: u32,
    pub inode: u32,
    id: [u8; 48],  // inet_diag_sockid as the kernel reported it (with cookie), for SOCK_DESTROY
}

fn ip_of(family: u8, b: &[u8]) -> IpAddr {
    if family == AF_INET { IpAddr::V4(Ipv4Addr::new(b[0], b[1], b[2], b[3])) }
    else { let mut a = [0u8; 16]; a.copy_from_slice(&b[..16]); unmap(IpAddr::V6(Ipv6Addr::from(a))) }
}

/// struct inet_diag_msg (72 bytes) → Sock.
fn parse_sock(p: &[u8], proto: u8) -> Option<Sock> {
    if p.len() < 72 || (p[0] != AF_INET && p[0] != AF_INET6) { return None; }
    let mut id = [0u8; 48];
    id.copy_from_slice(&p[4..52]);
    Some(Sock {
        family: p[0], proto, state: p[1],
        sport: u16::from_be_bytes([id[0], id[1]]), dport: u16::from_be_bytes([id[2], id[3]]),
        src: ip_of(p[0], &id[4..20]), dst: ip_of(p[0], &id[20..36]),
        uid: u32::from_ne_bytes([p[64], p[65], p[66], p[67]]),
        inode: u32::from_ne_bytes([p[68], p[69], p[70], p[71]]),
        id,
    })
}

fn put_ip(dst: &mut [u8], ip: IpAddr) {
    match ip { IpAddr::V4(v) => dst[..4].copy_from_slice(&v.octets()), IpAddr::V6(v) => dst[..16].copy_from_slice(&v.octets()) }
}

pub struct Diag { fd: OwnedFd, seq: std::cell::Cell<u32> }

/// Sequence number of the first message in a datagram (all messages of one reply carry the request's).
fn dgram_seq(buf: &[u8]) -> Option<u32> { buf.get(8..12).map(|b| u32::from_ne_bytes([b[0], b[1], b[2], b[3]])) }

impl Diag {
    pub fn open() -> io::Result<Diag> {
        let fd = nl_open(libc::NETLINK_SOCK_DIAG)?;
        // A lost reply must never hang the daemon with a packet waiting for its verdict.
        let tv = libc::timeval { tv_sec: 1, tv_usec: 0 };
        unsafe { libc::setsockopt(fd.as_raw_fd(), libc::SOL_SOCKET, libc::SO_RCVTIMEO, &tv as *const _ as *const libc::c_void,
                                  std::mem::size_of::<libc::timeval>() as libc::socklen_t); }
        Ok(Diag { fd, seq: std::cell::Cell::new(0) })
    }

    /// Next datagram that answers request `seq`. A reply that arrives after its request timed out
    /// used to be read as the answer to the *next* query (wrong socket → wrong owner → wrong verdict):
    /// replies are matched by sequence number now and anything else is dropped.
    fn recv_for(&self, seq: u32, buf: &mut [u8]) -> io::Result<usize> {
        for _ in 0..64 {
            let n = nl_recv(&self.fd, buf, 0)?;
            if dgram_seq(&buf[..n]) == Some(seq) { return Ok(n); }
        }
        Err(io::Error::from_raw_os_error(libc::EIO))
    }

    /// struct inet_diag_req_v2.
    /// Returns the request's sequence number.
    fn request(&self, ty: u16, flags: u16, family: u8, proto: u8, states: u32, id: &[u8; 48]) -> io::Result<u32> {
        // Whatever is still queued belongs to an earlier request: drop it before asking again.
        let mut junk = [0u8; 4096];
        for _ in 0..256 { if nl_recv(&self.fd, &mut junk, libc::MSG_DONTWAIT | libc::MSG_TRUNC).is_err() { break; } }
        let seq = self.seq.get().wrapping_add(1).max(1);
        self.seq.set(seq);
        let mut b = Vec::with_capacity(56);
        b.extend_from_slice(&[family, proto, 0, 0]);
        b.extend_from_slice(&states.to_ne_bytes());
        b.extend_from_slice(id);
        let mut m = nl_msg(ty, NLM_F_REQUEST | flags, &b);
        m[8..12].copy_from_slice(&seq.to_ne_bytes());
        nl_send(&self.fd, &m)?;
        Ok(seq)
    }

    /// All sockets of one family/protocol; `sport` != 0 filters by local port in the kernel.
    pub fn dump(&self, family: u8, proto: u8, states: u32, sport: u16) -> io::Result<Vec<Sock>> {
        let mut id = [0u8; 48];
        id[0..2].copy_from_slice(&sport.to_be_bytes());
        let seq = self.request(SOCK_DIAG_BY_FAMILY, NLM_F_DUMP, family, proto, states, &id)?;
        let mut out = Vec::new();
        let mut buf = [0u8; 32768];
        loop {
            let n = self.recv_for(seq, &mut buf)?;
            let (mut done, mut err) = (false, 0);
            nl_each(&buf[..n], |ty, p| match ty {
                NLMSG_DONE => done = true,
                NLMSG_ERROR => { err = nl_errno(p); done = true; }
                SOCK_DIAG_BY_FAMILY => out.extend(parse_sock(p, proto)),
                _ => {}
            });
            if err != 0 { return Err(io::Error::from_raw_os_error(err)); }
            if done { return Ok(out); }
        }
    }

    /// One TCP socket by its 4-tuple (hash lookup in the kernel, no dump).
    fn exact_tcp(&self, family: u8, local: (IpAddr, u16), remote: (IpAddr, u16)) -> Option<Sock> {
        let mut id = [0u8; 48];
        id[0..2].copy_from_slice(&local.1.to_be_bytes());
        id[2..4].copy_from_slice(&remote.1.to_be_bytes());
        put_ip(&mut id[4..20], local.0);
        put_ip(&mut id[20..36], remote.0);
        id[40..48].fill(0xff);  // INET_DIAG_NOCOOKIE
        let seq = self.request(SOCK_DIAG_BY_FAMILY, 0, family, TCP, !0, &id).ok()?;
        let mut buf = [0u8; 4096];
        let n = self.recv_for(seq, &mut buf).ok()?;
        let mut found = None;
        nl_each(&buf[..n], |ty, p| if ty == SOCK_DIAG_BY_FAMILY { found = parse_sock(p, TCP); });
        found
    }

    /// The local socket an outgoing packet came from.
    pub fn find_socket(&self, f: &Flow) -> Option<Sock> {
        let v4 = f.src.is_ipv4();
        let fams: &[u8] = if v4 { &[AF_INET, AF_INET6] } else { &[AF_INET6] };  // v4 flows may use a dual-stack socket
        if f.proto == TCP {
            if let Some(s) = self.exact_tcp(fams[0], (f.src, f.sport), (f.dst, f.dport)) { return Some(s); }
            // Device-bound or dual-stack socket: port-filtered dump instead.
            for &fam in fams {
                let hit = self.dump(fam, TCP, !(1u32 << TCP_LISTEN), f.sport).ok()?.into_iter()
                    .find(|s| s.dport == f.dport && s.dst == f.dst);
                if hit.is_some() { return hit; }
            }
            return None;
        }
        for &fam in fams {
            let v = self.dump(fam, UDP, !0, f.sport).ok()?;
            let hit = v.iter().find(|s| s.dport == f.dport && s.dst == f.dst)  // connected to this peer
                .or_else(|| v.iter().find(|s| s.dport == 0))                     // unconnected (sendto)
                .or(v.first());
            if hit.is_some() { return hit.cloned(); }
        }
        None
    }

    /// The local socket an incoming packet is for: the listener (TCP) or the bound socket (UDP).
    pub fn find_local(&self, f: &Flow) -> Option<Sock> {
        let fams: &[u8] = if f.dst.is_ipv4() { &[AF_INET, AF_INET6] } else { &[AF_INET6] };
        let states = if f.proto == TCP { 1u32 << TCP_LISTEN } else { !0 };
        for &fam in fams {
            let hit = self.dump(fam, f.proto, states, f.dport).ok()?.into_iter().next();
            if hit.is_some() { return hit; }
        }
        None
    }

    /// Aborts the socket (TCP: ECONNABORTED to the owner). Needs CAP_NET_ADMIN
    /// and CONFIG_INET_DIAG_DESTROY (EOPNOTSUPP without it).
    pub fn destroy(&self, s: &Sock) -> io::Result<()> {
        let seq = self.request(SOCK_DESTROY, NLM_F_ACK, s.family, s.proto, !0, &s.id)?;
        let mut buf = [0u8; 4096];
        let n = self.recv_for(seq, &mut buf)?;
        let mut err = libc::EIO;
        nl_each(&buf[..n], |ty, p| if ty == NLMSG_ERROR { err = nl_errno(p); });
        if err == 0 { Ok(()) } else { Err(io::Error::from_raw_os_error(err)) }
    }
}

// ── processes ──────────────────────────────────────────────────────────────

fn pids() -> impl Iterator<Item = i32> {
    std::fs::read_dir("/proc").into_iter().flatten().flatten().filter_map(|e| e.file_name().to_str()?.parse().ok())
}

fn socket_inodes(pid: i32) -> impl Iterator<Item = u32> {
    std::fs::read_dir(format!("/proc/{pid}/fd")).into_iter().flatten().flatten().filter_map(|e| {
        let l = std::fs::read_link(e.path()).ok()?;
        l.to_str()?.strip_prefix("socket:[")?.strip_suffix(']')?.parse().ok()
    })
}

fn exe_of(pid: i32) -> Option<String> {
    let p = std::fs::read_link(format!("/proc/{pid}/exe")).ok()?;
    let s = p.to_string_lossy();
    Some(s.strip_suffix(" (deleted)").unwrap_or(&s).to_owned())
}

/// wine, wine64, wine-preloader, wine64-preloader, wineserver, wineloader…
/// (system Wine, Proton, Lutris/Bottles runners, CrossOver).
pub fn is_wine_loader(exe: &str) -> bool { basename(exe).starts_with("wine") }

/// wineserver creates and connects every socket of the programs it serves, so holding one says nothing about who asked.
fn is_wine_server(exe: &str) -> bool { basename(exe).starts_with("wineserver") }

fn comm(pid: i32) -> String {
    std::fs::read_to_string(format!("/proc/{pid}/comm")).map(|s| s.trim().to_owned()).unwrap_or_default()
}

/// What a Wine process really runs: the unix path of the mapped Windows .exe
/// (the image named by argv[0] when several are mapped). Without a mapped
/// .exe yet: argv[0] (Z:\ paths made unix), else the loader path.
pub fn wine_ident(pid: i32, loader: &str) -> String {
    let argv0 = std::fs::read(format!("/proc/{pid}/cmdline")).ok()
        .map(|b| String::from_utf8_lossy(b.split(|&c| c == 0).next().unwrap_or(&[])).into_owned()).unwrap_or_default();
    let want = basename(&argv0).to_lowercase();
    let images = wine_images(pid);
    if let Some(p) = images.iter().find(|p| basename(p).to_lowercase() == want).or(images.first()) { return p.clone(); }
    if argv0.len() > 3 && argv0.as_bytes()[..3].eq_ignore_ascii_case(b"z:\\") { return argv0[2..].replace('\\', "/"); }
    if argv0.to_lowercase().ends_with(".exe") { argv0 } else { loader.to_owned() }
}

/// Every Windows .exe mapped into a process, in the order of its address space.
fn wine_images(pid: i32) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    if let Ok(f) = std::fs::File::open(format!("/proc/{pid}/maps")) {
        for line in io::BufReader::new(f).lines().map_while(Result::ok).take(100_000) {
            // address perms offset dev inode   pathname
            let Some(path) = line.splitn(6, ' ').nth(5).map(str::trim_start) else { continue };
            let path = path.strip_suffix(" (deleted)").unwrap_or(path);
            if !path.starts_with('/') || path.len() < 5 || !path.as_bytes()[path.len() - 4..].eq_ignore_ascii_case(b".exe") { continue; }
            if !out.iter().any(|p| p == path) { out.push(path.to_owned()); }
        }
    }
    out
}

/// inode → pid for the wanted socket inodes (first owner wins, wineserver only if nobody else
/// holds the socket). Sees only the caller's own processes unless root.
fn owners_of(want: &HashSet<u32>) -> HashMap<u32, i32> {
    let mut map = HashMap::new();
    let mut servers = Vec::new();
    for pid in pids() {
        if exe_of(pid).is_some_and(|e| is_wine_server(&e)) { servers.push(pid); continue; }
        for ino in socket_inodes(pid) {
            if want.contains(&ino) { map.entry(ino).or_insert(pid); }
        }
        if map.len() == want.len() { return map; }
    }
    for pid in servers {
        for ino in socket_inodes(pid) {
            if want.contains(&ino) { map.entry(ino).or_insert(pid); }
        }
    }
    map
}

/// The addresses of this machine's interfaces.
fn local_addrs() -> HashSet<IpAddr> {
    let mut out = HashSet::new();
    let mut list: *mut libc::ifaddrs = std::ptr::null_mut();
    if unsafe { libc::getifaddrs(&mut list) } != 0 { return out; }
    let mut cur = list;
    while !cur.is_null() {
        let (addr, next) = unsafe { ((*cur).ifa_addr, (*cur).ifa_next) };
        if !addr.is_null() {
            match i32::from(unsafe { (*addr).sa_family }) {
                libc::AF_INET => { out.insert(IpAddr::V4(Ipv4Addr::from(u32::from_be(unsafe { (*addr.cast::<libc::sockaddr_in>()).sin_addr.s_addr })))); }
                libc::AF_INET6 => { out.insert(IpAddr::V6(Ipv6Addr::from(unsafe { (*addr.cast::<libc::sockaddr_in6>()).sin6_addr.s6_addr }))); }
                _ => {}
            }
        }
        cur = next;
    }
    unsafe { libc::freeifaddrs(list); }
    out
}

// ── nftables ───────────────────────────────────────────────────────────────

fn sys_bin(name: &str) -> Option<PathBuf> {
    ["/usr/sbin", "/sbin", "/usr/bin", "/bin"].iter().map(|d| Path::new(d).join(name)).find(|p| crate::trusted_path(p))
}

fn nft_script(cfg: &Config) -> String { nft_ruleset(cfg, false) }

/// The whole table, replaced atomically (declare → delete → define is one transaction).
/// `log_blocked`: the firewall also reports what it stops to the daemon.
fn nft_ruleset(cfg: &Config, log_blocked: bool) -> String {
    let set = |name: &str, ty: &str, v4: bool| {
        let el: Vec<String> = cfg.blacklist.iter().filter(|n| n.addr.is_ipv4() == v4).map(Net::to_string).collect();
        let elements = if el.is_empty() { String::new() } else { format!(" elements = {{ {} }}", el.join(", ")) };
        format!("\tset {name} {{ type {ty}; flags interval; auto-merge;{elements} }}\n")
    };
    let mut s = format!("table inet {TABLE}\ndelete table inet {TABLE}\ntable inet {TABLE} {{\n");
    s += &set("bl4", "ipv4_addr", true);
    s += &set("bl6", "ipv6_addr", false);
    s += &cfg.fw.chains(log_blocked.then_some(NFLOG_GROUP));
    // A missing daemon must never silently allow Wine traffic through the
    // application guard. Logging alone must not interrupt connectivity.
    let log_queue = format!("\t\tmeta l4proto {{ tcp, udp }} queue num {QUEUE_NUM} bypass\n");
    let guard_queue = format!("\t\tmeta l4proto {{ tcp, udp }} queue num {QUEUE_NUM}\n");
    // Loopback first, before the blacklist drops: an entry or range that covers 127.0.0.0/8 or ::1
    // (e.g. "0.0.0.0 - 255.255.255.254") must not cut the machine off from its own local services.
    s += "\tchain in {\n\t\ttype filter hook input priority 0; policy accept;\n\t\tiifname \"lo\" accept\n\t\tip saddr @bl4 drop\n\t\tip6 saddr @bl6 drop\n";
    if cfg.traffic { s += "\t\tct state invalid counter\n"; }
    s += &cfg.fw.hook(true);  // before the queue: the daemon's accept would end this chain
    if cfg.log_conns {
        s += "\t\tct state != new accept\n\t\tmeta pkttype { broadcast, multicast } accept\n";
        s += &log_queue;
    }
    s += "\t}\n\tchain out {\n\t\ttype filter hook output priority 0; policy accept;\n\t\toifname \"lo\" accept\n\t\tip daddr @bl4 drop\n\t\tip6 daddr @bl6 drop\n";
    if cfg.traffic { s += "\t\tct state invalid counter\n"; }
    s += &cfg.fw.hook(false);
    if cfg.guarding() || cfg.log_conns {
        s += "\t\tct state != new accept\n";
        // With the connection log on, LAN flows must reach the daemon too; it applies the exemption itself.
        if cfg.allow_lan && !cfg.log_conns {
            s += "\t\tip daddr { 10.0.0.0/8, 100.64.0.0/10, 169.254.0.0/16, 172.16.0.0/12, 192.168.0.0/16, 224.0.0.0/4, 255.255.255.255 } accept\n";
            s += "\t\tip6 daddr { fc00::/7, fe80::/10, ff00::/8 } accept\n";
        }
        s += if cfg.guarding() { &guard_queue } else { &log_queue };
    }
    s += "\t}\n}\n";
    s
}

fn nft_run(script: &str) -> Result<(), String> { nft_feed(&["-f", "-"], script) }

fn nft_feed(args: &[&str], script: &str) -> Result<(), String> {
    let nft = sys_bin("nft").ok_or("nft not found (emerge net-firewall/nftables)")?;
    let mut c = std::process::Command::new(nft).args(args).env_clear().env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin")
        .current_dir("/").stdin(std::process::Stdio::piped()).stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::piped()).spawn().map_err(|e| format!("nft: {e}"))?;
    let _ = c.stdin.take().map(|mut i| i.write_all(script.as_bytes()));
    let out = c.wait_with_output().map_err(|e| format!("nft: {e}"))?;
    if out.status.success() { Ok(()) } else { Err(format!("nft: {}", String::from_utf8_lossy(&out.stderr).trim())) }
}

/// Loads the table. A kernel without nflog support still gets the rules, only without the blocked-packet log.
fn nft_load(cfg: &Config, log_blocked: bool) -> Result<(), String> {
    match nft_run(&nft_ruleset(cfg, log_blocked)) {
        Err(e) if log_blocked => {
            eprintln!("centurion-netguard: blocked packets are not logged: {e}");
            nft_run(&nft_script(cfg))
        }
        r => r,
    }
}

/// Output of a read-only nft command.
fn nft_read(args: &[&str]) -> Result<String, String> {
    let nft = sys_bin("nft").ok_or("nft not found (emerge net-firewall/nftables)")?;
    let out = std::process::Command::new(nft).args(args).env_clear().env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin")
        .current_dir("/").stdin(std::process::Stdio::null()).output().map_err(|e| format!("nft: {e}"))?;
    if out.status.success() { Ok(String::from_utf8_lossy(&out.stdout).into_owned()) } else { Err(format!("nft: {}", String::from_utf8_lossy(&out.stderr).trim())) }
}

fn nft_remove() { let _ = nft_run(&format!("table inet {TABLE}\ndelete table inet {TABLE}\n")); }

// ── packets ────────────────────────────────────────────────────────────────

#[derive(Debug, PartialEq)]
pub struct Flow { pub proto: u8, pub src: IpAddr, pub sport: u16, pub dst: IpAddr, pub dport: u16, l4: usize }

/// IPv4/IPv6 + TCP/UDP header of a packet starting at the network header.
pub fn parse_packet(p: &[u8]) -> Option<Flow> {
    let (proto, src, dst, l4) = match p.first()? >> 4 {
        4 if p.len() >= 20 => {
            if u16::from_be_bytes([p[6], p[7]]) & 0x1fff != 0 { return None; }  // not the first fragment
            (p[9], ip_of(AF_INET, &p[12..16]), ip_of(AF_INET, &p[16..20]), usize::from(p[0] & 0xf) * 4)
        }
        6 if p.len() >= 40 => {
            // Walk the extension headers to the transport header. A packet with any of them used to
            // come back as "not TCP/UDP" and was accepted without a look at its owner.
            let (mut nh, mut off) = (p[6], 40usize);
            for _ in 0..8 {
                match nh {
                    0 | 43 | 60 => { let h = p.get(off..off + 2)?; nh = h[0]; off += (usize::from(h[1]) + 1) * 8; }  // hop-by-hop, routing, destination options
                    51 => { let h = p.get(off..off + 2)?; nh = h[0]; off += (usize::from(h[1]) + 2) * 4; }           // authentication header
                    44 => {                                                                                             // fragment
                        let h = p.get(off..off + 4)?;
                        if u16::from_be_bytes([h[2], h[3]]) & 0xfff8 != 0 { return None; }  // not the first fragment
                        nh = h[0];
                        off += 8;
                    }
                    _ => break,
                }
            }
            (nh, ip_of(AF_INET6, &p[8..24]), ip_of(AF_INET6, &p[24..40]), off)
        }
        _ => return None,
    };
    if proto != TCP && proto != UDP { return None; }
    let h = p.get(l4..l4 + 4)?;
    Some(Flow { proto, src, dst, sport: u16::from_be_bytes([h[0], h[1]]), dport: u16::from_be_bytes([h[2], h[3]]), l4 })
}

/// Question name of a DNS query (UDP payload).
fn dns_qname(p: &[u8]) -> Option<String> {
    let (mut i, mut name) = (12usize, String::new());
    loop {
        let l = usize::from(*p.get(i)?);
        if l == 0 { break; }
        if l > 63 || name.len() > 253 { return None; }
        if !name.is_empty() { name.push('.'); }
        name.extend(p.get(i + 1..i + 1 + l)?.iter().map(|&c| if c.is_ascii_alphanumeric() || c == b'-' || c == b'_' { c as char } else { '?' }));
        i += 1 + l;
    }
    (!name.is_empty()).then_some(name)
}

// ── NFQUEUE ────────────────────────────────────────────────────────────────

const NFQ_PACKET: u16 = 3 << 8;       // NFNL_SUBSYS_QUEUE << 8 | NFQNL_MSG_PACKET
const NFQ_VERDICT: u16 = 3 << 8 | 1;
const NFQ_CONFIG: u16 = 3 << 8 | 2;
const NFQA_PACKET_HDR: u16 = 1;
const NFQA_VERDICT_HDR: u16 = 2;
const NFQA_PAYLOAD: u16 = 10;
const NFQA_CFG_CMD: u16 = 1;
const NFQA_CFG_PARAMS: u16 = 2;
const NFQA_CFG_MASK: u16 = 4;
const NFQA_CFG_FLAGS: u16 = 5;

struct Queue { fd: OwnedFd }

impl Queue {
    fn nfgen() -> Vec<u8> { let q = QUEUE_NUM.to_be_bytes(); vec![0, 0, q[0], q[1]] }  // AF_UNSPEC, NFNETLINK_V0, res_id

    fn config(&self, attrs: &[u8]) -> io::Result<()> {
        let mut body = Self::nfgen();
        body.extend_from_slice(attrs);
        nl_send(&self.fd, &nl_msg(NFQ_CONFIG, NLM_F_REQUEST | NLM_F_ACK, &body))?;
        let mut buf = [0u8; 4096];
        loop {
            let n = nl_recv(&self.fd, &mut buf, 0)?;
            let mut err = None;
            nl_each(&buf[..n], |ty, p| if ty == NLMSG_ERROR { err = Some(nl_errno(p)); });
            match err { Some(0) => return Ok(()), Some(e) => return Err(io::Error::from_raw_os_error(e)), None => {} }
        }
    }

    fn open() -> io::Result<Queue> {
        let q = Queue { fd: nl_open(libc::NETLINK_NETFILTER)? };
        let mut a = Vec::new();
        nl_attr(&mut a, NFQA_CFG_CMD, &[1, 0, 0, 0]);  // NFQNL_CFG_CMD_BIND
        q.config(&a)?;
        a.clear();
        let mut params = 256u32.to_be_bytes().to_vec();  // copy_range: headers + a DNS question
        params.push(2);                                  // NFQNL_COPY_PACKET
        nl_attr(&mut a, NFQA_CFG_PARAMS, &params);
        q.config(&a)?;
        // Queue full → accept instead of drop; do not segment GSO packets for us. Optional.
        a.clear();
        let flags = (1u32 | 1 << 2).to_be_bytes();  // NFQA_CFG_F_FAIL_OPEN | NFQA_CFG_F_GSO
        nl_attr(&mut a, NFQA_CFG_FLAGS, &flags);
        nl_attr(&mut a, NFQA_CFG_MASK, &flags);
        let _ = q.config(&a);
        let one: libc::c_int = 1;
        unsafe { libc::setsockopt(q.fd.as_raw_fd(), libc::SOL_NETLINK, libc::NETLINK_NO_ENOBUFS,
                                  &one as *const _ as *const libc::c_void, 4); }
        Ok(q)
    }

    fn verdict(&self, id: u32, accept: bool) {
        let mut body = Self::nfgen();
        let mut v = (accept as u32).to_be_bytes().to_vec();  // NF_DROP 0, NF_ACCEPT 1
        v.extend_from_slice(&id.to_be_bytes());
        nl_attr(&mut body, NFQA_VERDICT_HDR, &v);
        let _ = nl_send(&self.fd, &nl_msg(NFQ_VERDICT, NLM_F_REQUEST, &body));
    }
}

/// (packet id, netfilter hook, payload) of an NFQNL_MSG_PACKET message body.
fn queued_packet(p: &[u8]) -> Option<(u32, u8, &[u8])> {
    let (mut a, mut id, mut hook, mut payload) = (p.get(4..)?, None, 0u8, &[][..]);
    while a.len() >= 4 {
        let len = usize::from(u16::from_ne_bytes([a[0], a[1]]));
        if len < 4 || len > a.len() { break; }
        match u16::from_ne_bytes([a[2], a[3]]) & 0x3fff {
            NFQA_PACKET_HDR if len >= 8 => {  // nfqnl_msg_packet_hdr: be32 id, be16 hw_protocol, u8 hook
                id = Some(u32::from_be_bytes([a[4], a[5], a[6], a[7]]));
                if len >= 11 { hook = a[10]; }
            }
            NFQA_PAYLOAD => payload = &a[4..len],
            _ => {}
        }
        a = &a[((len + 3) & !3).min(a.len())..];
    }
    Some((id?, hook, payload))
}

// ── NFLOG (packets the firewall stopped) ───────────────────────────────────

const NFUL_PACKET: u16 = 4 << 8;      // NFNL_SUBSYS_ULOG << 8 | NFULNL_MSG_PACKET
const NFUL_CONFIG: u16 = 4 << 8 | 1;
const NFULA_PACKET_HDR: u16 = 1;
const NFULA_PAYLOAD: u16 = 9;
const NFULA_CFG_CMD: u16 = 1;
const NFULA_CFG_MODE: u16 = 2;

/// The socket bound to NFLOG_GROUP.
fn nflog_open() -> io::Result<OwnedFd> {
    let fd = nl_open(libc::NETLINK_NETFILTER)?;
    let config = |attr: u16, data: &[u8]| -> io::Result<()> {
        let g = NFLOG_GROUP.to_be_bytes();
        let mut body = vec![0, 0, g[0], g[1]];
        nl_attr(&mut body, attr, data);
        nl_send(&fd, &nl_msg(NFUL_CONFIG, NLM_F_REQUEST | NLM_F_ACK, &body))?;
        let mut buf = [0u8; 4096];
        loop {
            let n = nl_recv(&fd, &mut buf, 0)?;
            let mut err = None;
            nl_each(&buf[..n], |ty, p| if ty == NLMSG_ERROR { err = Some(nl_errno(p)); });
            match err { Some(0) => return Ok(()), Some(e) => return Err(io::Error::from_raw_os_error(e)), None => {} }
        }
    };
    config(NFULA_CFG_CMD, &[1])?;  // NFULNL_CFG_CMD_BIND
    let mut mode = 128u32.to_be_bytes().to_vec();  // copy_range: the headers
    mode.extend_from_slice(&[2, 0]);               // NFULNL_COPY_PACKET
    config(NFULA_CFG_MODE, &mode)?;
    Ok(fd)
}

/// (netfilter hook, payload) of an NFULNL_MSG_PACKET message body.
fn nflog_packet(p: &[u8]) -> Option<(u8, &[u8])> {
    let (mut a, mut hook, mut payload) = (p.get(4..)?, None, None);
    while a.len() >= 4 {
        let len = usize::from(u16::from_ne_bytes([a[0], a[1]]));
        if len < 4 || len > a.len() { break; }
        match u16::from_ne_bytes([a[2], a[3]]) & 0x3fff {
            NFULA_PACKET_HDR if len >= 7 => hook = Some(a[6]),  // nfulnl_msg_packet_hdr: be16 hw_protocol, u8 hook
            NFULA_PAYLOAD => payload = Some(&a[4..len]),
            _ => {}
        }
        a = &a[((len + 3) & !3).min(a.len())..];
    }
    Some((hook?, payload?))
}

// ── daemon ─────────────────────────────────────────────────────────────────

static STOP: AtomicBool = AtomicBool::new(false);
static RELOAD: AtomicBool = AtomicBool::new(false);
static WAKE_FD: AtomicI32 = AtomicI32::new(-1);

fn poke() {
    let fd = WAKE_FD.load(Ordering::SeqCst);
    if fd >= 0 { unsafe { libc::write(fd, b"x".as_ptr().cast(), 1); } }
}
extern "C" fn on_stop(_: libc::c_int) { STOP.store(true, Ordering::SeqCst); poke(); }
extern "C" fn on_reload(_: libc::c_int) { RELOAD.store(true, Ordering::SeqCst); poke(); }
fn handler(f: extern "C" fn(libc::c_int)) -> libc::sighandler_t { f as *const () as libc::sighandler_t }

fn now_secs() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs()) }

struct Guard {
    cfg: Config,
    diag: Diag,
    wine: HashSet<i32>,                    // pids running a Wine program, as of the last scan
    servers: HashSet<i32>,                 // pids running wineserver, as of the last scan
    idents: HashMap<i32, (u64, String)>,   // pid → (start time, .exe path)
    images: HashMap<i32, (u64, Vec<String>)>,  // pid → (start time, every mapped .exe)
    scanned: Instant,
    last_pid: String,
    logged: HashMap<String, Instant>,      // same program → same destination: one line a minute
    // connection log
    pending: Vec<Seen>,                    // new flows not written yet
    flushed: Instant,
    seen: HashMap<String, Instant>,        // same program ↔ same peer and port: one line a minute
    owners: HashMap<u32, i32>,             // socket inode → pid
    owners_at: Instant,
    hot: Vec<i32>,                         // pids that owned the last logged sockets, searched first
    acct: Option<Acct>,                    // traffic accounting, while switched on
    nflog: Option<OwnedFd>,                // packets the firewall stopped, while they are being logged
    nft_events: Option<OwnedFd>,           // nftables changes: notices our table being removed
}

/// Per-program traffic accounting: the conntrack socket, the connections open now, and the day ledger.
struct Acct {
    fd: OwnedFd,
    open: HashMap<u32, Conn>,              // conntrack id → owner, from NEW until DESTROY
    ledger: traffic::Ledger,
    dirty: Option<Instant>,                // totals changed and not written yet
    lost_logged: Option<Instant>,          // last "events lost" line (at most one a minute)
}

struct Conn { exe: String, inbound: bool }

/// One new flow waiting for its connection-log line.
struct Seen { ts: u64, inbound: bool, f: Flow, host: String, blocked: bool, firewall: bool, open: bool, inode: u32, uid: Option<u32>, pid: i32, exe: String }

/// Group allowed to read the guard's logs (the GUI reads them directly): `wheel`, as in the polkit
/// rules. Without it the logs stay root-only.
fn log_gid() -> Option<u32> {
    static GID: std::sync::OnceLock<Option<u32>> = std::sync::OnceLock::new();
    *GID.get_or_init(|| std::fs::read_to_string("/etc/group").ok()?.lines().find_map(|l| {
        let f: Vec<&str> = l.split(':').collect();
        (f.len() >= 3 && f[0] == "wheel").then(|| f[2].parse().ok()).flatten()
    }))
}

/// Connection history and DNS names of every user: 0640 root:wheel (0600 without a wheel
/// group), never world-readable. Files created 0644 by older versions are tightened on the next write.
fn append_log(file: &str, max: u64, text: &str) {
    use std::os::unix::fs::MetadataExt;
    let _ = std::fs::DirBuilder::new().recursive(true).mode(0o755).create(LOG_DIR);
    if std::fs::metadata(file).is_ok_and(|m| m.len() > max) { let _ = std::fs::rename(file, format!("{file}.1")); }
    if let Ok(mut f) = std::fs::OpenOptions::new().append(true).create(true).mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(file) {
        let gid = log_gid();
        let mode = if gid.is_some() { 0o640 } else { 0o600 };
        if let Ok(m) = f.metadata() {
            if let Some(g) = gid { if m.gid() != g || m.uid() != 0 { unsafe { libc::fchown(f.as_raw_fd(), 0, g); } } }
            if m.mode() & 0o777 != mode { let _ = f.set_permissions(std::fs::Permissions::from_mode(mode)); }
        }
        let _ = f.write_all(text.as_bytes());
    }
}

/// Newest pid the kernel handed out (5th field of /proc/loadavg).
fn newest_pid() -> String {
    std::fs::read_to_string("/proc/loadavg").ok().and_then(|s| s.split_whitespace().nth(4).map(str::to_owned)).unwrap_or_default()
}

impl Guard {
    /// The Wine program a socket counts against, if any: the one holding it. A socket only wineserver
    /// holds (it makes the connection itself, before the program has the descriptor) could be any of
    /// that user's Wine programs: the first that is not whitelisted answers for it.
    fn owner(&mut self, s: &Sock) -> Option<i32> {
        use std::os::unix::fs::MetadataExt;
        let inode = s.inode;
        let has = |p: &i32| socket_inodes(*p).any(|i| i == inode);
        let live = |p: &&i32| exe_of(**p).is_some_and(|e| is_wine_loader(&e));  // pid not recycled
        if let Some(&p) = self.wine.iter().filter(|p| has(p)).find(live) { return Some(p); }
        // No process was created since the last scan (and it is fresh): the sets are complete.
        let np = newest_pid();
        if np != self.last_pid || np.is_empty() || self.scanned.elapsed() >= Duration::from_millis(250) {
            let (servers, wine) = pids().filter_map(|p| Some((p, exe_of(p).filter(|e| is_wine_loader(e))?)))
                .partition::<Vec<_>, _>(|(_, e)| is_wine_server(e));
            self.servers = servers.into_iter().map(|(p, _)| p).collect();
            self.wine = wine.into_iter().map(|(p, _)| p).collect();
            self.scanned = Instant::now();
            self.last_pid = np;
            self.idents.retain(|p, _| self.wine.contains(p));
            self.images.retain(|p, _| self.wine.contains(p));
            if let Some(&p) = self.wine.iter().find(|p| has(p)) { return Some(p); }
        }
        if !self.servers.iter().any(has) { return None; }
        let same_user: Vec<i32> = self.wine.iter().copied()
            .filter(|p| std::fs::metadata(format!("/proc/{p}")).is_ok_and(|m| m.uid() == s.uid)).collect();
        same_user.into_iter().find(|&p| self.offender(p).is_some())
    }

    /// The image that keeps a Wine process from being allowed online, None if it may. Every mapped .exe
    /// must be whitelisted: mapping a whitelisted file next to one's own does not make a program that file.
    fn offender(&mut self, pid: i32) -> Option<String> {
        let start = crate::proc_start_time(pid).unwrap_or(0);
        let cached = self.images.get(&pid).filter(|(s, _)| *s == start).map(|(_, i)| i.clone());
        let images = cached.unwrap_or_else(|| wine_images(pid));
        if images.is_empty() {  // still starting: only its command line can name it
            let id = self.ident(pid);
            return (!self.cfg.whitelisted(&id)).then_some(id);
        }
        let found = images.iter().find(|i| !self.cfg.whitelisted(i)).cloned();
        self.images.insert(pid, (start, images));
        found
    }

    fn ident(&mut self, pid: i32) -> String {
        let start = crate::proc_start_time(pid).unwrap_or(0);
        if let Some((s, id)) = self.idents.get(&pid) { if *s == start { return id.clone(); } }
        let id = wine_ident(pid, &exe_of(pid).unwrap_or_default());
        if id.to_lowercase().ends_with(".exe") { self.idents.insert(pid, (start, id.clone())); }
        id
    }

    /// true = let the packet through.
    fn decide(&mut self, pkt: &[u8], hook: u8) -> bool {
        let Some(f) = parse_packet(pkt) else { return true };
        let inbound = hook == HOOK_LOCAL_IN;
        let guarded = self.cfg.guarding() && !inbound && !(self.cfg.allow_lan && is_lan(f.dst));
        let peer = if inbound { f.src } else { f.dst };
        let log = self.cfg.log_conns && !peer.is_multicast() && peer != IpAddr::V4(Ipv4Addr::BROADCAST) && self.pending.len() < 1024;
        if !guarded && !log { return true; }
        let sock = if inbound { self.diag.find_local(&f) } else { self.diag.find_socket(&f) };
        let block = if guarded { sock.as_ref().and_then(|s| self.block(&f, pkt, s)) } else { None };
        let accept = block.is_none();
        if log {
            let host = if f.proto == UDP && f.dport == 53 && !inbound { pkt.get(f.l4 + 8..).and_then(dns_qname) } else { None };
            let (pid, exe) = block.unwrap_or_default();
            self.pending.push(Seen { ts: now_secs(), inbound, host: host.unwrap_or_default(), blocked: !accept, firewall: false, open: sock.is_some(),
                                     inode: sock.as_ref().map_or(0, |s| s.inode), uid: sock.as_ref().map(|s| s.uid), pid, exe, f });
        }
        accept
    }

    /// Puts our table back if something removed it (`nft flush ruleset`, a restart of the nftables service).
    fn restore_table(&mut self) {
        if nft_read(&["-t", "list", "table", "inet", TABLE]).is_ok() { return; }
        match load_or_degrade(&self.cfg, self.nflog.is_some()) {
            Ok(err) => {
                eprintln!("centurion-netguard: the nftables table was removed by something else; restored");
                write_state(&self.cfg, err.as_deref());
            }
            Err(e) => {
                eprintln!("centurion-netguard: the nftables table was removed and cannot be restored: {e}");
                write_state(&self.cfg, Some(&e));
            }
        }
    }

    /// A packet the firewall stopped: one blocked line in the connection log, with the program when one is known.
    fn firewall_blocked(&mut self, pkt: &[u8], hook: u8) {
        let Some(f) = parse_packet(pkt) else { return };
        let inbound = hook == HOOK_LOCAL_IN;
        let broadcast = |ip: IpAddr| ip.is_multicast() || ip == IpAddr::V4(Ipv4Addr::BROADCAST);
        if broadcast(f.dst) || self.pending.len() >= 1024 { return; }
        let sock = if inbound { self.diag.find_local(&f) } else { self.diag.find_socket(&f) };
        self.pending.push(Seen { ts: now_secs(), inbound, host: String::new(), blocked: true, firewall: true, open: sock.is_some(),
                                 inode: sock.as_ref().map_or(0, |s| s.inode), uid: sock.as_ref().map(|s| s.uid), pid: 0, exe: String::new(), f });
    }

    /// Some((pid, program)) when the socket belongs to a non-whitelisted Wine program or to a native program on
    /// the block list: cut off and logged.
    fn block(&mut self, f: &Flow, pkt: &[u8], s: &Sock) -> Option<(i32, String)> {
        if s.inode == 0 { return None; }
        let (pid, ident) = self.wine_offender(s).or_else(|| self.app_offender(s))?;
        let killed = self.diag.destroy(s).is_ok();
        self.log(f, pkt, pid, &ident, killed);
        Some((pid, ident))
    }

    fn wine_offender(&mut self, s: &Sock) -> Option<(i32, String)> {
        if !self.cfg.guard { return None; }
        let pid = self.owner(s)?;
        Some((pid, self.offender(pid)?))
    }

    /// The listed native program holding the socket. Wine programs are left to the Wine guard and its whitelist.
    fn app_offender(&mut self, s: &Sock) -> Option<(i32, String)> {
        if self.cfg.apps.is_empty() { return None; }
        let pid = self.socket_owner(s)?;
        let exe = exe_of(pid)?;
        (!is_wine_loader(&exe) && self.cfg.app_blocked(&exe)).then_some((pid, exe))
    }

    /// The process holding one socket, before the verdict: the cached owner if it still holds it, the recent
    /// owners, then only the processes of the socket's user.
    fn socket_owner(&mut self, s: &Sock) -> Option<i32> {
        use std::os::unix::fs::MetadataExt;
        let inode = s.inode;
        let holds = |p: i32| socket_inodes(p).any(|i| i == inode);
        if let Some(&p) = self.owners.get(&inode) { if holds(p) { return Some(p); } }
        let found = self.hot.iter().copied().find(|&p| holds(p))
            .or_else(|| pids().filter(|p| std::fs::metadata(format!("/proc/{p}")).is_ok_and(|m| m.uid() == s.uid)).find(|&p| holds(p)))?;
        if self.owners.len() > 4096 { self.owners.clear(); }
        self.owners.insert(inode, found);
        if !self.hot.contains(&found) {
            if self.hot.len() >= 16 { self.hot.remove(0); }
            self.hot.push(found);
        }
        Some(found)
    }

    /// Guard switched on (or its rules changed): connections that non-whitelisted Wine programs opened
    /// *before* that are established flows the queue rule never sees again, so they used to stay up for
    /// as long as the program kept them. Abort them once here; new ones go through decide().
    fn sweep(&mut self) {
        if !self.cfg.guarding() { return; }
        let Ok(socks) = dump_all(&self.diag) else { return };
        let mut n = 0;
        for s in socks.iter().filter(|s| s.dport != 0 && s.inode != 0 && s.state != 6 && !s.dst.is_loopback() && !s.src.is_loopback()) {
            if self.cfg.allow_lan && is_lan(s.dst) { continue; }
            let f = Flow { proto: s.proto, src: s.src, sport: s.sport, dst: s.dst, dport: s.dport, l4: 0 };
            if self.block(&f, &[], s).is_some() { n += 1; }
        }
        if n > 0 { eprintln!("centurion-netguard: {n} connection(s) of blocked programs were already open — closed"); }
    }

    /// Fills `owners` for the wanted socket inodes: recent owners first, then one /proc pass.
    fn find_owners(&mut self, mut want: HashSet<u32>) {
        if self.owners.len() > 4096 || self.owners_at.elapsed() > Duration::from_secs(60) { self.owners.clear(); self.owners_at = Instant::now(); }
        want.retain(|i| !self.owners.contains_key(i));
        for &pid in &self.hot {
            if want.is_empty() { return; }
            for ino in socket_inodes(pid) { if want.remove(&ino) { self.owners.insert(ino, pid); } }
        }
        if want.is_empty() { return; }
        for (ino, pid) in owners_of(&want) {
            self.owners.insert(ino, pid);
            if !self.hot.contains(&pid) {
                if self.hot.len() >= 16 { self.hot.remove(0); }
                self.hot.push(pid);
            }
        }
    }

    fn program(&mut self, pid: i32, names: &mut HashMap<i32, String>) -> String {
        if let Some(x) = names.get(&pid) { return x.clone(); }
        let x = exe_of(pid).unwrap_or_default();
        let x = if is_wine_loader(&x) { self.ident(pid) } else if x.is_empty() { comm(pid) } else { x };
        names.insert(pid, x.clone());
        x
    }

    /// The connections that are already open when logging is switched on: one
    /// sock_diag dump, written as `existing` lines. A TCP socket on a port this
    /// machine listens on counts as inbound.
    fn snapshot(&mut self) {
        let Ok(socks) = dump_all(&self.diag) else { return };
        let listen: HashSet<u16> = socks.iter().filter(|s| s.proto == TCP && s.state == TCP_LISTEN).map(|s| s.sport).collect();
        let live: Vec<&Sock> = socks.iter().filter(|s| s.dport != 0 && s.state != 6 && !s.dst.is_loopback() && !s.src.is_loopback()
                                                       && !s.dst.is_multicast() && s.dst != IpAddr::V4(Ipv4Addr::BROADCAST)).collect();
        self.find_owners(live.iter().map(|s| s.inode).filter(|&i| i != 0).collect());
        let (ts, mut names, mut out) = (now_secs(), HashMap::new(), String::new());
        for s in live {
            let pid = self.owners.get(&s.inode).copied().unwrap_or(0);
            let exe = if pid == 0 { String::new() } else { self.program(pid, &mut names) };
            let proto = if s.proto == TCP { "tcp" } else { "udp" };
            let inbound = s.proto == TCP && listen.contains(&s.sport);
            let dir = if inbound { "in" } else { "out" };
            let key = format!("{dir}|{proto}|{}|{}|{exe}|{:?}|", s.dst, if inbound { s.sport } else { s.dport }, Some(s.uid));
            if self.seen.insert(key, Instant::now()).is_some() { continue; }
            out += &json!({"ts": ts, "dir": dir, "proto": proto, "remote": s.dst.to_string(), "rport": s.dport, "lport": s.sport,
                           "pid": pid, "uid": s.uid, "exe": exe, "host": "", "blocked": false, "open": true, "existing": true}).to_string();
            out.push('\n');
        }
        if !out.is_empty() { append_log(CONN_LOG, CONN_LOG_MAX, &out); }
    }

    /// Starts or stops traffic accounting to match the configuration.
    fn sync_acct(&mut self) -> Result<(), String> {
        if !self.cfg.traffic {
            self.acct_save();
            self.acct = None;
            return Ok(());
        }
        if self.acct.is_some() { return Ok(()); }
        enable_ct_accounting()?;
        let fd = ct_open().map_err(|e| format!("traffic accounting: conntrack events unavailable: {e} (kernel: NF_CT_NETLINK)"))?;
        self.acct = Some(Acct { fd, open: HashMap::new(), ledger: traffic::Ledger::load(), dirty: None, lost_logged: None });
        Ok(())
    }

    /// Reads the conntrack events queued so far. A new connection is tied to its owner now, while its socket
    /// exists; a connection that ended is counted under the program it was tied to.
    fn acct_read(&mut self) {
        let mut evs = Vec::new();
        {
            let Some(a) = self.acct.as_mut() else { return };
            let mut buf = vec![0u8; 32768];
            for _ in 0..64 {
                let n = match nl_recv(&a.fd, &mut buf, libc::MSG_DONTWAIT) {
                    Ok(n) => n,
                    // The kernel dropped events because the socket buffer was full (a burst of connections):
                    // those connections are not counted. Said in the daemon log, then reading goes on.
                    Err(e) if e.raw_os_error() == Some(libc::ENOBUFS) => {
                        if a.lost_logged.map_or(true, |t| t.elapsed() >= Duration::from_secs(60)) {
                            eprintln!("centurion-netguard: traffic accounting: conntrack events lost (receive buffer full); some connections are not counted");
                            a.lost_logged = Some(Instant::now());
                        }
                        continue;
                    }
                    Err(_) => break,
                };
                nl_each(&buf[..n], |ty, p| if let Some(ev) = traffic::parse_ct(ty, p) { evs.push(ev) });
            }
        }
        let (mut fresh, mut ended) = (Vec::new(), Vec::new());
        let local = if evs.iter().any(|e| !e.delete) { local_addrs() } else { HashSet::new() };
        for ev in evs {
            if ev.delete { ended.push(ev); continue; }
            let f = Flow { proto: ev.proto, src: ev.src, sport: ev.sport, dst: ev.dst, dport: ev.dport, l4: 0 };
            // The addresses say the direction; a socket lookup alone can hit a local socket that merely has the peer's port number.
            match (local.contains(&ev.src), local.contains(&ev.dst)) {
                (true, false) => if let Some(s) = self.diag.find_socket(&f) { fresh.push((ev.id, false, s.inode)); },
                (false, true) => if let Some(s) = self.diag.find_local(&f) { fresh.push((ev.id, true, s.inode)); },
                _ => {}  // within this machine, or forwarded: not traffic of a program with the network
            }
        }
        self.find_owners(fresh.iter().map(|x| x.2).filter(|&i| i != 0).collect());
        let mut names = HashMap::new();
        let mut opened = Vec::with_capacity(fresh.len());
        for (id, inbound, inode) in fresh {
            let pid = self.owners.get(&inode).copied().unwrap_or(0);
            let exe = if pid == 0 { traffic::UNKNOWN.to_owned() } else { self.program(pid, &mut names) };
            opened.push((id, Conn { exe, inbound }));
        }
        let Some(a) = self.acct.as_mut() else { return };
        if a.open.len() > 16384 { a.open.clear(); }
        a.open.extend(opened);
        for ev in ended {
            let Some(c) = a.open.remove(&ev.id) else { continue };
            let (tx, rx) = if c.inbound { (ev.reply_bytes, ev.orig_bytes) } else { (ev.orig_bytes, ev.reply_bytes) };
            a.ledger.add(&traffic::local_day(now_secs()), &c.exe, tx, rx);
            a.dirty.get_or_insert_with(Instant::now);
        }
    }

    fn acct_save(&mut self) {
        let gid = log_gid();
        let Some(a) = self.acct.as_mut() else { return };
        a.dirty = None;
        if let Err(e) = a.ledger.save(gid) { eprintln!("centurion-netguard: traffic totals not saved: {e}"); }
    }

    /// Writes the pending connection-log lines (runs after the verdicts went out).
    fn flush(&mut self) {
        self.flushed = Instant::now();
        let batch = std::mem::take(&mut self.pending);
        self.find_owners(batch.iter().filter(|e| e.pid == 0 && e.inode != 0).map(|e| e.inode).collect());
        if self.seen.len() > 8192 { self.seen.clear(); }
        let mut names: HashMap<i32, String> = HashMap::new();
        let mut out = String::new();
        for e in batch {
            let pid = if e.pid != 0 { e.pid } else { self.owners.get(&e.inode).copied().unwrap_or(0) };
            let exe = if !e.exe.is_empty() || pid == 0 { e.exe } else { self.program(pid, &mut names) };
            let proto = if e.f.proto == TCP { "tcp" } else { "udp" };
            let (dir, remote, rport, lport) = if e.inbound { ("in", e.f.src, e.f.sport, e.f.dport) } else { ("out", e.f.dst, e.f.dport, e.f.sport) };
            let key = format!("{dir}|{proto}|{remote}|{}|{exe}|{:?}|{}|{}", if e.inbound { lport } else { rport }, e.uid, e.host, e.firewall);
            if self.seen.get(&key).is_some_and(|t| t.elapsed() < Duration::from_secs(60)) { continue; }
            self.seen.insert(key, Instant::now());
            let mut line = json!({"ts": e.ts, "dir": dir, "proto": proto, "remote": remote.to_string(), "rport": rport, "lport": lport,
                                  "pid": pid, "uid": e.uid, "exe": exe, "host": e.host, "blocked": e.blocked, "open": e.open});
            if e.firewall { line["by"] = json!("firewall"); }
            out += &line.to_string();
            out.push('\n');
        }
        if !out.is_empty() { append_log(CONN_LOG, CONN_LOG_MAX, &out); }
    }

    fn log(&mut self, f: &Flow, pkt: &[u8], pid: i32, ident: &str, killed: bool) {
        let proto = if f.proto == TCP { "tcp" } else { "udp" };
        let key = format!("{ident}|{proto}|{}|{}", f.dst, f.dport);
        if self.logged.get(&key).is_some_and(|t| t.elapsed() < Duration::from_secs(60)) { return; }
        if self.logged.len() > 4096 { self.logged.clear(); }
        self.logged.insert(key, Instant::now());
        let host = if f.proto == UDP && f.dport == 53 { pkt.get(f.l4 + 8..).and_then(dns_qname) } else { None };
        let line = json!({"ts": now_secs(), "proto": proto, "dst": f.dst.to_string(), "dport": f.dport, "pid": pid,
                          "exe": ident, "host": host.unwrap_or_default(), "killed": killed});
        append_log(LOG_FILE, LOG_MAX, &format!("{line}\n"));
    }
}

/// Notifications of nftables changes.
fn nft_events_open() -> io::Result<OwnedFd> {
    let raw = unsafe { libc::socket(libc::AF_NETLINK, libc::SOCK_RAW | libc::SOCK_CLOEXEC, NETLINK_NETFILTER) };
    if raw < 0 { return Err(io::Error::last_os_error()); }
    let fd = unsafe { OwnedFd::from_raw_fd(raw) };
    let mut sa: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
    sa.nl_family = libc::AF_NETLINK as libc::sa_family_t;
    sa.nl_groups = 1 << (NFNLGRP_NFTABLES - 1);
    let r = unsafe { libc::bind(raw, &sa as *const _ as *const libc::sockaddr, std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t) };
    if r < 0 { return Err(io::Error::last_os_error()); }
    Ok(fd)
}

/// Was this nftables notification the deletion of our table?
fn is_our_table_deleted(ty: u16, p: &[u8]) -> bool {
    if ty != NFT_MSG_DELTABLE || p.first() != Some(&NFPROTO_INET) { return false; }
    let mut a = p.get(4..).unwrap_or(&[]);
    while a.len() >= 4 {
        let len = usize::from(u16::from_ne_bytes([a[0], a[1]]));
        if len < 4 || len > a.len() { break; }
        if u16::from_ne_bytes([a[2], a[3]]) & 0x3fff == NFTA_TABLE_NAME { return a[4..len].strip_suffix(&[0]) == Some(TABLE.as_bytes()); }
        a = &a[((len + 3) & !3).min(a.len())..];
    }
    false
}

/// Loads the table. Firewall rules this kernel refuses must not take the blacklist down with them:
/// Ok(Some(reason)) means the table is in place without the firewall.
fn load_or_degrade(cfg: &Config, log_blocked: bool) -> Result<Option<String>, String> {
    let Err(e) = nft_load(cfg, log_blocked) else { return Ok(None) };
    let without = Config { fw: Firewall { enabled: false, ..cfg.fw.clone() }, ..cfg.clone() };
    if !cfg.fw.active() || nft_run(&nft_script(&without)).is_err() { return Err(e); }
    eprintln!("centurion-netguard: firewall NOT applied: {e}");
    Ok(Some(format!("firewall not applied: {e}")))
}

fn wants_nflog(cfg: &Config) -> bool { cfg.log_conns && cfg.fw.active() }

fn open_nflog() -> Option<OwnedFd> {
    nflog_open().map_err(|e| eprintln!("centurion-netguard: blocked packets are not logged: {e} (kernel: NETFILTER_NETLINK_LOG)")).ok()
}

/// Conntrack events for new and destroyed connections (ctnetlink multicast groups).
fn ct_open() -> io::Result<OwnedFd> {
    let raw = unsafe { libc::socket(libc::AF_NETLINK, libc::SOCK_RAW | libc::SOCK_CLOEXEC, NETLINK_NETFILTER) };
    if raw < 0 { return Err(io::Error::last_os_error()); }
    let fd = unsafe { OwnedFd::from_raw_fd(raw) };
    let mut sa: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
    sa.nl_family = libc::AF_NETLINK as libc::sa_family_t;
    sa.nl_groups = (1 << (NFNLGRP_CONNTRACK_NEW - 1)) | (1 << (NFNLGRP_CONNTRACK_DESTROY - 1));
    let r = unsafe { libc::bind(raw, &sa as *const _ as *const libc::sockaddr, std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t) };
    if r < 0 { return Err(io::Error::last_os_error()); }
    // A larger receive buffer than the default (~200 KiB) so a burst of connections does not overflow it
    // (ENOBUFS, events lost). The daemon is root: FORCE goes past net.core.rmem_max.
    let size: libc::c_int = 4 << 20;
    unsafe { libc::setsockopt(raw, libc::SOL_SOCKET, libc::SO_RCVBUFFORCE, &size as *const _ as *const libc::c_void, 4); }
    Ok(fd)
}

/// Byte counters per connection need nf_conntrack_acct, which is off by default. Switched on once, and logged.
fn enable_ct_accounting() -> Result<(), String> {
    let cur = std::fs::read_to_string(NF_ACCT_SYSCTL).map_err(|e| format!("{NF_ACCT_SYSCTL}: {e} (kernel: NF_CONNTRACK)"))?;
    if cur.trim() == "1" { return Ok(()); }
    std::fs::write(NF_ACCT_SYSCTL, b"1\n").map_err(|e| format!("{NF_ACCT_SYSCTL}: {e}"))?;
    crate::wlog::log("sysctl", "net.netfilter.nf_conntrack_acct <- 1 (per-program traffic accounting)");
    Ok(())
}

/// Takes the daemon lock and records our pid; None if another daemon holds it.
fn lock_daemon() -> Option<std::fs::File> {
    crate::secure_dir(RUN_DIR).ok()?;
    let mut f = std::fs::OpenOptions::new().read(true).write(true).create(true).mode(0o644)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(LOCK_FILE).ok()?;
    if unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 { return None; }
    f.set_len(0).ok()?;
    f.write_all(format!("{}\n", std::process::id()).as_bytes()).ok()?;
    Some(f)
}

/// Pid of the running daemon (its lock is held), readable by any user.
pub fn daemon_pid() -> Option<i32> {
    let mut f = std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(LOCK_FILE).ok()?;
    if unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_SH | libc::LOCK_NB) } == 0 { return None; }  // nobody holds it
    let mut s = String::new();
    f.read_to_string(&mut s).ok()?;
    s.trim().parse().ok().filter(|&p| p > 1)
}

/// What the running daemon really enforces, for `status`: the helper and the GUI
/// compare it with the saved configuration (an older daemon writes nothing here).
fn write_state(cfg: &Config, error: Option<&str>) {
    let _ = crate::write_root_file(STATE_FILE, json!({"guard": cfg.guard, "allow_lan": cfg.allow_lan, "log_conns": cfg.log_conns, "traffic": cfg.traffic, "firewall": cfg.fw.to_json(), "error": error}).to_string().as_bytes());
}

fn applied() -> Value {
    crate::read_root_file(STATE_FILE, 65536).and_then(|s| serde_json::from_str(&s).ok()).unwrap_or(Value::Null)
}

#[derive(Debug, PartialEq, Eq)]
enum ReloadState { Applied, Failed, Pending }

fn reload_state(cfg: &Config, a: &Value) -> ReloadState {
    if a["error"].is_string() { return ReloadState::Failed; }
    if a["guard"] == cfg.guard && a["allow_lan"] == cfg.allow_lan && a["log_conns"] == cfg.log_conns && a["traffic"] == cfg.traffic
        && a["firewall"] == cfg.fw.to_json() {
        ReloadState::Applied
    } else { ReloadState::Pending }
}

pub fn run_daemon() -> i32 {
    if unsafe { libc::geteuid() } != 0 { eprintln!("centurion-netguard: the daemon must run as root"); return 1; }
    let Some(_lock) = lock_daemon() else { eprintln!("centurion-netguard: already running (or {LOCK_FILE} is not usable)"); return 1; };
    let mut pipe = [-1 as libc::c_int; 2];
    if unsafe { libc::pipe2(pipe.as_mut_ptr(), libc::O_CLOEXEC | libc::O_NONBLOCK) } != 0 { return 1; }
    WAKE_FD.store(pipe[1], Ordering::SeqCst);
    unsafe {
        libc::signal(libc::SIGTERM, handler(on_stop));
        libc::signal(libc::SIGINT, handler(on_stop));
        libc::signal(libc::SIGHUP, handler(on_reload));
    }
    let mut start_error = None;
    let cfg = Config::load().unwrap_or_else(|e| {
        eprintln!("centurion-netguard: {e}; new incoming connections are blocked until this is fixed");
        start_error = Some(format!("{e} (new incoming connections are blocked until this is fixed)"));
        Config::safe_mode()
    });
    let diag = match Diag::open().and_then(|d| d.dump(AF_INET, TCP, !0, 1).map(|_| d)) {
        Ok(d) => d,
        Err(e) => { eprintln!("centurion-netguard: sock_diag unavailable: {e} (kernel: INET_DIAG, INET_TCP_DIAG, INET_UDP_DIAG)"); return 1; }
    };
    // Preserve the old rules if binding the queue fails. Once open, nft_load
    // replaces the table as a single nftables transaction (no unguarded gap).
    let queue = match Queue::open() {
        Ok(q) => q,
        Err(e) => { eprintln!("centurion-netguard: cannot bind NFQUEUE {QUEUE_NUM}: {e} (kernel: NETFILTER_NETLINK_QUEUE)"); return 1; }
    };
    let _ = std::fs::remove_file(STATE_FILE);
    let nflog = if wants_nflog(&cfg) { open_nflog() } else { None };
    match load_or_degrade(&cfg, nflog.is_some()) {
        Ok(e) => start_error = start_error.or(e),
        Err(e) => {
            eprintln!("centurion-netguard: {e} (kernel: NF_TABLES, NF_TABLES_INET, NFT_QUEUE, NFT_CT, NF_CONNTRACK)");
            return 1;
        }
    }
    write_state(&cfg, start_error.as_deref());
    eprintln!("centurion-netguard: started (guard {}, connection log {}, firewall {}, {} blacklisted, {} whitelisted)",
              if cfg.guard { "on" } else { "off" }, if cfg.log_conns { "on" } else { "off" }, if cfg.fw.active() { "on" } else { "off" },
              cfg.blacklist.len(), cfg.whitelist.len());
    let nft_events = nft_events_open().map_err(|e| eprintln!("centurion-netguard: a removed table will not be noticed: {e}")).ok();
    let mut g = Guard { cfg, diag, wine: HashSet::new(), servers: HashSet::new(), idents: HashMap::new(), images: HashMap::new(), scanned: Instant::now(),
                        last_pid: String::new(), logged: HashMap::new(), pending: Vec::new(), flushed: Instant::now(),
                        seen: HashMap::new(), owners: HashMap::new(), owners_at: Instant::now(), hot: Vec::new(), acct: None, nflog, nft_events };
    if let Err(e) = g.sync_acct() { eprintln!("centurion-netguard: {e}"); write_state(&g.cfg, Some(&e)); }
    if g.cfg.log_conns { g.snapshot(); }
    g.sweep();
    let mut buf = [0u8; 8192];
    while !STOP.load(Ordering::SeqCst) {
        if RELOAD.swap(false, Ordering::SeqCst) {
            match Config::load() {
                Ok(c) => {
                    let was_logging = g.cfg.log_conns;
                    // Prepare sockets while the old policy remains active. Only
                    // publish cfg after the atomic nft transaction has succeeded.
                    let new_nflog = if wants_nflog(&c) && g.nflog.is_none() {
                        open_nflog()
                    } else { None };
                    let log_blocked = wants_nflog(&c) && (g.nflog.is_some() || new_nflog.is_some());
                    let acct = if c.traffic && g.acct.is_none() {
                        enable_ct_accounting().and_then(|_| ct_open().map_err(|e| format!(
                            "traffic accounting: conntrack events unavailable: {e} (kernel: NF_CT_NETLINK)")))
                            .map(|fd| Some(Acct { fd, open: HashMap::new(), ledger: traffic::Ledger::load(), dirty: None, lost_logged: None }))
                    } else { Ok(None) };
                    let result = acct.and_then(|new_acct| {
                        nft_load(&c, log_blocked).map(|()| new_acct)
                    });
                    match result {
                        Ok(new_acct) => {
                            if !c.traffic { g.acct_save(); g.acct = None; }
                            else if let Some(a) = new_acct { g.acct = Some(a); }
                            g.cfg = c;
                            if !wants_nflog(&g.cfg) { g.nflog = None; }
                            else if let Some(fd) = new_nflog { g.nflog = Some(fd); }
                            write_state(&g.cfg, None);
                            eprintln!("centurion-netguard: configuration reloaded");
                            if g.cfg.log_conns && !was_logging { g.snapshot(); }
                            g.sweep();  // guard switched on, or a program left the whitelist
                        }
                        Err(e) => {
                            // The previous sockets and policy remain installed.
                            eprintln!("centurion-netguard: reload failed, old rules retained: {e}");
                            write_state(&g.cfg, Some(&format!("reload failed (old rules retained): {e}")));
                        }
                    }
                }
                Err(e) => {
                    eprintln!("centurion-netguard: reload failed, keeping the old rules: {e}");
                    write_state(&g.cfg, Some(&format!("reload failed (old rules retained): {e}")));
                }
            }
        }
        // Pending log lines and traffic totals wake the loop when their batch window ends; otherwise it sleeps
        // until a packet, a conntrack event or a signal.
        let mut wait = if g.pending.is_empty() { None } else { Some(FLUSH_GAP.saturating_sub(g.flushed.elapsed())) };
        if let Some(d) = g.acct.as_ref().and_then(|a| a.dirty) {
            let left = TRAFFIC_FLUSH.saturating_sub(d.elapsed());
            wait = Some(wait.map_or(left, |w| w.min(left)));
        }
        let wait = wait.map_or(-1, |d| d.as_millis() as libc::c_int);
        let ct_fd = g.acct.as_ref().map_or(-1, |a| a.fd.as_raw_fd());  // a negative fd is ignored by poll
        let log_fd = g.nflog.as_ref().map_or(-1, |f| f.as_raw_fd());
        let event_fd = g.nft_events.as_ref().map_or(-1, |f| f.as_raw_fd());
        let mut fds = [libc::pollfd { fd: queue.fd.as_raw_fd(), events: libc::POLLIN, revents: 0 },
                       libc::pollfd { fd: pipe[0], events: libc::POLLIN, revents: 0 },
                       libc::pollfd { fd: ct_fd, events: libc::POLLIN, revents: 0 },
                       libc::pollfd { fd: log_fd, events: libc::POLLIN, revents: 0 },
                       libc::pollfd { fd: event_fd, events: libc::POLLIN, revents: 0 }];
        if unsafe { libc::poll(fds.as_mut_ptr(), 5, wait) } < 0 { continue; }  // EINTR: flags are checked above
        if fds[1].revents != 0 { let mut b = [0u8; 64]; unsafe { libc::read(pipe[0], b.as_mut_ptr().cast(), b.len()); } }
        if fds[0].revents != 0 {
            for _ in 0..64 {  // then back to poll, so a flood cannot starve SIGTERM/SIGHUP
                let Ok(n) = nl_recv(&queue.fd, &mut buf, libc::MSG_DONTWAIT) else { break };
                let mut pkts: Vec<(u32, bool)> = Vec::new();
                nl_each(&buf[..n], |ty, p| if ty == NFQ_PACKET {
                    if let Some((id, hook, payload)) = queued_packet(p) { pkts.push((id, g.decide(payload, hook))); }
                });
                for (id, accept) in pkts { queue.verdict(id, accept); }
            }
        }
        if fds[2].revents != 0 { g.acct_read(); }
        if fds[3].revents != 0 {
            for _ in 0..64 {
                let Some(Ok(n)) = g.nflog.as_ref().map(|f| nl_recv(f, &mut buf, libc::MSG_DONTWAIT)) else { break };
                let mut pkts: Vec<(u8, Vec<u8>)> = Vec::new();
                nl_each(&buf[..n], |ty, p| if ty == NFUL_PACKET {
                    if let Some((hook, payload)) = nflog_packet(p) { pkts.push((hook, payload.to_vec())); }
                });
                for (hook, p) in pkts { g.firewall_blocked(&p, hook); }
            }
        }
        if fds[4].revents != 0 {
            let mut removed = false;
            while let Some(r) = g.nft_events.as_ref().map(|f| nl_recv(f, &mut buf, libc::MSG_DONTWAIT)) {
                match r {
                    Ok(n) => nl_each(&buf[..n.min(buf.len())], |ty, p| removed |= is_our_table_deleted(ty, p)),
                    Err(e) if e.raw_os_error() == Some(libc::ENOBUFS) => removed = true,  // notices were lost: look
                    Err(_) => break,
                }
            }
            if removed { g.restore_table(); }
        }
        if !g.pending.is_empty() && g.flushed.elapsed() >= FLUSH_GAP { g.flush(); }
        if g.acct.as_ref().and_then(|a| a.dirty).is_some_and(|d| d.elapsed() >= TRAFFIC_FLUSH) { g.acct_save(); }
    }
    g.acct_save();
    nft_remove();
    let _ = std::fs::remove_file(STATE_FILE);
    eprintln!("centurion-netguard: stopped");
    0
}

// ── helper ops (netguard-helper) ───────────────────────────────────────────

fn state_name(s: &Sock) -> &'static str {
    if s.proto == UDP { return if s.dport != 0 { "CONNECTED" } else { "UNCONN" }; }
    match s.state { 1 => "ESTAB", 2 => "SYN-SENT", 3 => "SYN-RECV", 4 => "FIN-WAIT-1", 5 => "FIN-WAIT-2", 6 => "TIME-WAIT",
                    7 => "CLOSE", 8 => "CLOSE-WAIT", 9 => "LAST-ACK", 10 => "LISTEN", 11 => "CLOSING", _ => "?" }
}

fn dump_all(d: &Diag) -> Result<Vec<Sock>, String> {
    let mut out = Vec::new();
    let mut err = None;
    for (fam, proto) in [(AF_INET, TCP), (AF_INET6, TCP), (AF_INET, UDP), (AF_INET6, UDP)] {
        match d.dump(fam, proto, !0, 0) { Ok(v) => out.extend(v), Err(e) => err = Some(e) }
    }
    match err { Some(e) if out.is_empty() => Err(format!("sock_diag: {e} (kernel: INET_DIAG, INET_TCP_DIAG, INET_UDP_DIAG)")), _ => Ok(out) }
}

// Only for the presentation snapshot. The daemon's existing ownership and verdict
// algorithms remain unchanged. Read process identity on both sides of the fd scan.
fn proc_start_ticks(pid: i32) -> Option<u64> {
    parse_proc_start_ticks(&std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?, pid)
}
fn parse_proc_start_ticks(stat: &str, pid: i32) -> Option<u64> {
    let (prefix, tail) = stat.rsplit_once(')')?;
    if prefix.split_once(" (")?.0.parse::<i32>().ok()? != pid { return None; }
    tail.split_whitespace().nth(19)?.parse().ok()
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct ProcIdentity { ticks: u64, exe: String }
fn proc_identity(pid: i32) -> Option<ProcIdentity> {
    let ticks = proc_start_ticks(pid)?;
    let exe = std::fs::read_link(format!("/proc/{pid}/exe")).ok()?.into_os_string().into_string().ok()?;
    if exe.is_empty() || proc_start_ticks(pid)? != ticks { return None; }
    Some(ProcIdentity { ticks, exe })
}
struct SnapshotOwner { name: String, exe: String, wine: bool, identity: ProcIdentity, inodes: HashSet<u32> }
fn socket_cookie(s: &Sock) -> Option<String> {
    let bytes = &s.id[40..48];
    if bytes.iter().all(|&b| b == 255) { return None; }
    Some(bytes.iter().map(|b| format!("{b:02x}")).collect::<Vec<_>>().join(""))
}

/// Every TCP/UDP socket with its owner. Unprivileged: owners of the caller's own processes only.
pub fn list() -> Value {
    let cfg = Config::load().unwrap_or_default();
    let socks = match Diag::open().map_err(|e| format!("sock_diag: {e}")).and_then(|d| dump_all(&d)) {
        Ok(s) => s,
        Err(e) => return json!({"ok": false, "error": e}),
    };
    let want: HashSet<u32> = socks.iter().map(|s| s.inode).filter(|&i| i != 0).collect();
    let owners = owners_of(&want);
    let boot = crate::bootguard::boot_id();
    let netns = std::fs::read_link("/proc/self/ns/net").ok().and_then(|p| p.into_os_string().into_string().ok());
    // Recheck ownership and identity after the initial owner lookup; a PID alone is not evidence.
    let mut procs: HashMap<i32, Option<SnapshotOwner>> = HashMap::new();
    let conns: Vec<Value> = socks.iter().map(|s| {
        let pid = owners.get(&s.inode).copied().unwrap_or(0);
        let owner = if pid == 0 { None } else {
            procs.entry(pid).or_insert_with(|| {
                let before = proc_identity(pid)?;
                let inodes: HashSet<u32> = socket_inodes(pid).filter(|i| want.contains(i)).collect();
                let exe = exe_of(pid)?;
                let (name, display, wine) = if is_wine_loader(&exe) {
                    let id = wine_ident(pid, &exe); (basename(&id).to_owned(), id, true)
                } else { (comm(pid), exe, false) };
                if proc_identity(pid)? != before { return None; }
                Some(SnapshotOwner { name, exe: display, wine, identity: before, inodes })
            }).as_ref().filter(|owner| owner.inodes.contains(&s.inode))
        };
        // Preserve the reported PID as a candidate if the owner disappears, but
        // do not attach another process's program name or verified identity.
        let (name, exe, wine) = owner.map(|o| (o.name.as_str(),o.exe.as_str(),o.wine)).unwrap_or(("","",false));
        let mut c = json!({"proto": if s.proto == TCP { "tcp" } else { "udp" }, "state": state_name(s),
               "local": s.src.to_string(), "lport": s.sport,
               "remote": if s.dport == 0 { String::new() } else { s.dst.to_string() }, "rport": s.dport,
               "uid": s.uid, "pid": pid, "name": name, "exe": exe, "wine": wine,
               "bl": s.dport != 0 && cfg.blacklisted(s.dst),
               "boot_id": boot, "netns": netns, "family": s.family,
               "socket_cookie": socket_cookie(s), "socket_inode": s.inode});
        if let Some(owner) = owner {
            c["process_start_ticks"] = json!(owner.identity.ticks.to_string());
            c["process_exe"] = json!(owner.identity.exe);
        }
        if s.dport == 0 && (s.proto == UDP || s.state == TCP_LISTEN) {
            let (state, conditional) = cfg.fw.listen_state(s.proto == TCP, s.sport);
            c["fw"] = json!(state);
            c["fw_conditional"] = json!(conditional);
            c["fw_default"] = json!(cfg.fw.relies_on_default(s.proto == TCP, s.sport));
        }
        c
    }).collect();
    json!({"ok": true, "root": unsafe { libc::geteuid() } == 0, "conns": conns})
}

/// Kernel options from KCONFIG the running kernel lacks; None when its config is not readable.
fn kconfig_missing(cfg: &Config) -> Option<Vec<&'static str>> {
    let mut u: libc::utsname = unsafe { std::mem::zeroed() };
    unsafe { libc::uname(&mut u); }
    let rel = unsafe { std::ffi::CStr::from_ptr(u.release.as_ptr()) }.to_string_lossy().into_owned();
    let text = sys_bin("gzip").filter(|_| Path::new("/proc/config.gz").exists())
        .and_then(|gz| std::process::Command::new(gz).args(["-dc", "/proc/config.gz"]).env_clear().output().ok())
        .filter(|o| o.status.success()).map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .or_else(|| std::fs::read_to_string(format!("/boot/config-{rel}")).ok())?;
    Some(missing_kernel_options(cfg, &text, Path::new("/sys/module/nfnetlink_log").is_dir()))
}

fn missing_kernel_options(cfg: &Config, text: &str, nflog_loaded: bool) -> Vec<&'static str> {
    let set: HashSet<&str> = text.lines().filter_map(|line| {
        let (key, value) = line.strip_prefix("CONFIG_")?.split_once('=')?;
        matches!(value, "y" | "m").then_some(key)
    }).collect();
    let reject = if cfg.fw.uses_reject() { KCONFIG_REJECT } else { &[] };
    let log = if wants_nflog(cfg) { KCONFIG_FW_LOG } else { &[] };
    KCONFIG.iter().chain(reject).chain(log).copied().filter(|&key| {
        !set.contains(key) && !(key == "NETFILTER_NETLINK_LOG" && nflog_loaded)
    }).collect()
}

pub fn status(full: bool) -> Value {
    let (cfg, err) = match Config::load() { Ok(c) => (c, None), Err(e) => (Config::default(), Some(e)) };
    let mut v = cfg.to_json();
    v["firewall"]["shadowed"] = json!(cfg.fw.shadowed());
    let pid = daemon_pid();
    v["ok"] = json!(true);
    v["running"] = json!(pid.is_some());
    v["pid"] = json!(pid);
    if pid.is_some() {
        let a = applied();
        v["reload_state"] = json!(match reload_state(&cfg, &a) {
            ReloadState::Applied => "applied",
            ReloadState::Failed => "error",
            ReloadState::Pending => "pending",
        });
        v["applied"] = a;
    }
    if let Some(e) = err { v["config_error"] = json!(e); }
    if full {
        v["nft"] = json!(sys_bin("nft").is_some());
        if let Some(m) = kconfig_missing(&cfg) { v["missing"] = json!(m); }
    }
    v
}

fn reload_daemon() {
    if let Some(p) = daemon_pid() { unsafe { libc::kill(p, libc::SIGHUP); } }
}

fn strings(v: &Value) -> Vec<String> {
    v.as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.trim().to_owned())).collect()).unwrap_or_default()
}

/// Aborts every live socket whose peer matches. Returns (destroyed, last error).
fn kill_where(pred: impl Fn(&Sock) -> bool) -> (usize, Option<String>) {
    let Ok(d) = Diag::open() else { return (0, Some("sock_diag unavailable".into())) };
    let (mut n, mut err) = (0, None);
    for s in dump_all(&d).unwrap_or_default().iter().filter(|s| s.dport != 0 && pred(s)) {
        match d.destroy(s) {
            Ok(()) => n += 1,
            Err(e) if e.raw_os_error() == Some(libc::EOPNOTSUPP) => err = Some("kernel lacks CONFIG_INET_DIAG_DESTROY".to_owned()),
            Err(e) => err = Some(e.to_string()),
        }
    }
    (n, err)
}

/// Forgets the tracked flows a new blocking rule now decides, so their next packet meets the rule
/// instead of passing as "established". This is what ends a UDP flow, which has no socket state to abort.
fn forget_blocked_flows(fw: &Firewall, fresh: &[String]) {
    let Ok(fd) = nl_open(NETLINK_NETFILTER) else { return };
    let local = local_addrs();
    if nl_send(&fd, &nl_msg(CT_GET, NLM_F_REQUEST | NLM_F_DUMP, &[0, 0, 0, 0])).is_err() { return; }
    let (mut buf, mut doomed, mut done) = (vec![0u8; 65536], Vec::new(), false);
    while !done {
        let Ok(n) = nl_recv(&fd, &mut buf, 0) else { break };
        nl_each(&buf[..n], |ty, p| {
            if ty == NLMSG_DONE || ty == NLMSG_ERROR { done = true; }
            let Some(ev) = traffic::parse_ct(ty, p) else { return };
            let hit = match (local.contains(&ev.src), local.contains(&ev.dst)) {
                (true, false) => fw.newly_blocks(fresh, false, ev.proto, ev.dport, ev.dst),
                (false, true) => fw.newly_blocks(fresh, true, ev.proto, ev.dport, ev.src),
                _ => false,
            };
            if hit { doomed.push(ev); }
        });
    }
    for ev in doomed { let _ = nl_send(&fd, &nl_msg(traffic::CT_DELETE, NLM_F_REQUEST, &traffic::delete_request(&ev))); }
}

/// Aborts the live connections a new blocking rule now decides: conntrack would keep them alive otherwise.
/// A TCP socket on a port this machine listens on is inbound; inbound UDP has no such marker and is left
/// to forget_blocked_flows.
fn kill_blocked(fw: &Firewall, fresh: &[String]) -> usize {
    forget_blocked_flows(fw, fresh);
    let Ok(d) = Diag::open() else { return 0 };
    let listen: HashSet<u16> = dump_all(&d).unwrap_or_default().iter().filter(|s| s.proto == TCP && s.state == TCP_LISTEN).map(|s| s.sport).collect();
    kill_where(|s| {
        if s.state == 6 || s.dst.is_loopback() || s.src.is_loopback() { return false; }
        let inbound = s.proto == TCP && listen.contains(&s.sport);
        fw.newly_blocks(fresh, inbound, s.proto, if inbound { s.sport } else { s.dport }, s.dst)
    }).0
}

/// What the kernel really holds: this table (terse), hit counters per rule, and every other filter chain
/// on the same hooks. A drop there wins over an allow here. Root only: nft cannot list for other users.
fn fw_inspect(cfg: &Config) -> Value {
    // The text form: `nft -j` exists only when nftables was built with JSON support.
    let text = match nft_read(&["-t", "list", "ruleset"]) { Ok(t) => t, Err(e) => return json!({"ok": false, "error": e}) };
    let (present, hits, others) = parse_ruleset(&text);
    // iptables-legacy rules are invisible to nft; this file lists the tables it has loaded.
    let legacy: Vec<String> = ["/proc/net/ip_tables_names", "/proc/net/ip6_tables_names"].iter()
        .filter_map(|f| std::fs::read_to_string(f).ok()).flat_map(|t| t.lines().map(str::to_owned).collect::<Vec<_>>()).collect();
    let rules = nft_read(&["-t", "list", "table", "inet", TABLE]).unwrap_or_default();
    json!({"ok": true, "present": present, "active": cfg.fw.active(), "hits": hits, "others": others, "legacy": legacy, "rules": rules})
}

/// From `nft list ruleset`: is our table there, its counters by rule comment, and the other tables' input / output chains.
fn parse_ruleset(text: &str) -> (bool, serde_json::Map<String, Value>, Vec<Value>) {
    let (mut present, mut hits, mut others) = (false, serde_json::Map::new(), Vec::new());
    let (mut family, mut table, mut chain) = ("", "", "");
    for line in text.lines().map(str::trim) {
        let w: Vec<&str> = line.split_whitespace().collect();
        match w.as_slice() {
            ["table", f, t, "{"] => { (family, table) = (f, t); present |= *f == "inet" && *t == TABLE; continue; }
            ["chain", c, "{"] => { chain = c; continue; }
            ["type", _, "hook", hook, "priority", rest @ ..] if table != TABLE && (*hook == "input" || *hook == "output") => {
                let priority = rest.first().map_or("", |p| p.trim_end_matches(';'));
                let policy = rest.iter().position(|x| *x == "policy").and_then(|i| rest.get(i + 1)).map_or("accept", |p| p.trim_end_matches(';'));
                others.push(json!({"family": family, "table": table, "chain": chain, "hook": hook, "priority": priority, "policy": policy}));
                continue;
            }
            _ => {}
        }
        if table != TABLE { continue; }
        let Some(tag) = line.rsplit_once("comment \"").and_then(|(_, t)| t.strip_suffix('"')) else { continue };
        let Some(i) = w.iter().position(|x| *x == "counter") else { continue };
        let (Some(n), Some(bytes)) = (w.get(i + 2).and_then(|v| v.parse::<u64>().ok()), w.get(i + 4).and_then(|v| v.parse::<u64>().ok())) else { continue };
        let key = if tag == "default" { format!("default_{chain}") } else { tag.to_owned() };
        let slot = hits.entry(key).or_insert_with(|| json!({"packets": 0, "bytes": 0}));
        slot["packets"] = json!(slot["packets"].as_u64().unwrap_or(0) + n);
        slot["bytes"] = json!(slot["bytes"].as_u64().unwrap_or(0) + bytes);
    }
    (present, hits, others)
}

/// Group `label` lets go of its blacklist entries (a removed list, or a subscription update with `replace`):
/// an entry another group or the user also added stays. Done before the new entries of an update are added,
/// so an entry the new list writes differently (a range, "1.2.3.5/24") is not removed again after it was re-added.
fn drop_group(cfg: &mut Config, label: &str) {
    for others in cfg.shared.values_mut() { others.retain(|l| l != label); }
    let old: Vec<String> = cfg.labels.iter().filter(|(_, l)| l.as_str() == label).map(|(n, _)| n.clone()).collect();
    let mut gone = HashSet::new();
    for n in old {
        cfg.labels.remove(&n);
        match cfg.shared.get_mut(&n).filter(|o| !o.is_empty()).map(|o| o.remove(0)) {
            Some(next) if !next.is_empty() => { cfg.labels.insert(n, next); }
            Some(_) => {}  // the user added it too: it stays, without a group
            None => { gone.insert(n); }
        }
    }
    cfg.blacklist.retain(|n| !gone.contains(&n.to_string()));
    cfg.shared.retain(|_, o| !o.is_empty());
}

/// An entry of a published list that must not be applied: it reaches this machine's own or local addresses.
/// Bogon lists such as FireHOL level 1 name these ranges because they should not come from the internet.
fn covers_local(n: &Net) -> bool {
    let unspecified = matches!(n.addr, IpAddr::V4(v) if v.octets()[0] == 0) || n.addr.is_unspecified();
    unspecified || n.addr.is_loopback() || is_lan(n.addr)
        || ["127.0.0.1", "10.0.0.1", "100.64.0.1", "169.254.0.1", "172.16.0.1", "192.168.0.1", "224.0.0.1", "::1", "fc00::1", "fe80::1", "ff02::1"]
            .iter().filter_map(|a| a.parse().ok()).any(|ip| n.contains(ip))
}

fn expected_firewall_matches(req: &Value, fw: &Firewall) -> bool {
    req.get("expected_firewall").map_or(true, |expected| expected == &fw.to_json())
}

#[cfg(test)]
#[test]
fn firewall_expected_snapshot_guard() {
    let fw = Firewall::default();
    assert!(expected_firewall_matches(&json!({}), &fw));
    assert!(expected_firewall_matches(&json!({"expected_firewall": fw.to_json()}), &fw));
    assert!(!expected_firewall_matches(&json!({"expected_firewall": null}), &fw));
    let mut changed = fw.clone(); changed.enabled = true;
    assert!(!expected_firewall_matches(&json!({"expected_firewall": fw.to_json()}), &changed));
}

/// One request of netguard-helper. `list` and `status` work for any user; the rest is root-only.
pub fn handle(req: &Value) -> Value {
    let op = req["op"].as_str().unwrap_or("");
    match op {
        "list" => return list(),
        "status" => return status(true),
        "whois" => return lookup(req["query"].as_str().or(req["ip"].as_str()).unwrap_or(""), req["asn"] == true),
        "wifi" => return crate::wifi::status(req),
        "wifi_hw" => return crate::wifi::hardware(req),
        "wifi_scan" => return crate::wifi::scan(req),
        "wifi_rescan" => return crate::wifi::rescan(req),
        _ => {}
    }
    if unsafe { libc::geteuid() } != 0 { return json!({"ok": false, "needs_root": true, "error": "this operation needs root (pkexec)"}); }
    // Read, change, write: two requests at once must not overwrite each other.
    let lock = match crate::helper_lock("netguard-helper") { Ok(l) => l, Err(e) => return json!({"ok": false, "error": e}) };
    let mut cfg = match Config::load() { Ok(c) => c, Err(e) => return json!({"ok": false, "error": e}) };
    let mut killed = 0;
    let mut fresh_blocks = Vec::new();
    let mut skipped = 0;
    match op {
        "set" => {
            if let Some(b) = req["guard"].as_bool() { cfg.guard = b; }
            if let Some(b) = req["allow_lan"].as_bool() { cfg.allow_lan = b; }
            if let Some(b) = req["log_conns"].as_bool() { cfg.log_conns = b; }
            if let Some(b) = req["traffic"].as_bool() { cfg.traffic = b; }
        }
        "blacklist" => {
            let label = req["label"].as_str().map(|l| l.chars().filter(|c| !c.is_control()).take(64).collect::<String>().trim().to_owned())
                .filter(|l| !l.is_empty());
            // Clear first: "replace the list" must keep what this same request adds.
            if req["remove_all"] == true { cfg.blacklist.clear(); }
            if req["replace"] == true { if let Some(l) = &label { drop_group(&mut cfg, l); } }
            for l in strings(&req["drop_groups"]) { drop_group(&mut cfg, &l); }
            let mut have: HashSet<String> = cfg.blacklist.iter().map(Net::to_string).collect();
            let mut added = Vec::new();
            for s in strings(&req["add"]) {
                let Some(nets) = parse_nets(&s) else { return json!({"ok": false, "error": format!("not an IP address, CIDR block or range: {s}")}) };
                for n in nets {
                    if req["public_only"] == true && covers_local(&n) { skipped += 1; continue; }
                    if n.contains(IpAddr::V4(Ipv4Addr::LOCALHOST)) || n.contains(IpAddr::V6(Ipv6Addr::LOCALHOST)) {
                        return json!({"ok": false, "error": format!("{s} covers the loopback address; local programs would stop working")});
                    }
                    let key = n.to_string();
                    let new = have.insert(key.clone());
                    if new { cfg.blacklist.push(n); added.push(n); }
                    let adding = label.clone().unwrap_or_default();
                    match cfg.labels.get(&key) {
                        None if new && !adding.is_empty() => { cfg.labels.insert(key, adding); }
                        Some(first) if *first != adding => {
                            let others = cfg.shared.entry(key).or_default();
                            if !others.contains(&adding) { others.push(adding); }
                        }
                        _ => {}  // already in this group, or already added by hand
                    }
                }
            }
            let del: HashSet<String> = strings(&req["remove"]).iter().filter_map(|s| Net::parse(s)).map(|n| n.to_string()).collect();
            cfg.blacklist.retain(|n| !del.contains(&n.to_string()));
            let have: HashSet<String> = cfg.blacklist.iter().map(Net::to_string).collect();
            cfg.labels.retain(|k, _| have.contains(k));
            cfg.shared.retain(|k, _| have.contains(k));
            if cfg.blacklist.len() > MAX_BLACKLIST { return json!({"ok": false, "error": "blacklist is full"}); }
            if !added.is_empty() { killed = kill_where(|s| added.iter().any(|n| n.contains(s.dst))).0; }
        }
        "whitelist" => {
            for s in strings(&req["add"]) {
                if !valid_entry(&s) { return json!({"ok": false, "error": "invalid whitelist entry"}); }
                if !cfg.whitelist.contains(&s) { cfg.whitelist.push(s); }
            }
            let del = strings(&req["remove"]);
            cfg.whitelist.retain(|w| !del.contains(w));
            if cfg.whitelist.len() > MAX_WHITELIST { return json!({"ok": false, "error": "whitelist is full"}); }
        }
        "block_apps" => {
            for s in strings(&req["add"]) {
                if !valid_entry(&s) { return json!({"ok": false, "error": "invalid program entry"}); }
                if !cfg.apps.contains(&s) { cfg.apps.push(s); }
            }
            let del = strings(&req["remove"]);
            cfg.apps.retain(|w| !del.contains(w));
            if cfg.apps.len() > MAX_WHITELIST { return json!({"ok": false, "error": "program block list is full"}); }
        }
        "firewall" => {
            // Compare under the existing helper lock, before apply, nft validation, save or reload.
            // Optional for compatibility with CLI and older clients.
            if !expected_firewall_matches(req, &cfg.fw) {
                return json!({"ok": false, "conflict": true, "error": "saved firewall changed; refresh and review before applying"});
            }
            let before = cfg.fw.blocking();
            if let Err(e) = cfg.fw.apply(req) { return json!({"ok": false, "error": e}); }
            // Ask the kernel before saving: a rule it refuses would keep the whole new rule set from loading.
            if sys_bin("nft").is_some() {
                let probe = Config { fw: cfg.fw.clone(), ..Config::default() };
                if let Err(e) = nft_feed(&["-c", "-f", "-"], &nft_script(&probe)) { return json!({"ok": false, "error": e}); }
            }
            fresh_blocks = cfg.fw.blocking().into_iter().filter(|s| !before.contains(s)).collect();
        }
        "fw_inspect" => return fw_inspect(&cfg),
        "kill" => {
            let (proto, lport, rport) = (if req["proto"] == "udp" { UDP } else { TCP }, req["lport"].as_u64().unwrap_or(0), req["rport"].as_u64().unwrap_or(0));
            let remote: Option<IpAddr> = req["remote"].as_str().and_then(|s| s.parse().ok());
            let (n, err) = kill_where(|s| s.proto == proto && u64::from(s.sport) == lport && u64::from(s.dport) == rport && Some(s.dst) == remote);
            return match (n, err) {
                (0, Some(e)) => json!({"ok": false, "error": e}),
                (0, None) => json!({"ok": false, "error": "connection no longer exists"}),
                _ => json!({"ok": true, "killed": n}),
            };
        }
        "clear_log" => {
            let _ = std::fs::remove_file(LOG_FILE);
            let _ = std::fs::remove_file(format!("{LOG_FILE}.1"));
            return json!({"ok": true});
        }
        "clear_conn_log" => {
            let _ = std::fs::remove_file(CONN_LOG);
            let _ = std::fs::remove_file(format!("{CONN_LOG}.1"));
            return json!({"ok": true});
        }
        _ => return json!({"ok": false, "error": "unknown op"}),
    }
    if let Err(e) = cfg.save() { return json!({"ok": false, "error": e}); }
    let saved = std::fs::metadata(CONF_FILE).and_then(|m| m.modified()).ok();
    reload_daemon();
    drop(lock);
    // Wait for the daemon, so the answer says what is really in force. A long blacklist takes seconds;
    // a newer change to the file means this request's state will never be reported.
    for _ in 0..500 {
        if daemon_pid().is_none() || reload_state(&cfg, &applied()) != ReloadState::Pending { break; }
        if std::fs::metadata(CONF_FILE).and_then(|m| m.modified()).ok() != saved { break; }
        std::thread::sleep(Duration::from_millis(40));
    }
    // After the reload: a program that reconnects at once must already meet the new rule.
    let state = applied();
    if !fresh_blocks.is_empty() && state["error"].is_null() && state["firewall"] == cfg.fw.to_json() { killed += kill_blocked(&cfg.fw, &fresh_blocks); }
    let mut v = status(false);
    v["killed"] = json!(killed);
    v["skipped"] = json!(skipped);
    v
}

// ── whois ──────────────────────────────────────────────────────────────────

/// A whois hostname we are willing to dial: a real DNS name whose first label is `whois`.
/// Stops a registry `refer:` from pointing this process at an arbitrary host on port 43.
fn whois_host_ok(host: &str) -> bool {
    if !(4..254).contains(&host.len()) || host.starts_with('.') || host.ends_with('.') || host.contains("..") {
        return false;
    }
    let mut labels = host.split('.');
    let Some(first) = labels.next() else { return false };
    if first != "whois" { return false; }
    let mut n = 1;
    for l in labels {
        n += 1;
        if l.is_empty() || l.len() > 63 || l.starts_with('-') || l.ends_with('-')
            || !l.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
        {
            return false;
        }
    }
    n >= 2
}

/// Resolved address of a whois server. Private, loopback, link-local and multicast
/// answers are refused so a poisoned referral cannot probe the LAN or localhost.
fn public_whois_ip(ip: IpAddr) -> bool {
    let ip = unmap(ip);
    if ip.is_loopback() || ip.is_unspecified() || is_lan(ip) { return false; }
    match ip {
        IpAddr::V4(v) => {
            let o = v.octets();
            o[0] != 0 && o[0] < 224
                && !(o[0] == 192 && o[1] == 0 && o[2] == 2)
                && !(o[0] == 198 && o[1] == 51 && o[2] == 100)
                && !(o[0] == 203 && o[1] == 0 && o[2] == 113)
        }
        IpAddr::V6(v) => { let s = v.segments(); !(s[0] == 0x2001 && s[1] == 0x0db8) }
    }
}

/// One query to a whois server (TCP 43): bounded in time and size.
fn whois_query(server: &str, q: &str) -> Result<String, String> {
    use std::net::{TcpStream, ToSocketAddrs};
    if !whois_host_ok(server) { return Err(format!("{server}: refused whois server")); }
    let addr = (server, 43).to_socket_addrs().map_err(|e| format!("{server}: {e}"))?
        .find(|a| public_whois_ip(a.ip())).ok_or_else(|| format!("{server}: no public address"))?;
    let mut s = TcpStream::connect_timeout(&addr, Duration::from_millis(2500)).map_err(|e| format!("{server}: {e}"))?;
    let _ = s.set_read_timeout(Some(Duration::from_millis(2500)));
    let _ = s.set_write_timeout(Some(Duration::from_millis(2500)));
    s.write_all(format!("{q}\r\n").as_bytes()).map_err(|e| format!("{server}: {e}"))?;
    let (mut out, mut buf, end) = (Vec::new(), [0u8; 8192], Instant::now() + Duration::from_secs(4));
    while out.len() < 256 * 1024 && Instant::now() < end {
        match s.read(&mut buf) { Ok(0) | Err(_) => break, Ok(n) => out.extend_from_slice(&buf[..n]) }
    }
    if out.is_empty() { return Err(format!("{server}: no answer")); }
    Ok(String::from_utf8_lossy(&out).chars().filter(|c| !c.is_control() || *c == '\n' || *c == '\t').collect())
}

fn whois_fields(text: &str) -> Vec<(String, &str)> {
    text.lines().filter(|l| !l.starts_with('%') && !l.starts_with('#'))
        .filter_map(|l| l.split_once(':').map(|(k, v)| (k.trim().to_lowercase(), v.trim()))).filter(|(_, v)| !v.is_empty()).collect()
}

/// The server a reply points to (IANA `refer:`, ARIN `ReferralServer: whois://host`, a registry's `Registrar WHOIS Server:`).
fn whois_referral(text: &str) -> Option<String> {
    let v = whois_fields(text).into_iter().find(|(k, _)| matches!(k.as_str(), "refer" | "referralserver" | "registrar whois server" | "whois server"))?.1.to_lowercase();
    let host = v.strip_prefix("whois://").unwrap_or(&v).trim_end_matches('/').trim_end_matches(":43");
    (!v.starts_with("rwhois") && whois_host_ok(host)).then(|| host.to_owned())
}

/// LACNIC writes blocks with the trailing zero octets left out: "200.3.12/22" is 200.3.12.0/22.
fn full_cidr(v: &str) -> String {
    match v.split_once('/') {
        Some((a, p)) if !a.contains(':') && a.matches('.').count() < 3 => format!("{a}{}/{p}", ".0".repeat(3 - a.matches('.').count())),
        _ => v.to_owned(),
    }
}

/// The handful of fields worth a glance. ARIN lists the covering networks first, the other registries the closest one first.
fn whois_summary(text: &str, last: bool) -> Vec<[String; 2]> {
    let f = whois_fields(text);
    let pick = |keys: &[&str]| keys.iter().find_map(|k| {
        let mut hits = f.iter().filter(|(key, _)| key == k).map(|(_, v)| *v);
        if last { hits.next_back() } else { hits.next() }
    });
    let mut out: Vec<[String; 2]> = Vec::new();
    for (label, keys) in [("Range", &["inetnum", "inet6num", "netrange"][..]), ("CIDR", &["cidr", "route", "route6"]),
                          ("Network", &["netname"]), ("Organisation", &["orgname", "org-name", "organization", "owner", "descr"]),
                          ("Country", &["country"]), ("AS", &["originas", "origin", "aut-num"]),
                          ("Abuse contact", &["orgabuseemail", "abuse-mailbox"]), ("Registered", &["regdate", "created"]),
                          ("Updated", &["updated", "last-modified"]), ("Status", &["nettype", "status"])] {
        if let Some(v) = pick(keys) { out.push([label.to_owned(), if label == "Range" { full_cidr(v) } else { v.to_owned() }]); }
    }
    // RIPE / APNIC: "% Abuse contact for '…' is 'abuse@example.net'"
    if !out.iter().any(|r| r[0] == "Abuse contact") {
        if let Some(a) = text.lines().find(|l| l.starts_with("% Abuse contact")).and_then(|l| l.rsplit('\'').nth(1)) {
            out.push(["Abuse contact".to_owned(), a.to_owned()]);
        }
    }
    out
}

#[derive(Debug, PartialEq)]
enum WhoisQuery { Ip(IpAddr), Asn(u32), Domain(String) }

/// An address, a CIDR block (its network address is asked for), `AS123`, or a domain name in ASCII.
fn whois_target(q: &str) -> Option<WhoisQuery> {
    let q = q.trim().trim_end_matches('.');
    if let Ok(ip) = q.parse::<IpAddr>() { return Some(WhoisQuery::Ip(unmap(ip))); }
    if q.contains('/') { return Net::parse(q).map(|n| WhoisQuery::Ip(n.addr)); }
    if let Some(n) = q.get(..2).filter(|p| p.eq_ignore_ascii_case("as")).and_then(|_| q[2..].parse().ok()) { return Some(WhoisQuery::Asn(n)); }
    let d = q.to_ascii_lowercase();
    let label_ok = |l: &str| !l.is_empty() && l.len() <= 63 && !l.starts_with('-') && !l.ends_with('-') && l.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_');
    (d.len() <= 253 && d.contains('.') && d.split('.').all(label_ok)).then_some(WhoisQuery::Domain(d))
}

/// The same question as each registry wants it asked.
fn whois_question(server: &str, q: &WhoisQuery) -> String {
    match q {
        WhoisQuery::Ip(ip) if server == "whois.arin.net" => format!("n + {ip}"),
        WhoisQuery::Ip(ip) => ip.to_string(),
        WhoisQuery::Asn(n) if server == "whois.arin.net" => format!("a {n}"),
        WhoisQuery::Asn(n) => format!("AS{n}"),
        WhoisQuery::Domain(d) if server == "whois.verisign-grs.com" => format!("domain {d}"),
        WhoisQuery::Domain(d) if server == "whois.denic.de" => format!("-T dn,ace {d}"),
        WhoisQuery::Domain(d) => d.clone(),
    }
}

/// `keys` looked up in the records, last server first (the most specific one answers last).
fn first_of<'a>(records: &[Vec<(String, &'a str)>], keys: &[&str]) -> Option<&'a str> {
    keys.iter().find_map(|k| records.iter().rev().find_map(|f| f.iter().find(|(key, v)| key == k && !v.to_ascii_lowercase().contains("redacted")).map(|(_, v)| *v)))
}

/// Every distinct value of `keys`, from the first record that has any.
fn all_of(records: &[Vec<(String, &str)>], keys: &[&str], clean: fn(&str) -> String) -> String {
    for f in records.iter().rev() {
        let mut out: Vec<String> = Vec::new();
        for (_, v) in f.iter().filter(|(k, _)| keys.contains(&k.as_str())) {
            let c = clean(v);
            if !out.iter().any(|x| x.eq_ignore_ascii_case(&c)) { out.push(c); }
        }
        if !out.is_empty() { return out.join(", "); }
    }
    String::new()
}

fn domain_summary(texts: &[&str]) -> Vec<[String; 2]> {
    let records: Vec<_> = texts.iter().map(|t| whois_fields(t)).collect();
    let mut out: Vec<[String; 2]> = Vec::new();
    for (label, keys) in [("Domain", &["domain name", "domain"][..]), ("Registrar", &["registrar", "sponsoring registrar"]),
                          ("Registered", &["creation date", "created", "registered on", "registered"]),
                          ("Updated", &["updated date", "last updated", "last-update", "changed", "modified"]),
                          ("Expires", &["registry expiry date", "registrar registration expiration date", "expiry date", "expires", "expires on", "paid-till"]),
                          ("DNSSEC", &["dnssec"]), ("Organisation", &["registrant organization", "org", "organisation"]),
                          ("Country", &["registrant country", "country"]), ("Abuse contact", &["registrar abuse contact email", "abuse-mailbox"])] {
        if let Some(v) = first_of(&records, keys) { out.push([label.to_owned(), v.to_owned()]); }
    }
    // "clientTransferProhibited https://icann.org/epp#…": the name is the information.
    let status = all_of(&records, &["domain status", "status"], |v| v.split_whitespace().next().unwrap_or("").to_owned());
    if !status.is_empty() { out.push(["Status".to_owned(), status]); }
    let ns = all_of(&records, &["name server", "nserver", "nameserver"], |v| v.split_whitespace().next().unwrap_or("").to_lowercase());
    if !ns.is_empty() { out.push(["Name servers".to_owned(), ns]); }
    out
}

fn asn_summary(text: &str) -> Vec<[String; 2]> {
    let records = [whois_fields(text)];
    let mut out: Vec<[String; 2]> = Vec::new();
    for (label, keys) in [("AS", &["aut-num", "asnumber"][..]), ("AS name", &["as-name", "asname"]), ("Organisation", &["orgname", "org-name", "organization", "owner", "descr"]),
                          ("Country", &["country"]), ("Registered", &["regdate", "created"]), ("Updated", &["updated", "last-modified"]),
                          ("Abuse contact", &["orgabuseemail", "abuse-mailbox"])] {
        if let Some(v) = first_of(&records, keys) { out.push([label.to_owned(), v.to_owned()]); }
    }
    out
}

/// Team Cymru's routing table answer, `AS | IP | BGP Prefix | CC | Registry | Allocated | AS Name`
/// (for an AS query: `AS | CC | Registry | Allocated | AS Name`), as summary rows.
fn cymru_summary(text: &str) -> Vec<[String; 2]> {
    let mut out: Vec<[String; 2]> = Vec::new();
    let mut origins: Vec<String> = Vec::new();
    let mut add = |label: &str, v: &str| if !v.is_empty() && v != "NA" && !out.iter().any(|r: &[String; 2]| r[0] == label) { out.push([label.to_owned(), v.to_owned()]); };
    for line in text.lines() {
        let f: Vec<&str> = line.split('|').map(str::trim).collect();
        if f[0].parse::<u32>().is_err() { continue; }  // the header, or "NA": not announced
        let (prefix, reg, alloc, name) = match f.len() { 7 => (f[2], f[4], f[5], f[6]), 5 => ("", f[2], f[3], f[4]), _ => continue };
        if !origins.contains(&format!("AS{}", f[0])) { origins.push(format!("AS{}", f[0])); }
        add("BGP prefix", prefix);
        add("AS name", name);
        add("RIR", reg);
        add("Allocated", alloc);
    }
    if origins.is_empty() { add("Origin AS", "not announced"); } else { add("Origin AS", &origins.join(", ")); }
    out
}

/// Registry record of an address, CIDR block, AS number or domain: IANA names the registry, the registry
/// answers, a domain's registry may name its registrar (at most four servers). With `asn`, Team Cymru's
/// routing data is added. Runs as the calling user; nothing is sent for local addresses.
pub fn lookup(query: &str, asn: bool) -> Value {
    let Some(q) = whois_target(query) else { return json!({"ok": false, "error": "not an IP address, CIDR block, AS number (AS123) or domain name"}) };
    let kind = match q { WhoisQuery::Ip(_) => "ip", WhoisQuery::Asn(_) => "asn", WhoisQuery::Domain(_) => "domain" };
    if let WhoisQuery::Ip(ip) = q {
        if ip.is_loopback() || ip.is_unspecified() || is_lan(ip) {
            return json!({"ok": true, "kind": kind, "ip": ip.to_string(), "server": "", "raw": "", "hops": [],
                          "summary": [["Network", "private / local address — not registered anywhere"]]});
        }
    }
    let (mut next, mut hops, mut warning): (String, Vec<(String, String)>, Option<String>) = ("whois.iana.org".to_owned(), Vec::new(), None);
    for _ in 0..4 {
        match whois_query(&next, &whois_question(&next, &q)) {
            Ok(t) => hops.push((std::mem::take(&mut next), t)),
            Err(e) if hops.is_empty() => return json!({"ok": false, "error": e}),
            Err(e) => { warning = Some(e); break; }
        }
        let (server, text) = &hops[hops.len() - 1];
        match whois_referral(text) { Some(n) if !hops.iter().any(|(s, _)| *s == n) && n != *server => next = n, _ => break }
    }
    let (server, raw) = hops.last().map_or(("", ""), |(s, t)| (s.as_str(), t.as_str()));
    // IANA's own record describes the TLD or the block, not what was asked: used only if nothing else answered.
    let answers: Vec<&str> = hops.iter().filter(|(s, _)| hops.len() == 1 || s != "whois.iana.org").map(|(_, t)| t.as_str()).collect();
    let mut summary = match &q {
        WhoisQuery::Ip(_) => whois_summary(raw, server.ends_with("arin.net")),
        WhoisQuery::Asn(_) => asn_summary(raw),
        WhoisQuery::Domain(_) => domain_summary(&answers),
    };
    let mut asn_error = None;
    let routed = match &q { WhoisQuery::Ip(ip) => Some(ip.to_string()), WhoisQuery::Asn(n) => Some(format!("AS{n}")), WhoisQuery::Domain(_) => None };
    if let Some(target) = routed.filter(|_| asn) {
        match whois_query("whois.cymru.com", &format!(" -v {target}")) {
            Ok(t) => {
                let have: Vec<String> = summary.iter().map(|r| r[0].clone()).collect();
                summary.extend(cymru_summary(&t).into_iter().filter(|r| !have.contains(&r[0])));
                hops.push(("whois.cymru.com".to_owned(), t));
            }
            Err(e) => asn_error = Some(e),
        }
    }
    let server = server_of(&hops);
    let raw = hops.iter().rfind(|(s, _)| s != "whois.cymru.com").map_or("", |(_, t)| t.as_str());
    let mut v = json!({"ok": true, "kind": kind, "query": query.trim(), "server": server, "summary": summary, "raw": raw,
                       "hops": hops.iter().map(|(s, t)| json!({"server": s, "text": t})).collect::<Vec<_>>()});
    if let WhoisQuery::Ip(ip) = q { v["ip"] = json!(ip.to_string()); }
    if let Some(w) = warning.or(asn_error) { v["warning"] = json!(w); }
    v
}

/// The registry that gave the answer (the routing lookup is an addition, not the source).
fn server_of(hops: &[(String, String)]) -> &str {
    hops.iter().rfind(|(s, _)| s != "whois.cymru.com").map_or("", |(s, _)| s.as_str())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn snapshot_process_lifetime_fields() {
        let line = format!("42 (weird ) name) S{} 18446744073709551615 0", " 0".repeat(18));
        assert_eq!(parse_proc_start_ticks(&line, 42), Some(u64::MAX));
        assert_eq!(parse_proc_start_ticks(&line, 43), None);
        assert_eq!(parse_proc_start_ticks("42 (foo) S 0", 42), None);
        let mut packet = [0u8; 72]; packet[0] = AF_INET;
        packet[44..52].fill(255);
        let s = parse_sock(&packet, TCP).unwrap(); assert!(socket_cookie(&s).is_none());
        packet[44..52].copy_from_slice(&[0,1,2,3,4,5,6,7]);
        let s = parse_sock(&packet, TCP).unwrap(); assert_eq!(socket_cookie(&s).as_deref(), Some("0001020304050607"));
    }

    #[test]
    fn nets() {
        let n = Net::parse("10.1.2.3/8").unwrap();
        assert_eq!(n.to_string(), "10.0.0.0/8");
        assert!(n.contains("10.200.0.1".parse().unwrap()) && !n.contains("11.0.0.1".parse().unwrap()));
        assert!(n.contains("::ffff:10.9.9.9".parse().unwrap()));
        assert_eq!(Net::parse("1.2.3.4").unwrap().to_string(), "1.2.3.4");
        assert_eq!(Net::parse("2001:db8::1/32").unwrap().to_string(), "2001:db8::/32");
        assert!(Net::parse("0.0.0.0/0").is_none() && Net::parse("1.2.3.4/33").is_none() && Net::parse("x; drop").is_none());
    }

    #[test]
    fn whitelist() {
        let c = Config { whitelist: vec!["Game.exe".into(), "/games/a/".into(), "/opt/x/tool.exe".into()], ..Config::default() };
        assert!(c.whitelisted("/home/u/pfx/drive_c/GAME.EXE") && c.whitelisted("C:\\dir\\game.exe"));
        assert!(c.whitelisted("/games/a/bin/launcher.exe") && c.whitelisted("/opt/x/tool.exe"));
        assert!(!c.whitelisted("/opt/y/tool.exe") && !c.whitelisted("/games/b/launcher.exe"));
        let c = Config { whitelist: vec!["Çalis.exe".into()], ..Config::default() };
        assert!(c.whitelisted("C:\\x\\ÇALIS.EXE") && !c.whitelisted("C:\\x\\calis.exe"));
    }

    #[test]
    fn app_block_list() {
        let c = Config { apps: vec!["firefox".into(), "/opt/spotify/".into(), "/usr/bin/curl".into()], ..Config::default() };
        assert!(c.guarding() && !c.guard);
        assert!(c.app_blocked("/usr/lib/firefox/firefox") && c.app_blocked("/opt/spotify/spotify") && c.app_blocked("/usr/bin/curl"));
        assert!(!c.app_blocked("/usr/local/bin/curl") && !c.app_blocked("/usr/bin/wget") && !c.app_blocked("/opt/spotify2/x"));
        assert!(!c.whitelisted("/usr/lib/firefox/firefox"));   // separate lists
        // Round trip through the saved configuration.
        let back = Config { ..c.clone() }.to_json();
        assert_eq!(back["block_apps"], json!(["firefox", "/opt/spotify/", "/usr/bin/curl"]));
    }

    #[test]
    fn packets() {
        let mut p = vec![0x45, 0, 0, 40, 0, 0, 0x40, 0, 64, 17, 0, 0, 192, 168, 1, 5, 8, 8, 8, 8, 0xC0, 0x00, 0, 53, 0, 0, 0, 0];
        p.extend_from_slice(&[0; 12]);
        p.extend_from_slice(b"\x03www\x07example\x03com\x00\x00\x01\x00\x01");
        let f = parse_packet(&p).unwrap();
        assert_eq!((f.proto, f.sport, f.dport, f.dst.to_string().as_str()), (UDP, 0xC000, 53, "8.8.8.8"));
        assert_eq!(dns_qname(&p[f.l4 + 8..]).as_deref(), Some("www.example.com"));
        let mut v6 = vec![0x60, 0, 0, 0, 0, 20, 6, 64];
        v6.extend_from_slice(&[0; 15]); v6.push(1);
        v6.extend_from_slice(&[0x20, 1, 0xd, 0xb8]); v6.extend_from_slice(&[0; 12]);
        v6.extend_from_slice(&[0x9c, 0x40, 0x01, 0xbb]);
        let f = parse_packet(&v6).unwrap();
        // The same packet behind a hop-by-hop options header (8 bytes) and a first fragment header.
        let mut ext = v6[..40].to_vec();
        ext[6] = 0;
        ext.extend_from_slice(&[44, 0, 0, 0, 0, 0, 0, 0]);
        ext.extend_from_slice(&[6, 0, 0, 1, 0, 0, 0, 0]);
        ext.extend_from_slice(&v6[40..]);
        let fe = parse_packet(&ext).unwrap();
        assert!(fe.proto == TCP && fe.sport == f.sport && fe.dport == f.dport);
        ext[50] = 0x01;  // fragment offset != 0: no transport header in this one
        assert!(parse_packet(&ext).is_none());
        assert_eq!((f.proto, f.sport, f.dport, f.dst.to_string().as_str()), (TCP, 40000, 443, "2001:db8::"));
        assert!(parse_packet(&[0x45, 0, 0]).is_none());
    }

    #[test]
    fn drop_group_removes_only_that_group() {
        let mut c = Config::default();
        for s in ["7.7.7.7", "8.8.8.8", "6.6.6.6"] { c.blacklist.push(Net::parse(s).unwrap()); c.labels.insert(s.to_owned(), "FireHOL".to_owned()); }
        c.shared.insert("7.7.7.7".to_owned(), vec!["other".to_owned()]);
        c.shared.insert("8.8.8.8".to_owned(), vec![String::new()]);
        drop_group(&mut c, "FireHOL");
        assert_eq!(c.blacklist.iter().map(Net::to_string).collect::<Vec<_>>(), ["7.7.7.7", "8.8.8.8"]);
        assert_eq!((c.labels.get("7.7.7.7").map(String::as_str), c.labels.get("8.8.8.8")), (Some("other"), None));
        let mut c = Config::default();
        for (s, l) in [("1.2.3.0/24", "FireHOL"), ("5.6.7.8", "FireHOL"), ("9.9.9.9", "manual")] {
            c.blacklist.push(Net::parse(s).unwrap());
            c.labels.insert(s.to_owned(), l.to_owned());
        }
        drop_group(&mut c, "FireHOL");
        assert_eq!(c.blacklist.iter().map(Net::to_string).collect::<Vec<_>>(), vec!["9.9.9.9"]);
        assert_eq!(c.labels.len(), 1);
    }

    #[test]
    fn nft_rules() {
        let mut c = Config::default();
        assert!(!nft_script(&c).contains("queue") && !nft_script(&c).contains("elements"));
        c.guard = true;
        c.blacklist = vec![Net::parse("1.2.3.4").unwrap(), Net::parse("2001:db8::/32").unwrap()];
        let s = nft_script(&c);
        // The application guard queues without `bypass`: a missing daemon must block, not allow (fail-closed).
        assert!(s.contains("elements = { 1.2.3.4 }") && s.contains("elements = { 2001:db8::/32 }")
                && s.contains("queue num 19536\n") && !s.contains("bypass"));
        assert!(s.matches("queue num").count() == 1 && s.contains("192.168.0.0/16"));
        c.log_conns = true;  // both hooks queued, LAN exemption left to the daemon
        let s = nft_script(&c);
        assert!(s.matches("queue num").count() == 2 && !s.contains("192.168.0.0/16") && s.contains("iifname \"lo\" accept"));
        c.guard = false;
        assert!(nft_script(&c).matches("queue num").count() == 2);
        // A native app block list alone queues outbound flows the same fail-closed way as the Wine guard.
        let mut a = Config { apps: vec!["firefox".into()], ..Config::default() };
        let s = nft_script(&a);
        assert!(s.matches("queue num").count() == 1 && s.contains("queue num 19536\n") && !s.contains("bypass"));
        a.apps.clear();
        assert!(!nft_script(&a).contains("queue"));
        // Loopback is accepted before the blacklist drops in both chains, whatever is enabled.
        for (guard, log) in [(false, false), (true, false), (false, true)] {
            c.guard = guard;
            c.log_conns = log;
            let s = nft_script(&c);
            assert!(s.find("iifname \"lo\" accept").unwrap() < s.find("ip saddr @bl4 drop").unwrap());
            assert!(s.find("oifname \"lo\" accept").unwrap() < s.find("ip daddr @bl4 drop").unwrap());
            assert_eq!(s.matches("ifname \"lo\" accept").count(), 2);
        }
    }

    #[test]
    fn ranges() {
        let p = |s: &str| parse_nets(s).map(|v| v.iter().map(Net::to_string).collect::<Vec<_>>().join(" "));
        assert_eq!(p("1.2.3.0 - 1.2.3.255").as_deref(), Some("1.2.3.0/24"));
        assert_eq!(p("192.0.2.10-192.0.2.20").as_deref(), Some("192.0.2.10/31 192.0.2.12/30 192.0.2.16/30 192.0.2.20"));
        assert_eq!(p("10.0.0.0 - 10.255.255.255").as_deref(), Some("10.0.0.0/8"));
        assert_eq!(p("0.0.0.0 - 0.0.0.3").as_deref(), Some("0.0.0.0/30"));
        assert_eq!(p("2001:db8:: - 2001:db8:ffff:ffff:ffff:ffff:ffff:ffff").as_deref(), Some("2001:db8::/32"));
        assert_eq!(p("255.255.255.254 - 255.255.255.255").as_deref(), Some("255.255.255.254/31"));
        assert_eq!(p("5.6.7.8/16").as_deref(), Some("5.6.0.0/16"));
        assert!(p("0.0.0.0 - 255.255.255.255").is_none() && p(":: - ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff").is_none());
        assert!(p("1.2.3.9 - 1.2.3.1").is_none() && p("1.2.3.4 - ::1").is_none() && p("a - b").is_none());
    }

    #[test]
    fn lan() {
        for a in ["192.168.1.9", "10.0.0.1", "100.64.3.1", "169.254.1.1", "224.0.0.251", "fe80::1", "fd00::5", "::ffff:172.16.0.1"] { assert!(is_lan(a.parse().unwrap()), "{a}"); }
        for a in ["8.8.8.8", "100.128.0.1", "172.32.0.1", "2001:db8::1"] { assert!(!is_lan(a.parse().unwrap()), "{a}"); }
    }

    #[test]
    fn whois_parse() {
        assert_eq!(whois_referral("% IANA WHOIS server\nrefer:        whois.ripe.net\n\ninetnum: 1.0.0.0\n").as_deref(), Some("whois.ripe.net"));
        assert_eq!(whois_referral("ReferralServer:  whois://whois.apnic.net:43\n").as_deref(), Some("whois.apnic.net"));
        assert_eq!(whois_referral("refer: whois.nic.ad.jp\n").as_deref(), Some("whois.nic.ad.jp"));
        assert!(whois_referral("ReferralServer: rwhois://rwhois.example.net:4321\n").is_none());
        assert!(whois_referral("refer: bad host;x\n").is_none());
        assert!(whois_referral("refer: evil.example.net\n").is_none());
        assert!(whois_referral("refer: whois.ripe.net.\n").is_none());
        assert!(whois_referral("refer: whois..ripe.net\n").is_none());
        assert!(whois_referral("refer: whois.-bad.net\n").is_none());
        assert!(!public_whois_ip("127.0.0.1".parse().unwrap()));
        assert!(!public_whois_ip("10.1.2.3".parse().unwrap()));
        assert!(!public_whois_ip("169.254.169.254".parse().unwrap()));
        assert!(!public_whois_ip("192.168.1.1".parse().unwrap()));
        assert!(!public_whois_ip("::1".parse().unwrap()));
        assert!(public_whois_ip("193.0.6.135".parse().unwrap()));
        let ripe = "% Abuse contact for '1.2.3.0 - 1.2.3.255' is 'abuse@example.net'\n\ninetnum:  1.2.3.0 - 1.2.3.255\nnetname:  EX-NET\ndescr:    Example\ncountry:  TR\n\nroute:    1.2.0.0/16\ndescr:    other\norigin:   AS64500\n";
        let s = whois_summary(ripe, false);
        assert_eq!(s, [["Range", "1.2.3.0 - 1.2.3.255"], ["CIDR", "1.2.0.0/16"], ["Network", "EX-NET"], ["Organisation", "Example"],
                       ["Country", "TR"], ["AS", "AS64500"], ["Abuse contact", "abuse@example.net"]].map(|r| r.map(str::to_owned)));
        let arin = "NetRange: 8.0.0.0 - 8.127.255.255\nNetName: BIG\nOrgName: Parent\n\nNetRange: 8.8.8.0 - 8.8.8.255\nCIDR: 8.8.8.0/24\nNetName: SMALL\nOrgName: Child\n";
        let s = whois_summary(arin, true);
        assert_eq!((s[0][1].as_str(), s[2][1].as_str(), s[3][1].as_str()), ("8.8.8.0 - 8.8.8.255", "SMALL", "Child"));
    }

    #[test]
    fn local_ranges_of_published_lists() {
        for s in ["127.0.0.0/8", "10.0.0.0/8", "192.168.0.0/16", "224.0.0.0/3", "0.0.0.0/8", "100.64.0.0/10", "8.0.0.0/5", "fe80::/10"] {
            assert!(covers_local(&Net::parse(s).unwrap()), "{s}");
        }
        for s in ["8.8.8.0/24", "1.0.0.0/8", "2001:db8::/32"] { assert!(!covers_local(&Net::parse(s).unwrap()), "{s}"); }
        assert_eq!(full_cidr("200.3.12/22"), "200.3.12.0/22");
        assert_eq!((full_cidr("200/8"), full_cidr("1.2.3.0/24"), full_cidr("2001:db8::/32"), full_cidr("1.2.3.0 - 1.2.3.9")),
                   ("200.0.0.0/8".to_owned(), "1.2.3.0/24".to_owned(), "2001:db8::/32".to_owned(), "1.2.3.0 - 1.2.3.9".to_owned()));
    }

    #[test]
    fn ruleset_text() {
        let t = "table inet centurion_netguard {\n\tset bl4 {\n\t\ttype ipv4_addr\n\t}\n\tchain fw_in {\n\t\ttcp dport { 22 } counter packets 3 bytes 180 return comment \"r1\"\n\t\tcounter packets 7 bytes 70 jump fw_drop comment \"default\"\n\t}\n}\ntable inet ufw {\n\tchain input {\n\t\ttype filter hook input priority filter; policy drop;\n\t\tcounter packets 9 bytes 9 comment \"r1\"\n\t}\n}\n";
        let (present, hits, others) = parse_ruleset(t);
        assert!(present);
        assert_eq!((hits["r1"]["packets"].as_u64(), hits["default_fw_in"]["bytes"].as_u64()), (Some(3), Some(70)));
        assert_eq!((others.len(), others[0]["table"].as_str(), others[0]["policy"].as_str(), others[0]["priority"].as_str()), (1, Some("ufw"), Some("drop"), Some("filter")));
        assert!(!parse_ruleset("table inet other {\n}\n").0);
    }

    #[test]
    fn table_deletion_notice() {
        let mut p = vec![NFPROTO_INET, 0, 0, 0];
        nl_attr(&mut p, NFTA_TABLE_NAME, b"centurion_netguard\0");
        assert!(is_our_table_deleted(NFT_MSG_DELTABLE, &p));
        assert!(!is_our_table_deleted(NFT_MSG_DELTABLE + 1, &p));
        let mut other = vec![NFPROTO_INET, 0, 0, 0];
        nl_attr(&mut other, NFTA_TABLE_NAME, b"filter\0");
        assert!(!is_our_table_deleted(NFT_MSG_DELTABLE, &other));
    }

    #[test]
    fn conntrack_delete_request_round_trip() {
        let ev = traffic::CtEvent { delete: false, id: 0, proto: UDP, src: "10.0.0.2".parse().unwrap(), sport: 5000, dst: "10.0.0.1".parse().unwrap(), dport: 53, orig_bytes: 0, reply_bytes: 0 };
        let got = traffic::parse_ct(traffic::CT_NEW, &traffic::delete_request(&ev)).unwrap();
        assert_eq!((got.proto, got.src, got.sport, got.dst, got.dport), (ev.proto, ev.src, ev.sport, ev.dst, ev.dport));
    }

    #[test]
    fn whois_targets() {
        assert_eq!(whois_target(" 8.8.8.8 "), Some(WhoisQuery::Ip("8.8.8.8".parse().unwrap())));
        assert_eq!(whois_target("8.8.8.77/24"), Some(WhoisQuery::Ip("8.8.8.0".parse().unwrap())));
        assert_eq!(whois_target("2001:db8::1"), Some(WhoisQuery::Ip("2001:db8::1".parse().unwrap())));
        assert_eq!(whois_target("as15169"), Some(WhoisQuery::Asn(15169)));
        assert_eq!(whois_target("Example.COM."), Some(WhoisQuery::Domain("example.com".into())));
        assert_eq!(whois_target("xn--bcher-kva.example"), Some(WhoisQuery::Domain("xn--bcher-kva.example".into())));
        assert_eq!(whois_target("as.example"), Some(WhoisQuery::Domain("as.example".into())));
        for bad in ["", "localhost", "exa mple.com", "-x.com", "a\r\nb.com", "x;y.com", "1.2.3.4/99"] { assert_eq!(whois_target(bad), None, "{bad}"); }
        assert_eq!(whois_question("whois.arin.net", &WhoisQuery::Asn(1)), "a 1");
        assert_eq!(whois_question("whois.ripe.net", &WhoisQuery::Asn(1)), "AS1");
        assert_eq!(whois_question("whois.verisign-grs.com", &WhoisQuery::Domain("a.com".into())), "domain a.com");
    }

    #[test]
    fn domain_and_routing_summaries() {
        let registry = "Domain Name: EXAMPLE.COM\nRegistrar WHOIS Server: whois.registrar.example\nRegistrar: Reg Inc\nCreation Date: 1995-08-14T04:00:00Z\nRegistry Expiry Date: 2030-08-13T04:00:00Z\nDomain Status: clientDeleteProhibited https://icann.org/epp#x\nDomain Status: clientTransferProhibited https://icann.org/epp#y\nName Server: A.IANA-SERVERS.NET\nName Server: B.IANA-SERVERS.NET\nDNSSEC: signedDelegation\n";
        assert_eq!(whois_referral(registry).as_deref(), Some("whois.registrar.example"));
        let registrar = "Domain Name: example.com\nRegistrant Organization: REDACTED FOR PRIVACY\nRegistrant Country: TR\nRegistrar Abuse Contact Email: abuse@reg.example\n";
        let s = domain_summary(&[registry, registrar]);
        let get = |k: &str| s.iter().find(|r| r[0] == k).map(|r| r[1].as_str());
        assert_eq!(get("Registrar"), Some("Reg Inc"));
        assert_eq!(get("Expires"), Some("2030-08-13T04:00:00Z"));
        assert_eq!(get("Status"), Some("clientDeleteProhibited, clientTransferProhibited"));
        assert_eq!(get("Name servers"), Some("a.iana-servers.net, b.iana-servers.net"));
        assert_eq!((get("Country"), get("Organisation"), get("Abuse contact")), (Some("TR"), None, Some("abuse@reg.example")));

        let c = cymru_summary("AS      | IP               | BGP Prefix          | CC | Registry | Allocated  | AS Name\n15169   | 8.8.8.8          | 8.8.8.0/24          | US | arin     | 2023-12-28 | GOOGLE, US\n");
        assert_eq!(c[0], ["BGP prefix".to_owned(), "8.8.8.0/24".to_owned()]);
        assert!(c.contains(&["AS name".to_owned(), "GOOGLE, US".to_owned()]) && c.contains(&["Origin AS".to_owned(), "AS15169".to_owned()]));
        let c = cymru_summary("AS      | CC | Registry | Allocated  | AS Name\n15169   | US | arin     | 2000-03-30 | GOOGLE, US\n");
        assert!(c.contains(&["Allocated".to_owned(), "2000-03-30".to_owned()]));
        let c = cymru_summary("AS | IP | BGP Prefix | CC | Registry | Allocated | AS Name\nNA | 192.0.2.1 | NA | | other | | NA\n");
        assert_eq!(c, [["Origin AS".to_owned(), "not announced".to_owned()]]);
    }

    #[test]
    fn firewall_in_the_table() {
        let mut c = Config::default();
        let plain = nft_script(&c);
        assert!(!plain.contains("fw_") && !plain.contains("established"), "off: the table is what it always was");
        c.fw.enabled = true;
        c.fw.inbound = crate::firewall::Verdict::Block;
        c.log_conns = true;
        let s = nft_ruleset(&c, true);
        // The verdict of the queue ends the chain: the firewall must have spoken before it, after the blacklist.
        let chain = &s[s.find("chain in {").unwrap()..s.find("chain out {").unwrap()];
        let at = |t: &str| chain.find(t).unwrap_or_else(|| panic!("{t}"));
        assert!(at("ip saddr @bl4 drop") < at("ct state established,related accept") && at("jump fw_in") < at("queue num"));
        assert!(s.contains("log group 19536") && !nft_ruleset(&c, false).contains("log group"));
        let saved = Config::default().to_json();
        assert_eq!(saved["firewall"]["enabled"], false);
    }

    #[test]
    fn queue_fail_closed_only_when_guarding() {
        let mut c = Config { log_conns: true, ..Default::default() };
        let log_only = nft_script(&c);
        assert!(log_only.contains("queue num 19536 bypass"));
        c.guard = true;
        let guarding = nft_script(&c);
        let outbound = guarding.split("chain out {").nth(1).unwrap();
        assert!(outbound.contains("queue num 19536\n"));
        assert!(!outbound.contains("queue num 19536 bypass"));
        let inbound = guarding.split("chain in {").nth(1).unwrap().split("chain out {").next().unwrap();
        assert!(inbound.contains("queue num 19536 bypass"));
    }

    #[test]
    fn reload_error_is_not_success() {
        let c = Config::default();
        let good = json!({"guard": c.guard, "allow_lan": c.allow_lan, "log_conns": c.log_conns,
            "traffic": c.traffic, "firewall": c.fw.to_json(), "error": null});
        assert_eq!(reload_state(&c, &good), ReloadState::Applied);
        let mut error = good.clone();
        error["error"] = json!("nft failed");
        assert_eq!(reload_state(&c, &error), ReloadState::Failed);
        let mut stale = good.clone();
        stale["guard"] = json!(!c.guard);
        assert_eq!(reload_state(&c, &stale), ReloadState::Pending);
    }

    #[test]
    fn loaded_nflog_overrides_missing_kernel_config_symbol() {
        let mut cfg = Config { log_conns: true, ..Default::default() };
        cfg.fw.enabled = true;
        cfg.fw.inbound = crate::firewall::Verdict::Block;
        let nflog = "NETFILTER_NETLINK_LOG";
        let text = "# CONFIG_NETFILTER_NETLINK_LOG is not set";
        let missing = missing_kernel_options(&cfg, text, true);
        assert!(!missing.contains(&nflog));
        assert!(missing.contains(&"NFT_LOG"));
        assert!(missing_kernel_options(&cfg, text, false).contains(&nflog));
        for text in ["CONFIG_NETFILTER_NETLINK_LOG=m", "CONFIG_NETFILTER_NETLINK_LOG=y"] {
            assert!(!missing_kernel_options(&cfg, text, false).contains(&nflog));
        }
    }

    #[test]
    fn nflog_message() {
        let mut body = vec![0, 0, 0x4c, 0x50];
        nl_attr(&mut body, NFULA_PACKET_HDR, &[8, 0, 3, 0]);
        nl_attr(&mut body, NFULA_PAYLOAD, &[0x45, 1, 2]);
        assert_eq!(nflog_packet(&body), Some((3, &[0x45u8, 1, 2][..])));
        assert_eq!(nflog_packet(&[0, 0, 0, 0]), None);
    }

    #[test]
    fn queue_message() {
        let mut body = vec![2, 0, 0x4c, 0x50];
        nl_attr(&mut body, NFQA_PACKET_HDR, &[0, 0, 0, 7, 8, 0, 3]);
        nl_attr(&mut body, NFQA_PAYLOAD, &[0x45, 1, 2]);
        assert_eq!(queued_packet(&body), Some((7, 3, &[0x45u8, 1, 2][..])));
    }
}
