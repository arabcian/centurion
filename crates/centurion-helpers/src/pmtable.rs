//! Which power / current / thermal limit is holding the CPU back ("limit reasons" for Ryzen), from the
//! SMU's PM table that the ryzen_smu module exports (/sys/kernel/ryzen_smu_drv/pm_table, root-only).
//!
//! The table is an array of f32 whose layout changes with the table version. Only the head is used here,
//! and only for two layout families whose head is well known (ryzen_monitor, ZenStates, RyzenAdj):
//!
//!   desktop silicon (Matisse 0x24, Vermeer 0x38, Raphael / Dragon Range 0x54, Granite Ridge / Fire Range 0x62)
//!     [0] PPT limit W  [1] PPT W   [2] TDC limit A  [3] TDC A   [4] THM limit °C  [5] THM °C
//!     [6] FIT limit    [7] FIT     [8] EDC limit A  [9] EDC A
//!   mobile APUs (Renoir 0x37, Cezanne 0x40, Rembrandt 0x45, Phoenix / Hawk Point 0x4C, Strix 0x5D, Krackan 0x65)
//!     [0] STAPM limit [1] STAPM  [2] PPT fast limit [3] PPT fast  [4] PPT slow limit [5] PPT slow
//!     [8] TDC VDD limit [9] TDC VDD  [12] EDC VDD limit [13] EDC VDD  [16] THM limit [17] THM
//!
//! A layout is trusted only when every limit and value is physically plausible (range checks below);
//! otherwise nothing is reported and the GUI says the table was not recognised. Nothing is ever written.

use serde_json::{json, Map, Value};

pub const DIR: &str = "/sys/kernel/ryzen_smu_drv";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Layout { Desktop, Apu }

/// (name, unit, limit index, value index, plausible limit range)
type Field = (&'static str, &'static str, usize, usize, (f32, f32));

const DESKTOP: &[Field] = &[
    ("PPT", "W", 0, 1, (10.0, 500.0)),
    ("TDC", "A", 2, 3, (10.0, 400.0)),
    ("EDC", "A", 8, 9, (10.0, 600.0)),
    ("THM", "°C", 4, 5, (50.0, 115.0)),
];
const APU: &[Field] = &[
    ("STAPM", "W", 0, 1, (3.0, 200.0)),
    ("PPT fast", "W", 2, 3, (3.0, 250.0)),
    ("PPT slow", "W", 4, 5, (3.0, 250.0)),
    ("TDC", "A", 8, 9, (5.0, 300.0)),
    ("EDC", "A", 12, 13, (5.0, 400.0)),
    ("THM", "°C", 16, 17, (50.0, 115.0)),
];

pub fn layout_for(version: u32) -> Option<Layout> {
    match version >> 16 {
        0x24 | 0x38 | 0x54 | 0x62 => Some(Layout::Desktop),
        0x37 | 0x40 | 0x45 | 0x4C | 0x5D | 0x65 => Some(Layout::Apu),
        _ => None,
    }
}

fn fields(l: Layout) -> &'static [Field] { match l { Layout::Desktop => DESKTOP, Layout::Apu => APU } }

/// Decodes the head of a table; None when the layout is unknown or a number is implausible.
pub fn decode(version: u32, raw: &[u8]) -> Option<Vec<(&'static str, &'static str, f32, f32)>> {
    let l = layout_for(version)?;
    let f = |i: usize| -> Option<f32> {
        let b = raw.get(i * 4..i * 4 + 4)?;
        Some(f32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    };
    let mut out = Vec::new();
    for &(name, unit, li, vi, (lo, hi)) in fields(l) {
        let (lim, val) = (f(li)?, f(vi)?);
        if !lim.is_finite() || !val.is_finite() || lim < lo || lim > hi || val < 0.0 || val > lim * 1.6 + 5.0 { return None; }
        out.push((name, unit, lim, val));
    }
    Some(out)
}

fn read_version() -> Option<u32> {
    let raw = std::fs::read(format!("{DIR}/pm_table_version")).ok()?;
    // Binary u32 (little endian) in the driver; some forks print hex text.
    if raw.len() == 4 { return Some(u32::from_le_bytes([raw[0], raw[1], raw[2], raw[3]])); }
    let t = String::from_utf8_lossy(&raw);
    let t = t.trim().trim_start_matches("0x");
    u32::from_str_radix(t, 16).ok()
}

/// One sample for the Ryzen tab's monitor: {"version", "layout", "limits": [{name, unit, limit, value, pct}]}
/// or {"error"}.
pub fn sample() -> Value {
    if !std::path::Path::new(DIR).is_dir() { return json!({"error": "ryzen_smu is not loaded"}); }
    let Some(version) = read_version() else { return json!({"error": "ryzen_smu exports no PM table here"}) };
    let raw = match std::fs::read(format!("{DIR}/pm_table")) {
        Ok(r) => r,
        Err(e) => return json!({"error": format!("pm_table: {e}"), "version": format!("{version:#08x}")}),
    };
    match decode(version, &raw) {
        None => json!({"error": "PM table layout not recognised", "version": format!("{version:#08x}")}),
        Some(v) => {
            let limits: Vec<Value> = v.iter().map(|(n, u, l, x)| {
                let mut m = Map::new();
                m.insert("name".into(), json!(n));
                m.insert("unit".into(), json!(u));
                m.insert("limit".into(), json!((*l * 10.0).round() / 10.0));
                m.insert("value".into(), json!((*x * 10.0).round() / 10.0));
                m.insert("pct".into(), json!(((*x / *l) * 1000.0).round() / 10.0));
                Value::Object(m)
            }).collect();
            json!({"version": format!("{version:#08x}"), "layout": match layout_for(version) { Some(Layout::Apu) => "apu", _ => "desktop" }, "limits": limits})
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn table(v: &[f32]) -> Vec<u8> { v.iter().flat_map(|x| x.to_le_bytes()).collect() }

    #[test]
    fn desktop_head() {
        let raw = table(&[120.0, 85.5, 95.0, 60.0, 95.0, 81.0, 1.0, 0.5, 160.0, 140.0]);
        let d = decode(0x0062_0205, &raw).unwrap();
        assert_eq!(d[0], ("PPT", "W", 120.0, 85.5));
        assert_eq!(d[3].0, "THM");
        assert_eq!(d[2], ("EDC", "A", 160.0, 140.0));
    }

    #[test]
    fn implausible_is_refused() {
        // A different layout read with the desktop map: a "PPT limit" of 0.3 W is not a PPT limit.
        let raw = table(&[0.3, 0.2, 95.0, 60.0, 95.0, 81.0, 1.0, 0.5, 160.0, 140.0]);
        assert!(decode(0x0054_0104, &raw).is_none());
        assert!(decode(0x0099_0000, &table(&[120.0; 20])).is_none(), "unknown family");
        assert!(decode(0x0062_0205, &table(&[120.0, 85.0])).is_none(), "short table");
    }

    #[test]
    fn apu_head() {
        let mut v = vec![0.0f32; 20];
        v[0] = 45.0; v[1] = 30.0; v[2] = 65.0; v[3] = 40.0; v[4] = 54.0; v[5] = 35.0;
        v[8] = 60.0; v[9] = 30.0; v[12] = 90.0; v[13] = 50.0; v[16] = 100.0; v[17] = 70.0;
        let d = decode(0x0045_0005, &table(&v)).unwrap();
        assert_eq!(d.len(), 6);
        assert_eq!(d[5], ("THM", "°C", 100.0, 70.0));
    }
}
