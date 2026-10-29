//! Wi-Fi link health — Health → Network → Wi-Fi.
//!
//! Read-only and unprivileged: `iw dev <if> link` and `iw dev <if> station dump` are nl80211 reads.
//! The interface is the first wireless one under /sys/class/net; nothing is taken from the request.
//! No Wi-Fi interface → `present: false` (the GUI hides the tab). The GUI asks only while the tab is
//! on screen (every 3 s), so nothing runs in the background.

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
    pub ssid: String,
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

pub fn parse_link(text: &str) -> Link {
    let mut l = Link { connected: text.trim_start().starts_with("Connected to"), ..Link::default() };
    for (k, v) in fields(text) {
        match k {
            "SSID" => l.ssid = v.to_owned(),
            "signal" => l.signal_dbm = first(v),
            "rx bitrate" => l.rx_mbps = first(v),
            "tx bitrate" => l.tx_mbps = first(v),
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
            _ => {}
        }
    }
    s
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

/// The first interface with a Wi-Fi (nl80211 or wext) directory; one that is up comes before one that is not.
fn wireless_iface() -> Option<String> {
    let mut names: Vec<(bool, String)> = std::fs::read_dir("/sys/class/net").ok()?
        .filter_map(Result::ok)
        .filter(|e| e.path().join("phy80211").exists() || e.path().join("wireless").exists())
        .map(|e| {
            let up = std::fs::read_to_string(e.path().join("operstate")).is_ok_and(|s| s.trim() == "up");
            (!up, e.file_name().to_string_lossy().into_owned())
        })
        .collect();
    names.sort();
    names.into_iter().next().map(|(_, n)| n)
}

fn iw_bin() -> Option<PathBuf> {
    ["/usr/sbin", "/sbin", "/usr/bin", "/bin"].iter().map(|d| Path::new(d).join("iw")).find(|p| crate::trusted_path(p))
}

fn iw(bin: &Path, iface: &str, args: &[&str]) -> Result<String, String> {
    let out = Command::new(bin).arg("dev").arg(iface).args(args)
        .env_clear().env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin").env("LC_ALL", "C").current_dir("/")
        .output().map_err(|e| format!("iw: {e}"))?;
    if out.status.success() { Ok(String::from_utf8_lossy(&out.stdout).into_owned()) }
    else { Err(format!("iw dev {iface} {}: {}", args.join(" "), String::from_utf8_lossy(&out.stderr).trim())) }
}

/// Link and station counters of the first Wi-Fi interface. Errors are reported, not raised.
pub fn status() -> Value {
    let Some(iface) = wireless_iface() else { return json!({"ok": true, "present": false}) };
    let Some(bin) = iw_bin() else { return json!({"ok": true, "present": true, "iface": iface, "iw": false}) };
    let (link, station) = match (iw(&bin, &iface, &["link"]), iw(&bin, &iface, &["station", "dump"])) {
        (Ok(l), Ok(s)) => (parse_link(&l), parse_station(&s)),
        (Err(e), _) | (_, Err(e)) => return json!({"ok": false, "present": true, "iface": iface, "iw": true, "error": e}),
    };
    json!({"ok": true, "present": true, "iw": true, "iface": iface, "connected": link.connected, "ssid": link.ssid,
           "signal_dbm": station.signal_dbm.or(link.signal_dbm), "rx_mbps": link.rx_mbps,
           "tx_mbps": station.tx_mbps.or(link.tx_mbps), "tx_packets": station.tx_packets,
           "tx_retries": station.tx_retries, "tx_failed": station.tx_failed,
           "retry_pct": pct(station.tx_retries, station.tx_packets), "fail_pct": pct(station.tx_failed, station.tx_packets),
           "warn": warnings(&station)})
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
}
