//! Wi-Fi link, hardware and nearby networks — Health → Network → Wi-Fi.
//!
//! Read-only and unprivileged: `iw dev <if> link / info / station dump / scan dump` and `iw phy <phy> info`
//! are nl80211 reads (`scan dump` shows the kernel's cached results; it does not scan). The one action,
//! `wifi_rescan`, asks NetworkManager (`nmcli device wifi rescan`, which polkit allows an active session).
//! The interface is one of the wireless ones under /sys/class/net: the request may name one, but only a
//! name found there is used. No Wi-Fi interface → `present: false` (the GUI hides the tab). The GUI asks
//! only while the tab is on screen (link every 3 s; hardware and nearby networks when their page is
//! opened or refreshed), so nothing runs in the background.

use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use std::process::Command;

/// Below this the link is called weak (a common rule of thumb, not a measured limit).
const WEAK_DBM: i32 = -75;
/// Retries and failed transmissions, as a share of transmitted packets, since association.
const RETRY_PCT: f64 = 10.0;
const FAIL_PCT: f64 = 2.0;

#[derive(Debug, Default, PartialEq)]
pub struct Link {
    pub connected: bool,
    pub bssid: String,
    pub ssid: String,
    pub freq: Option<u32>,
    pub rx_mode: String,
    pub tx_mode: String,
    pub signal_dbm: Option<i32>,
    pub rx_mbps: Option<f64>,
    pub tx_mbps: Option<f64>,
}

#[derive(Debug, Default, PartialEq)]
pub struct Station {
    pub signal_dbm: Option<i32>,
    pub tx_mbps: Option<f64>,
    pub tx_packets: u64,
    pub tx_retries: u64,
    pub tx_failed: u64,
    pub rx_packets: u64,
    pub rx_bytes: u64,
    pub tx_bytes: u64,
    pub beacon_loss: u64,
    pub connected_secs: Option<u64>,
    pub signal_avg_dbm: Option<i32>,
    pub expected_mbps: Option<f64>,
}

/// `iw dev <if> info`: the interface's own address, phy and current channel.
#[derive(Debug, Default, PartialEq)]
pub struct Info {
    pub addr: String,
    pub wiphy: Option<u32>,
    pub freq: Option<u32>,
    pub channel: Option<u32>,
    pub width_mhz: Option<u32>,
    pub txpower_dbm: Option<f64>,
}

/// One access point of the scan results.
#[derive(Debug, Default, PartialEq)]
pub struct Bss {
    pub bssid: String,
    pub ssid: String,
    pub freq: u32,
    pub signal_dbm: Option<i32>,
    pub security: &'static str,
    pub seen_ms: Option<u64>,
    pub seen_boottime_ms: Option<u64>,
    pub channel_range: Option<ChannelRange>,
    pub channel_80p80: Option<SplitChannel>,
    pub associated: bool,
}

/// Nominal advertised contiguous channel, not observed spectral occupancy.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ChannelRange { pub center_mhz: u32, pub width_mhz: u32, pub source: &'static str }

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SplitChannel { pub primary_center_mhz: u32, pub secondary_center_mhz: u32 }

#[derive(Default)]
struct Operation {
    primary: Option<u32>, secondary: Option<String>, ht_width: Option<String>,
    ht: bool, vht: bool, width: Option<u32>, seg1: Option<u32>, seg2: Option<u32>,
    unsupported: bool, invalid: bool, section: u8,
}

// Duplicate information elements/fields can be inconsistent (e.g. beacon vs probe).
// Do not select one arbitrarily.
fn unique<T>(slot: &mut Option<T>, value: Option<T>, invalid: &mut bool) {
    if slot.is_some() || value.is_none() { *invalid = true; }
    else { *slot = value; }
}

impl Operation {
    const VHT_80_CENTERS: [u32; 7] = [42, 58, 106, 122, 138, 155, 171];
    const VHT_160_CENTERS: [u32; 3] = [50, 114, 163];

    fn line(&mut self, line: &str) {
        let t = line.trim();
        if line.starts_with('\t') && !line.starts_with("\t\t") {
            self.section = 0;
            if t.starts_with("HT operation:") { self.invalid |= self.ht; self.ht = true; self.section = 1; }
            else if t.starts_with("VHT operation:") { self.invalid |= self.vht; self.section = 2; self.vht = true; }
            else if t.starts_with("HE operation:") || t.starts_with("EHT operation:") || t.starts_with("HE Operation:") || t.starts_with("EHT Operation:") { self.unsupported = true; }
            return;
        }
        if !line.starts_with("\t\t") { return; }
        let Some((key, val)) = t.strip_prefix("* ").and_then(|x| x.split_once(':')) else { return; };
        let val = val.trim();
        match (self.section, key) {
            (1, "primary channel") => unique(&mut self.primary, val.parse().ok(), &mut self.invalid),
            (1, "secondary channel offset") => unique(&mut self.secondary, Some(val.to_owned()), &mut self.invalid),
            (1, "STA channel width") => unique(&mut self.ht_width, Some(val.to_owned()), &mut self.invalid),
            (2, "channel width") => unique(&mut self.width, first(val), &mut self.invalid),
            (2, "center freq segment 1") => unique(&mut self.seg1, val.parse().ok(), &mut self.invalid),
            (2, "center freq segment 2") => unique(&mut self.seg2, val.parse().ok(), &mut self.invalid),
            _ => {}
        }
    }
    fn primary_channel(&self, freq: u32) -> Option<u32> {
        if self.invalid || self.unsupported || !matches!(band(freq), "2.4" | "5") { return None; }
        let ch = self.primary?;
        let primary = if band(freq) == "2.4" && (1..=13).contains(&ch) { 2407 + 5*ch }
            else if band(freq) == "5" && (((36..=64).contains(&ch) || (100..=144).contains(&ch)) && ch % 4 == 0 || (149..=177).contains(&ch) && ch % 4 == 1) { 5000 + 5*ch }
            else { return None; };
        (primary == freq).then_some(ch)
    }

    fn split_range(&self, freq: u32) -> Option<SplitChannel> {
        let primary = self.primary_channel(freq)?;
        if band(freq) != "5" || !self.vht || !matches!(self.width?, 1 | 3) { return None; }
        let first = self.seg1?;
        let second = self.seg2?;
        if !Self::VHT_80_CENTERS.contains(&first) || !Self::VHT_80_CENTERS.contains(&second) || first.abs_diff(second) <= 16
            || !matches!(primary.abs_diff(first), 2 | 6) { return None; }
        Some(SplitChannel { primary_center_mhz: 5000 + 5 * first, secondary_center_mhz: 5000 + 5 * second })
    }

    fn range(&self, freq: u32) -> Option<ChannelRange> {
        let ch = self.primary_channel(freq)?;
        let (center, width, source) = if self.vht && self.width? != 0 {
            if band(freq) != "5" { return None; }
            let a = self.seg1?; let b = self.seg2?;
            let (c, w) = match (self.width?, b) {
                (1, 0) if Self::VHT_80_CENTERS.contains(&a) => (a, 80),
                (1, b) if Self::VHT_80_CENTERS.contains(&a) && Self::VHT_160_CENTERS.contains(&b)
                    && a.abs_diff(b) == 8 && matches!(ch.abs_diff(a), 2 | 6) => (b, 160),
                (2, 0) if Self::VHT_160_CENTERS.contains(&a) => (a, 160),
                _ => return None, // 80+80 and unknown encodings are not contiguous
            };
            if !matches!((w, ch.abs_diff(c)), (80, 2 | 6) | (160, 2 | 6 | 10 | 14)) { return None; }
            (5000 + 5*c, w, "VHT operation")
        } else {
            // Width=0 VHT delegates to HT. No channel inferred from capability bits alone.
            if self.vht && (self.seg1? != 0 || self.seg2? != 0) { return None; }
            match (self.secondary.as_deref()?, self.ht_width.as_deref()?) {
                ("no secondary", "20 MHz") | ("no secondary", "any") => (freq, 20, "HT operation"),
                ("above", "any") => (freq + 10, 40, "HT operation"),
                ("below", "any") => (freq - 10, 40, "HT operation"),
                _ => return None,
            }
        };
        let (lo, hi) = if band(freq) == "2.4" { (2400,2500) } else { (5150,5895) };
        if center-width/2 < lo || center+width/2 > hi { return None; }
        Some(ChannelRange { center_mhz: center, width_mhz: width, source })
    }
}

fn seen_boottime(v: &str) -> Option<u64> {
    let secs = v.strip_suffix("s [boottime]")?.trim();
    let (whole, frac) = secs.split_once('.').unwrap_or((secs, ""));
    if whole.is_empty() || !whole.bytes().all(|c| c.is_ascii_digit()) || !frac.bytes().all(|c| c.is_ascii_digit()) || frac.len() > 9 { return None; }
    let whole: u64 = whole.parse().ok()?;
    let mut ms = 0u64;
    for (i,c) in frac.bytes().take(3).enumerate() { ms += u64::from(c-b'0') * [100,10,1][i]; }
    whole.checked_mul(1000)?.checked_add(ms).filter(|v| *v <= 9_007_199_254_740_991)
}

fn boot_ms() -> Option<u64> {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    if unsafe { libc::clock_gettime(libc::CLOCK_BOOTTIME, &mut ts) } != 0 || ts.tv_sec < 0 || !(0..1_000_000_000).contains(&ts.tv_nsec) { return None; }
    (ts.tv_sec as u64).checked_mul(1000)?.checked_add(ts.tv_nsec as u64 / 1_000_000).filter(|v| *v <= 9_007_199_254_740_991)
}

/// One band of `iw phy info`: usable channels, strongest allowed power, 802.11 generations it speaks.
#[derive(Debug, Default, PartialEq)]
pub struct Band {
    pub name: &'static str,
    pub channels: u32,
    pub max_dbm: Option<f64>,
    pub standards: Vec<&'static str>,
}

/// `key: value` pairs of iw's tab-indented output, keys trimmed. Lines without a colon are skipped.
fn fields(text: &str) -> impl Iterator<Item = (&str, &str)> {
    text.lines().filter_map(|l| {
        let (k, v) = l.trim().split_once(':')?;
        Some((k.trim(), v.trim()))
    })
}

/// First number of a value such as "-52 dBm" or "866.7 MBit/s 80MHz".
fn first<T: std::str::FromStr>(v: &str) -> Option<T> { v.split_whitespace().next()?.parse().ok() }

fn frequency_mhz(value: &str) -> Option<u32> {
    let freq = first::<f64>(value)?;
    (freq >= 1.0 && freq <= f64::from(u32::MAX) && freq.fract() == 0.0).then_some(freq as u32)
}

pub fn parse_link(text: &str) -> Link {
    let mut l = Link { connected: text.trim_start().starts_with("Connected to"), ..Link::default() };
    if let Some(b) = text.trim_start().strip_prefix("Connected to ").and_then(|t| t.split_whitespace().next()) { l.bssid = b.to_owned(); }
    for (k, v) in fields(text) {
        match k {
            "SSID" => l.ssid = v.to_owned(),
            "freq" => l.freq = frequency_mhz(v),
            "signal" => l.signal_dbm = first(v),
            "rx bitrate" => { l.rx_mbps = first(v); l.rx_mode = v.to_owned(); }
            "tx bitrate" => { l.tx_mbps = first(v); l.tx_mode = v.to_owned(); }
            _ => {}
        }
    }
    l
}

pub fn parse_station(text: &str) -> Station {
    let mut s = Station::default();
    for (k, v) in fields(text) {
        match k {
            "signal" => s.signal_dbm = first(v),
            "tx bitrate" => s.tx_mbps = first(v),
            "tx packets" => s.tx_packets = first(v).unwrap_or(0),
            "tx retries" => s.tx_retries = first(v).unwrap_or(0),
            "tx failed" => s.tx_failed = first(v).unwrap_or(0),
            "rx packets" => s.rx_packets = first(v).unwrap_or(0),
            "rx bytes" => s.rx_bytes = first(v).unwrap_or(0),
            "tx bytes" => s.tx_bytes = first(v).unwrap_or(0),
            "beacon loss" => s.beacon_loss = first(v).unwrap_or(0),
            "connected time" => s.connected_secs = first(v),
            "signal avg" => s.signal_avg_dbm = first(v),
            "expected throughput" => s.expected_mbps = lead_f64(v),
            _ => {}
        }
    }
    s
}

/// The number a value starts with, even when the unit is glued to it ("866.700Mbps", "22.00 dBm").
fn lead_f64(v: &str) -> Option<f64> {
    let end = v.find(|c: char| !(c.is_ascii_digit() || c == '.' || c == '-')).unwrap_or(v.len());
    v[..end].parse().ok()
}

pub fn parse_info(text: &str) -> Info {
    let mut i = Info::default();
    for l in text.lines().map(str::trim) {
        if let Some(v) = l.strip_prefix("addr ") { i.addr = v.trim().to_owned(); }
        else if let Some(v) = l.strip_prefix("wiphy ") { i.wiphy = v.trim().parse().ok(); }
        else if let Some(v) = l.strip_prefix("txpower ") { i.txpower_dbm = lead_f64(v); }
        else if let Some(v) = l.strip_prefix("channel ") {
            // "36 (5180 MHz), width: 80 MHz, center1: 5210 MHz"
            i.channel = first(v);
            i.freq = v.split('(').nth(1).and_then(first);
            i.width_mhz = v.split("width:").nth(1).and_then(first);
        }
    }
    i
}

/// "2.4", "5" or "6" (GHz), empty for a frequency outside them.
pub fn band(freq: u32) -> &'static str {
    match freq { 2400..=2500 => "2.4", 5150..=5895 => "5", 5925..=7125 => "6", _ => "" }
}

pub fn channel(freq: u32) -> Option<u32> {
    match freq {
        2484 => Some(14),
        2412..=2472 if (freq - 2407) % 5 == 0 => Some((freq - 2407) / 5),
        5150..=5895 if freq % 5 == 0 => Some((freq - 5000) / 5),
        5935 => Some(2),
        5955..=7115 if freq % 5 == 0 => Some((freq - 5950) / 5),
        _ => None,
    }
}

/// The 802.11 generation a bitrate line speaks: "be" (Wi-Fi 7), "ax" (6), "ac" (5), "n" (4), "legacy", or "".
pub fn standard(mode: &str) -> &'static str {
    if mode.contains("EHT-MCS") { "be" }
    else if mode.contains("HE-MCS") { "ax" }
    else if mode.contains("VHT-MCS") { "ac" }
    else if mode.contains("MCS") { "n" }
    else if mode.trim().is_empty() { "" }
    else { "legacy" }
}

fn security(auth: Option<&str>, wpa: bool, privacy: bool) -> &'static str {
    match auth {
        Some(a) if a.contains("OWE") => "OWE",
        Some(a) if a.contains("802.1X") => "Enterprise",
        Some(a) if a.contains("SAE") && a.contains("PSK") => "WPA2/WPA3",
        Some(a) if a.contains("SAE") => "WPA3",
        Some(_) => "WPA2",
        None if wpa => "WPA",
        None if privacy => "WEP",
        None => "Open",
    }
}

/// `iw dev <if> scan dump`, strongest first. Lines of the RSN / WPA blocks are read only for their
/// authentication suites; everything else is a plain `key: value` line one tab deep.
pub fn parse_scan(text: &str) -> Vec<Bss> {
    struct Raw { bss: Bss, auth: Option<String>, wpa: bool, privacy: bool, operation: Operation }
    let done = |r: Raw| Bss {
        security: security(r.auth.as_deref(), r.wpa, r.privacy),
        channel_range: r.operation.range(r.bss.freq),
        channel_80p80: r.operation.split_range(r.bss.freq),
        ..r.bss
    };
    let mut out: Vec<Bss> = Vec::new();
    let mut cur: Option<Raw> = None;
    let mut in_rsn = false;
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix("BSS ") {
            if let Some(r) = cur.take() { out.push(done(r)); }
            in_rsn = false;
            let bssid = rest.split(|c: char| c == '(' || c.is_whitespace()).next().unwrap_or("").to_owned();
            cur = Some(Raw { bss: Bss { bssid, associated: line.contains("-- associated"), ..Bss::default() }, auth: None, wpa: false, privacy: false, operation: Operation::default() });
            continue;
        }
        let Some(r) = cur.as_mut() else { continue };
        r.operation.line(line);
        let t = line.trim();
        if t.contains("Authentication suites:") && in_rsn {
            let v = t.split_once("suites:").map_or("", |(_, v)| v.trim());
            r.auth = Some(format!("{} {v}", r.auth.as_deref().unwrap_or("")));
        }
        if !line.starts_with('\t') || line.starts_with("\t\t") { continue; }
        in_rsn = t.starts_with("RSN:");
        if in_rsn { r.auth.get_or_insert_with(String::new); }
        if t.starts_with("WPA:") { r.wpa = true; }
        let Some((k, v)) = t.split_once(':') else { continue };
        let v = v.trim();
        match k {
            "freq" => r.bss.freq = frequency_mhz(v).unwrap_or(0),
            "signal" => r.bss.signal_dbm = first::<f64>(v).filter(|f| f.is_finite() && (-127.0..=0.0).contains(f)).map(|f| f.round() as i32),
            "SSID" => r.bss.ssid = v.to_owned(),
            "capability" => r.privacy = v.contains("Privacy"),
            "last seen" if v.ends_with("s [boottime]") => r.bss.seen_boottime_ms = seen_boottime(v),
            "last seen" if v.ends_with(" ms ago") => r.bss.seen_ms = v.strip_suffix(" ms ago").and_then(|n| n.trim().parse().ok()),
            _ => {}
        }
    }
    if let Some(r) = cur { out.push(done(r)); }
    out.sort_by_key(|b| std::cmp::Reverse(b.signal_dbm.unwrap_or(i32::MIN)));
    out
}

/// How crowded the connected channel is: other networks on it, and (2.4 GHz only) on channels within
/// four of it, which overlap it. `None` when the scan holds no associated network.
pub fn channel_load(list: &[Bss]) -> Option<(u32, usize, usize)> {
    let me = list.iter().find(|b| b.associated)?;
    let ch = channel(me.freq)?;
    let mut same = 0;
    let mut overlap = 0;
    for b in list.iter().filter(|b| !b.associated && band(b.freq) == band(me.freq)) {
        let Some(c) = channel(b.freq) else { continue };
        if c == ch { same += 1; }
        else if band(me.freq) == "2.4" && c.abs_diff(ch) <= 4 { overlap += 1; }
    }
    Some((ch, same, overlap))
}

/// Bands, and the interface modes, of `iw phy <phy> info`.
pub fn parse_phy(text: &str) -> (Vec<Band>, Vec<String>) {
    let mut bands: Vec<Band> = Vec::new();
    let mut modes = Vec::new();
    let mut in_modes = false;
    for line in text.lines() {
        let t = line.trim();
        if line.starts_with("\tBand ") && t.ends_with(':') { bands.push(Band::default()); in_modes = false; continue; }
        if t.starts_with("Supported interface modes:") { in_modes = true; continue; }
        if in_modes {
            if let Some(m) = t.strip_prefix("* ") { modes.push(m.to_owned()); continue; }
            in_modes = false;
        }
        let Some(b) = bands.last_mut() else { continue };
        let g = if t.starts_with("Capabilities:") { "n" }
            else if t.starts_with("VHT Capabilities") { "ac" }
            else if t.starts_with("HE Iftypes") { "ax" }
            else if t.starts_with("EHT Iftypes") { "be" }
            else { "" };
        if !g.is_empty() && !b.standards.contains(&g) { b.standards.push(g); }
        if let Some(f) = t.strip_prefix("* ").filter(|r| r.contains(" MHz [") && !r.contains("(disabled)")) {
            b.channels += 1;
            if b.name.is_empty() { b.name = band(first(f).unwrap_or(0)); }
            let dbm = f.split(" dBm)").next().filter(|_| f.contains(" dBm)")).and_then(|h| h.rsplit('(').next()).and_then(lead_f64);
            if dbm > b.max_dbm { b.max_dbm = dbm; }
        }
    }
    (bands, modes)
}

/// The first `country XX:` of `iw reg get` ("00" is the world domain).
pub fn parse_reg(text: &str) -> Option<String> {
    let c = text.lines().find_map(|l| l.trim().strip_prefix("country "))?;
    Some(c.split(':').next()?.trim().to_owned())
}

/// `name` and `name_pci` of the PCI ID database, when it is installed.
fn pci_names(vendor: &str, device: &str) -> (Option<String>, Option<String>) {
    for path in ["/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids"] {
        let Ok(txt) = std::fs::read_to_string(path) else { continue };
        let mut vname = None;
        for l in txt.lines().filter(|l| !l.is_empty() && !l.starts_with('#')) {
            if !l.starts_with('\t') {
                if vname.is_some() { break; }
                if let Some(r) = l.strip_prefix(vendor).filter(|r| r.starts_with("  ")) { vname = Some(r.trim().to_owned()); }
            } else if vname.is_some() && !l.starts_with("\t\t") {
                if let Some(r) = l[1..].strip_prefix(device).filter(|r| r.starts_with("  ")) { return (vname, Some(r.trim().to_owned())); }
            }
        }
        return (vname, None);
    }
    (None, None)
}

fn pct(n: u64, of: u64) -> f64 { if of == 0 { 0.0 } else { n as f64 * 100.0 / of as f64 } }

/// What the numbers say, as keys the GUI words: weak signal, many retries, many failed transmissions.
pub fn warnings(s: &Station) -> Vec<&'static str> {
    let mut w = Vec::new();
    if s.signal_dbm.is_some_and(|d| d < WEAK_DBM) { w.push("weak_signal"); }
    if pct(s.tx_retries, s.tx_packets) > RETRY_PCT { w.push("retries"); }
    if pct(s.tx_failed, s.tx_packets) > FAIL_PCT { w.push("failed"); }
    w
}

/// Every interface with a Wi-Fi (nl80211 or wext) directory; the ones that are up come first.
fn wireless_ifaces() -> Vec<String> {
    let Ok(dir) = std::fs::read_dir("/sys/class/net") else { return Vec::new() };
    let mut names: Vec<(bool, String)> = dir
        .filter_map(Result::ok)
        .filter(|e| e.path().join("phy80211").exists() || e.path().join("wireless").exists())
        .map(|e| {
            let up = std::fs::read_to_string(e.path().join("operstate")).is_ok_and(|s| s.trim() == "up");
            (!up, e.file_name().to_string_lossy().into_owned())
        })
        .collect();
    names.sort();
    names.into_iter().map(|(_, n)| n).collect()
}

/// The interface the request names if it is a wireless one, else the first.
fn pick(req: &Value) -> (Vec<String>, Option<String>) {
    let all = wireless_ifaces();
    let one = req["iface"].as_str().filter(|w| all.iter().any(|n| n == w)).map(str::to_owned).or_else(|| all.first().cloned());
    (all, one)
}


/// A trusted (root-owned) tool by name, e.g. `ethtool`, `nmcli`.
fn tool(name: &str) -> Option<PathBuf> {
    ["/usr/sbin", "/sbin", "/usr/bin", "/bin"].iter().map(|d| Path::new(d).join(name)).find(|p| crate::trusted_path(p))
}

fn run(bin: &Path, args: &[&str]) -> Result<String, String> {
    let name = bin.file_name().map_or_else(String::new, |n| n.to_string_lossy().into_owned());
    let out = Command::new(bin).args(args)
        .env_clear().env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin").env("LC_ALL", "C").current_dir("/")
        .output().map_err(|e| format!("{name}: {e}"))?;
    if out.status.success() { Ok(String::from_utf8_lossy(&out.stdout).into_owned()) }
    else {
        let err = String::from_utf8_lossy(&out.stderr);
        let err = if err.trim().is_empty() { String::from_utf8_lossy(&out.stdout) } else { err };
        Err(format!("{name} {}: {}", args.join(" "), err.trim()))
    }
}

fn iw(bin: &Path, iface: &str, args: &[&str]) -> Result<String, String> {
    run(bin, &[&["dev", iface], args].concat())
}

/// Link, station counters and channel of one Wi-Fi interface. Errors are reported, not raised.
pub fn status(req: &Value) -> Value {
    let (ifaces, iface) = pick(req);
    let Some(iface) = iface else { return json!({"ok": true, "present": false}) };
    let Some(bin) = tool("iw") else { return json!({"ok": true, "present": true, "iface": iface, "ifaces": ifaces, "iw": false}) };
    let (link, station) = match (iw(&bin, &iface, &["link"]), iw(&bin, &iface, &["station", "dump"])) {
        (Ok(l), Ok(s)) => (parse_link(&l), parse_station(&s)),
        (Err(e), _) | (_, Err(e)) => return json!({"ok": false, "present": true, "iface": iface, "ifaces": ifaces, "iw": true, "error": e}),
    };
    let info = iw(&bin, &iface, &["info"]).map(|t| parse_info(&t)).unwrap_or_default();
    let freq = link.freq.or(info.freq);
    let tx_mode = if link.tx_mode.is_empty() { &link.rx_mode } else { &link.tx_mode };
    json!({"ok": true, "present": true, "iw": true, "iface": iface, "ifaces": ifaces, "connected": link.connected, "ssid": link.ssid,
           "bssid": link.bssid, "freq": freq, "channel": info.channel.or(freq.and_then(channel)), "band": freq.map_or("", band),
           "width_mhz": info.width_mhz, "tx_power_dbm": info.txpower_dbm,
           "standard": standard(tx_mode), "rx_mode": link.rx_mode, "tx_mode": link.tx_mode,
           "signal_dbm": station.signal_dbm.or(link.signal_dbm), "signal_avg_dbm": station.signal_avg_dbm, "rx_mbps": link.rx_mbps,
           "tx_mbps": station.tx_mbps.or(link.tx_mbps), "expected_mbps": station.expected_mbps,
           "connected_secs": station.connected_secs, "beacon_loss": station.beacon_loss,
           "rx_packets": station.rx_packets, "rx_bytes": station.rx_bytes, "tx_bytes": station.tx_bytes,
           "tx_packets": station.tx_packets, "tx_retries": station.tx_retries, "tx_failed": station.tx_failed,
           "retry_pct": pct(station.tx_retries, station.tx_packets), "fail_pct": pct(station.tx_failed, station.tx_packets),
           "warn": warnings(&station)})
}

fn read_trim(p: impl AsRef<Path>) -> Option<String> {
    std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned()).filter(|s| !s.is_empty())
}

fn link_name(p: impl AsRef<Path>) -> Option<String> {
    std::fs::read_link(p).ok()?.file_name().map(|n| n.to_string_lossy().into_owned())
}

/// Whether a rfkill switch of the phy blocks the radio: (software, hardware).
fn rfkill(phy: u32) -> (Option<bool>, Option<bool>) {
    let Ok(dir) = std::fs::read_dir(format!("/sys/class/ieee80211/phy{phy}")) else { return (None, None) };
    let Some(e) = dir.flatten().find(|e| e.file_name().to_string_lossy().starts_with("rfkill")) else { return (None, None) };
    let blocked = |f: &str| read_trim(e.path().join(f)).map(|v| v == "1");
    (blocked("soft"), blocked("hard"))
}

/// The adapter behind one interface and what the driver and the radio say about themselves. Static, so the
/// GUI asks once when the page is opened.
pub fn hardware(req: &Value) -> Value {
    let (_, iface) = pick(req);
    let Some(iface) = iface else { return json!({"ok": true, "present": false}) };
    let dev = PathBuf::from(format!("/sys/class/net/{iface}/device"));
    let bus = link_name(dev.join("subsystem"));
    // PCI and SDIO keep the IDs on the device; a USB interface keeps them (and the product strings) on its parent.
    let (vendor, product, vname, pname) = if bus.as_deref() == Some("usb") {
        let up = dev.join("..");
        (read_trim(up.join("idVendor")), read_trim(up.join("idProduct")), read_trim(up.join("manufacturer")), read_trim(up.join("product")))
    } else {
        let id = |f: &str| read_trim(dev.join(f)).map(|v| v.trim_start_matches("0x").to_ascii_lowercase());
        let (v, d) = (id("vendor"), id("device"));
        let (vn, dn) = match (&v, &d, bus.as_deref()) { (Some(v), Some(d), Some("pci")) => pci_names(v, d), _ => (None, None) };
        (v, d, vn, dn)
    };
    let slot = std::fs::canonicalize(&dev).ok().and_then(|p| p.file_name().map(|n| n.to_string_lossy().into_owned()));
    let mut out = json!({"ok": true, "present": true, "iface": iface, "driver": link_name(dev.join("driver")), "bus": bus, "slot": slot,
                         "vendor_id": vendor, "device_id": product, "vendor": vname, "product": pname});
    if let Some(bin) = tool("ethtool") {
        if let Ok(t) = run(&bin, &["-i", &iface]) {
            out["firmware"] = json!(fields(&t).find(|(k, _)| *k == "firmware-version").map(|(_, v)| v).filter(|v| !v.is_empty() && *v != "N/A"));
        }
    }
    let Some(iw_bin) = tool("iw") else { out["iw"] = json!(false); return out };
    out["iw"] = json!(true);
    let info = iw(&iw_bin, &iface, &["info"]).map(|t| parse_info(&t)).unwrap_or_default();
    out["mac"] = json!(read_trim(format!("/sys/class/net/{iface}/address")).or(Some(info.addr).filter(|a| !a.is_empty())));
    out["power_save"] = json!(iw(&iw_bin, &iface, &["get", "power_save"]).ok()
        .and_then(|t| fields(&t).next().map(|(_, v)| v.eq_ignore_ascii_case("on"))));
    out["regdom"] = json!(run(&iw_bin, &["reg", "get"]).ok().and_then(|t| parse_reg(&t)));
    if let Some(phy) = info.wiphy {
        let (soft, hard) = rfkill(phy);
        out["rfkill_soft"] = json!(soft);
        out["rfkill_hard"] = json!(hard);
        if let Ok(t) = run(&iw_bin, &["phy", &format!("phy{phy}"), "info"]) {
            let (bands, modes) = parse_phy(&t);
            out["bands"] = bands.iter().map(|b| json!({"band": b.name, "channels": b.channels, "max_dbm": b.max_dbm, "standards": b.standards})).collect();
            out["modes"] = json!(modes);
        }
    }
    out
}

// Unlike status discovery, a scan request naming a device must not silently
// fall back to another adapter when the selected one disappears.
fn scan_iface(req: &Value) -> Result<Option<String>, Value> {
    let (_, iface) = pick(req);
    if let Some(requested) = req["iface"].as_str().filter(|s| !s.is_empty()) {
        if iface.as_deref() != Some(requested) {
            return Err(json!({"ok": false, "iface": requested, "error": "Selected wireless adapter is no longer available"}));
        }
    }
    Ok(iface)
}

fn adapter_stamp(iface: &str) -> Option<String> {
    use std::os::unix::fs::MetadataExt;

    let path = Path::new("/sys/class/net").join(iface);
    let before = std::fs::metadata(&path).ok()?;
    let index = std::fs::read_to_string(path.join("ifindex")).ok()?.trim().parse::<u32>().ok()?;
    let after = std::fs::metadata(&path).ok()?;
    if index == 0 || (before.dev(), before.ino()) != (after.dev(), after.ino()) { return None; }
    Some(format!("{}:{}:{index}", before.dev(), before.ino()))
}

fn scan_adapter_stamp(req: &Value, iface: &str) -> Result<String, Value> {
    let stamp = adapter_stamp(iface).ok_or_else(|| json!({
        "ok": false, "iface": iface, "error": "Wireless adapter identity is unavailable"
    }))?;
    if let Some(expected) = req.get("adapter_stamp") {
        if expected.as_str() != Some(stamp.as_str()) {
            return Err(json!({"ok": false, "iface": iface, "error": "Selected wireless adapter has changed"}));
        }
    }
    Ok(stamp)
}

/// Nearby networks from the kernel's cached scan results (NetworkManager and wpa_supplicant keep them fresh).
pub fn scan(req: &Value) -> Value {
    let iface = match scan_iface(req) { Ok(i) => i, Err(e) => return e };
    let Some(iface) = iface else { return json!({"ok": true, "present": false}) };
    let Some(bin) = tool("iw") else { return json!({"ok": true, "present": true, "iw": false}) };
    let before = match scan_adapter_stamp(req, &iface) { Ok(stamp) => stamp, Err(error) => return error };
    let text = match iw(&bin, &iface, &["scan", "dump"]) {
        Ok(t) => t,
        Err(e) => return json!({"ok": false, "present": true, "iface": iface, "error": e}),
    };
    let dump_completed_boottime_ms = boot_ms();
    if adapter_stamp(&iface).as_deref() != Some(before.as_str()) { return json!({"ok": false, "iface": iface, "error": "Wireless adapter changed during scan dump"}); }
    let list = parse_scan(&text);
    let load = channel_load(&list).map(|(ch, same, overlap)| json!({"channel": ch, "same": same, "overlap": overlap}));
    let nets: Vec<Value> = list.iter().take(64).map(|b| json!({
        "bssid": b.bssid, "ssid": b.ssid, "freq": b.freq, "channel": channel(b.freq), "band": band(b.freq),
        "signal_dbm": b.signal_dbm, "security": b.security, "seen_ms": b.seen_ms, "associated": b.associated,
        "seen_boottime_ms": b.seen_boottime_ms,
        "channel_range": b.channel_range.map(|r| json!({"center_mhz": r.center_mhz, "width_mhz": r.width_mhz, "source": r.source})),
        "channel_80p80": b.channel_80p80.map(|r| json!({"primary_center_mhz": r.primary_center_mhz,
            "secondary_center_mhz": r.secondary_center_mhz}))})).collect();
    json!({"ok": true, "present": true, "iw": true, "iface": iface, "adapter_stamp": before, "networks": nets, "total": list.len(), "load": load, "dump_completed_boottime_ms": dump_completed_boottime_ms,
           "can_rescan": tool("nmcli").is_some()})
}

/// Asks NetworkManager for a fresh scan (polkit lets an active local session do that without a password).
pub fn rescan(req: &Value) -> Value {
    let iface = match scan_iface(req) { Ok(i) => i, Err(e) => return e };
    let Some(iface) = iface else { return json!({"ok": true, "present": false}) };
    let Some(bin) = tool("nmcli") else { return json!({"ok": false, "error": "nmcli (NetworkManager) is not installed"}) };
    let before = match scan_adapter_stamp(req, &iface) { Ok(stamp) => stamp, Err(error) => return error };
    match run(&bin, &["device", "wifi", "rescan", "ifname", &iface]) {
        Ok(_) if adapter_stamp(&iface).as_deref() == Some(before.as_str()) =>
            json!({"ok": true, "iface": iface, "adapter_stamp": before}),
        Ok(_) => json!({"ok": false, "iface": iface, "error": "Wireless adapter changed during rescan"}),
        Err(e) => json!({"ok": false, "iface": iface, "error": e}),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const LINK: &str = "Connected to 3c:52:82:11:22:33 (on wlan0)\n\tSSID: Home 5G: fast\n\tfreq: 5180.0\n\tsignal: -52 dBm\n\trx bitrate: 866.7 MBit/s 80MHz VHT-MCS 9 short GI\n\ttx bitrate: 780.0 MBit/s 80MHz VHT-MCS 8\n";
    const STATION: &str = "Station 3c:52:82:11:22:33 (on wlan0)\n\tinactive time:\t12 ms\n\ttx bytes:\t9000\n\ttx packets:\t10000\n\ttx retries:\t1500\n\ttx failed:\t40\n\tsignal:  \t-80 dBm\n\tsignal avg:\t-79 dBm\n\ttx bitrate:\t780.0 MBit/s 80MHz\n";

    #[test]
    fn link_fields() {
        let l = parse_link(LINK);
        assert!(l.connected);
        assert_eq!(l.ssid, "Home 5G: fast");  // a colon inside the name stays in it
        assert_eq!(l.signal_dbm, Some(-52));
        assert_eq!(l.rx_mbps, Some(866.7));
        assert_eq!(l.tx_mbps, Some(780.0));
        assert_eq!(parse_link("Not connected.\n"), Link::default());
    }

    #[test]
    fn station_counters() {
        let s = parse_station(STATION);
        assert_eq!((s.tx_packets, s.tx_retries, s.tx_failed), (10000, 1500, 40));
        assert_eq!(s.signal_dbm, Some(-80));  // "signal", not "signal avg"
        assert_eq!(s.tx_mbps, Some(780.0));
        assert_eq!(parse_station(""), Station::default());
    }

    #[test]
    fn warnings_follow_the_numbers() {
        // 15 % retries, but only 0.4 % failed: below the 2 % line.
        assert_eq!(warnings(&parse_station(STATION)), vec!["weak_signal", "retries"]);
        let good = Station { signal_dbm: Some(-55), tx_packets: 1000, tx_retries: 20, tx_failed: 1, ..Station::default() };
        assert!(warnings(&good).is_empty());
        // No packets yet: no division by zero, nothing to say about the rates.
        assert!(warnings(&Station::default()).is_empty());
    }

    #[test]
    fn link_and_info_extras() {
        let l = parse_link(LINK);
        assert_eq!((l.bssid.as_str(), l.freq), ("3c:52:82:11:22:33", Some(5180)));
        assert_eq!(l.tx_mode, "780.0 MBit/s 80MHz VHT-MCS 8");
        let i = parse_info("Interface wlan0\n\tifindex 3\n\taddr 3c:52:82:aa:bb:cc\n\ttype managed\n\twiphy 0\n\tchannel 36 (5180 MHz), width: 80 MHz, center1: 5210 MHz\n\ttxpower 22.00 dBm\n");
        assert_eq!((i.addr.as_str(), i.wiphy, i.channel, i.freq, i.width_mhz, i.txpower_dbm),
                   ("3c:52:82:aa:bb:cc", Some(0), Some(36), Some(5180), Some(80), Some(22.0)));
        assert_eq!(parse_info("Interface wlan0\n\ttype managed\n"), Info::default());
    }

    #[test]
    fn station_extras() {
        let s = parse_station("Station 3c (on wlan0)\n\trx bytes:\t5000\n\trx packets:\t40\n\tbeacon loss:\t2\n\tconnected time:\t3661 seconds\n\tsignal avg:\t-61 dBm\n\texpected throughput:\t866.700Mbps\n");
        assert_eq!((s.rx_bytes, s.rx_packets, s.beacon_loss, s.connected_secs, s.signal_avg_dbm, s.expected_mbps),
                   (5000, 40, 2, Some(3661), Some(-61), Some(866.7)));
    }

    #[test]
    fn channels_bands_and_generations() {
        assert_eq!((band(2437), channel(2437)), ("2.4", Some(6)));
        assert_eq!((band(2484), channel(2484)), ("2.4", Some(14)));
        assert_eq!((band(5180), channel(5180)), ("5", Some(36)));
        assert_eq!((band(5955), channel(5955)), ("6", Some(1)));
        assert_eq!((band(5935), channel(5935)), ("6", Some(2)));
        for freq in [2413, 5181, 5956] {
            assert_eq!(channel(freq), None);
        }
        assert_eq!((band(900), channel(900)), ("", None));
        assert_eq!(standard("866.7 MBit/s 80MHz VHT-MCS 9"), "ac");
        assert_eq!(standard("1201.0 MBit/s 80MHz HE-MCS 11 HE-NSS 2"), "ax");
        assert_eq!(standard("2882.0 MBit/s 160MHz EHT-MCS 11"), "be");
        assert_eq!(standard("144.4 MBit/s MCS 15 short GI"), "n");
        assert_eq!(standard("54.0 MBit/s"), "legacy");
        assert_eq!(standard(""), "");
    }

    const SCAN: &str = "BSS 3c:52:82:11:22:33(on wlan0) -- associated\n\tlast seen: 1234.5s [boottime]\n\tTSF: 1 usec\n\tfreq: 5180\n\tcapability: ESS Privacy ShortSlotTime (0x0411)\n\tsignal: -52.00 dBm\n\tlast seen: 120 ms ago\n\tSSID: Home 5G: fast\n\tRSN:\t * Version: 1\n\t\t * Group cipher: CCMP\n\t\t * Pairwise ciphers: CCMP\n\t\t * Authentication suites: PSK SAE\n\t\t * Capabilities: 16-PTKSA-RC (0x000c)\n\tHT operation:\n\t\t * primary channel: 36\nBSS aa:bb:cc:00:00:01(on wlan0)\n\tfreq: 2437.0\n\tcapability: ESS Privacy (0x0411)\n\tsignal: -71.00 dBm\n\tlast seen: 900 ms ago\n\tSSID: Cafe\n\tWPA:\t * Version: 1\n\t\t * Authentication suites: PSK\nBSS aa:bb:cc:00:00:02(on wlan0)\n\tfreq: 2442\n\tcapability: ESS (0x0401)\n\tsignal: -60.00 dBm\n\tSSID: \nBSS aa:bb:cc:00:00:03(on wlan0)\n\tfreq: 2412\n\tcapability: ESS Privacy (0x0411)\n\tsignal: -80.00 dBm\n\tSSID: Old\nBSS aa:bb:cc:00:00:04(on wlan0)\n\tfreq: 2437\n\tcapability: ESS Privacy (0x0411)\n\tsignal: -85.00 dBm\n\tSSID: Corp\n\tRSN:\t * Version: 1\n\t\t * Authentication suites: IEEE 802.1X\n";

    #[test]
    fn scan_results() {
        let l = parse_scan(SCAN);
        assert_eq!(l.len(), 5);
        // strongest first
        assert_eq!(l.iter().map(|b| b.signal_dbm.unwrap()).collect::<Vec<_>>(), vec![-52, -60, -71, -80, -85]);
        let home = &l[0];
        assert_eq!((home.ssid.as_str(), home.bssid.as_str(), home.freq, home.associated, home.seen_ms),
                   ("Home 5G: fast", "3c:52:82:11:22:33", 5180, true, Some(120)));
        assert_eq!(home.security, "WPA2/WPA3");
        assert_eq!((l[1].ssid.as_str(), l[1].security), ("", "Open"));  // hidden network, no Privacy bit
        assert_eq!(l[2].security, "WPA");
        assert_eq!(l[3].security, "WEP");
        assert_eq!(l[4].security, "Enterprise");
        assert!(parse_scan("").is_empty());
    }

    #[test]
    fn scan_rejects_nonfinite_and_wrong_units() {
        let result = parse_scan("BSS aa:bb:cc:00:00:01(on wlan0)\n\tfreq: 2437.5\n\tsignal: NaN dBm\n\tlast seen: 2 seconds ago\n");
        assert_eq!(result.len(), 1);
        assert_eq!(result[0].freq, 0);
        assert_eq!(result[0].signal_dbm, None);
        assert_eq!(result[0].seen_ms, None);
    }

    #[test]
    fn advertised_channel_ranges_and_timestamp() {
        fn scan(freq: u32, operation: &str) -> Bss {
            parse_scan(&format!("BSS aa:bb:cc:00:00:01(on wlan0)\n\tfreq: {freq}\n\tlast seen: 1234.5s [boottime]\n\tSSID: \n{operation}")).remove(0)
        }
        let ht = "\tHT operation:\n\t\t * primary channel: 36\n\t\t * secondary channel offset: above\n\t\t * STA channel width: any\n";
        let vht = "\tVHT operation:\n\t\t * channel width: 1 (80 MHz)\n\t\t * center freq segment 1: 42\n\t\t * center freq segment 2: 0\n";
        let b = scan(5180,ht);
        assert_eq!(b.seen_boottime_ms, Some(1234500)); assert!(b.ssid.is_empty());
        assert_eq!(b.channel_range, Some(ChannelRange {center_mhz:5190,width_mhz:40,source:"HT operation"}));
        assert_eq!(scan(5180,&format!("{ht}{vht}")).channel_range.unwrap().center_mhz,5210);
        assert!(scan(5180, &format!("{ht}{}", vht.replace("segment 1: 42", "segment 1: 38"))).channel_range.is_none());
        assert_eq!(scan(5180,&format!("{ht}{}",vht.replace("segment 2: 0","segment 2: 50"))).channel_range.unwrap().width_mhz,160);
        for op in [format!("{ht}{vht}\tEHT Operation:\n"),format!("{ht}{ht}"),format!("{ht}{}",vht.replace("segment 2: 0","segment 2: 106")),ht.replace("primary channel: 36","primary channel: 40")] {
            assert!(scan(5180,&op).channel_range.is_none());
        }
        assert!(scan(5955,ht).channel_range.is_none());
        assert!(scan(5180, "\tSSID: HT operation:\n").channel_range.is_none());
        assert_eq!(seen_boottime("0.001s [boottime]"),Some(1));
        assert_eq!(seen_boottime("1.999999999 s [boottime]"),Some(1999));
        for bad in ["-1s [boottime]","NaNs [boottime]","18446744073709551615s [boottime]","1e3s [boottime]","1 ms ago"] { assert!(seen_boottime(bad).is_none()); }
    }

    #[test]
    fn channel_load_counts_overlap_only_on_2_4() {
        let at = |freq, associated| Bss { freq, associated, ..Bss::default() };
        // Me on channel 6; one more on 6, one on 11 (5 away: no overlap), one on 4 (2 away: overlaps), one on 5 GHz.
        let l = vec![at(2437, true), at(2437, false), at(2462, false), at(2427, false), at(5180, false)];
        assert_eq!(channel_load(&l), Some((6, 1, 1)));
        assert_eq!(channel_load(&[at(2437, false)]), None);
        // 5 GHz: neighbours on another channel never count.
        assert_eq!(channel_load(&[at(5180, true), at(5200, false), at(5180, false)]), Some((36, 1, 0)));
    }

    const PHY: &str = "Wiphy phy0\n\tmax # scan SSIDs: 20\n\tSupported interface modes:\n\t\t * IBSS\n\t\t * managed\n\t\t * AP\n\t\t * monitor\n\tBand 1:\n\t\tCapabilities: 0x19ef\n\t\t\tRX LDPC\n\t\tHE Iftypes: managed\n\t\tFrequencies:\n\t\t\t* 2412 MHz [1] (22.0 dBm)\n\t\t\t* 2467 MHz [12] (disabled)\n\t\t\t* 2472 MHz [13] (20.0 dBm) (no IR)\n\tBand 2:\n\t\tCapabilities: 0x19ef\n\t\tVHT Capabilities (0x339071b2):\n\t\tHE Iftypes: managed\n\t\tHE Iftypes: AP\n\t\tFrequencies:\n\t\t\t* 5180 MHz [36] (23.0 dBm)\n\t\t\t* 5260 MHz [52] (23.0 dBm) (no IR, radar detection)\n\tBand 4:\n\t\tHE Iftypes: managed\n\t\tEHT Iftypes: managed\n\t\tFrequencies:\n\t\t\t* 5955 MHz [1] (12.0 dBm)\n";

    #[test]
    fn phy_bands_and_modes() {
        let (bands, modes) = parse_phy(PHY);
        assert_eq!(modes, vec!["IBSS", "managed", "AP", "monitor"]);
        assert_eq!(bands.len(), 3);
        assert_eq!((bands[0].name, bands[0].channels, bands[0].max_dbm, bands[0].standards.clone()), ("2.4", 2, Some(22.0), vec!["n", "ax"]));
        assert_eq!((bands[1].name, bands[1].channels, bands[1].standards.clone()), ("5", 2, vec!["n", "ac", "ax"]));
        assert_eq!((bands[2].name, bands[2].channels, bands[2].standards.clone()), ("6", 1, vec!["ax", "be"]));
        assert_eq!(parse_phy("").0.len(), 0);
    }

    #[test]
    fn regulatory_domain() {
        assert_eq!(parse_reg("global\ncountry DE: DFS-ETSI\n\t(2400 - 2483 @ 40), (N/A, 20)\n").as_deref(), Some("DE"));
        assert_eq!(parse_reg("global\ncountry 00: DFS-UNSET\n").as_deref(), Some("00"));
        assert_eq!(parse_reg(""), None);
    }
}
