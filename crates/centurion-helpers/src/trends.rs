//! Trend numbers for two Health charts, both read-only and run as the user when their page is shown:
//! the battery wear forecast (Health → Battery use, from battery.json's daily capacity samples) and the
//! kernel events per day and kind (Health → Monitor, from health.log). Nothing is measured here.
//!
//! stdin:  {"op":"battery_trend"}  stdout: {"ok": true, "status", "samples", "health", "months_to_80"}
//! stdin:  {"op":"event_days"}     stdout: {"ok": true, "days": [{"d": "YYYY-MM-DD", "kinds": {…}}]}, oldest first

use crate::overview::{local_clock, parse_local, read_json, read_tail, state_dir, ymd};
use serde_json::{json, Map, Value};
use std::collections::{BTreeMap, HashMap};

const DAY: i64 = 86_400;
/// The forecast looks at this many days of daily samples.
const WINDOW_DAYS: i64 = 180;
/// Fewer daily samples than this and no forecast is shown.
const MIN_SAMPLES: usize = 30;
/// End of life the forecast counts to, in percent of design capacity (the Overview's "ok" threshold).
const TARGET_PCT: f64 = 80.0;
const MONTH_DAYS: f64 = 30.44;
/// Days on Monitor's event chart, today included.
const EVENT_DAYS: i64 = 30;
/// Bytes read from the end of each health.log (the Overview reads the same much).
const TAIL_BYTES: u64 = 2 << 20;

/// Local day number (days since 1970-01-01) of a "YYYY-MM-DD" date.
fn day_number(date: &str) -> Option<i64> {
    Some(parse_local(&format!("{date} 00:00:00"), 0)?.div_euclid(DAY))
}

/// Wear forecast from `capacity` (battery.json's samples: "d", "full", "design"). Health is full ÷ design in
/// percent. `today` is the local day number. Least squares over the samples of the last WINDOW_DAYS.
pub fn wear(capacity: &Value, today: i64) -> Value {
    let mut pts: Vec<(f64, f64)> = Vec::new();
    for e in capacity.as_array().into_iter().flatten() {
        let (Some(d), Some(full), Some(design)) = (e["d"].as_str().and_then(day_number), e["full"].as_f64(), e["design"].as_f64()) else { continue };
        let age = today - d;
        if design <= 0.0 || !(0..WINDOW_DAYS).contains(&age) { continue; }
        pts.push((d as f64, 100.0 * full / design));
    }
    let n = pts.len();
    if n < MIN_SAMPLES { return json!({"ok": true, "status": "insufficient", "samples": n}); }
    let mean_x = pts.iter().map(|p| p.0).sum::<f64>() / n as f64;
    let mean_y = pts.iter().map(|p| p.1).sum::<f64>() / n as f64;
    let sxx: f64 = pts.iter().map(|p| (p.0 - mean_x).powi(2)).sum();
    if sxx <= 0.0 { return json!({"ok": true, "status": "insufficient", "samples": n}); }
    let slope = pts.iter().map(|p| (p.0 - mean_x) * (p.1 - mean_y)).sum::<f64>() / sxx;  // percent per day
    let health = mean_y + slope * (today as f64 - mean_x);
    if slope >= 0.0 { return json!({"ok": true, "status": "no_decline", "samples": n, "health": health}); }
    if health <= TARGET_PCT { return json!({"ok": true, "status": "at_or_below", "samples": n, "health": health}); }
    json!({"ok": true, "status": "ok", "samples": n, "health": health,
           "months_to_80": (health - TARGET_PCT) / -slope / MONTH_DAYS})
}

/// Op "battery_trend".
pub fn battery_request(_req: &Value) -> Value {
    let (now, offset) = local_clock();
    let state = read_json(&state_dir().join("battery.json")).unwrap_or(Value::Null);
    wear(&state["capacity"], (now + offset).div_euclid(DAY))
}

/// Kernel events per local day and kind over the EVENT_DAYS days up to `today` (local day number), oldest first.
/// `log` is health.log text: "date time  boot=… seq=…  LEVEL    KIND    title  |  text" per line.
pub fn event_days(log: &str, today: i64) -> Value {
    let first = today - (EVENT_DAYS - 1);
    let dates: Vec<String> = (first..=today).map(|d| ymd(d * DAY, 0)).collect();
    let index: HashMap<&str, usize> = dates.iter().enumerate().map(|(i, d)| (d.as_str(), i)).collect();
    let mut counts: Vec<BTreeMap<String, u64>> = vec![BTreeMap::new(); dates.len()];
    for line in log.lines() {
        let t: Vec<&str> = line.split_whitespace().take(6).collect();
        if t.len() < 6 || !t[2].starts_with("boot=") || !t[3].starts_with("seq=") { continue; }
        let Some(&i) = index.get(t[0]) else { continue };
        *counts[i].entry(t[5].to_ascii_lowercase()).or_insert(0) += 1;
    }
    let days: Vec<Value> = dates.iter().zip(counts).map(|(d, kinds)| {
        let mut k = Map::new();
        for (kind, n) in kinds { k.insert(kind, json!(n)); }
        json!({"d": d, "kinds": k})
    }).collect();
    json!({"ok": true, "days": days})
}

/// Op "event_days".
pub fn event_request(_req: &Value) -> Value {
    let (now, offset) = local_clock();
    let st = state_dir();
    let mut log = String::new();
    for name in ["health.log.1", "health.log"] {
        if let Some(t) = read_tail(&st.join(name), TAIL_BYTES) { log.push_str(&t); log.push('\n'); }
    }
    event_days(&log, (now + offset).div_euclid(DAY))
}

#[cfg(test)]
mod tests {
    use super::*;

    const TODAY: i64 = 20_000;

    fn date(back: i64) -> String { ymd((TODAY - back) * DAY, 0) }

    /// One capacity sample per day for `n` days ending today; health falls by `per_day` percent a day from `start`.
    fn samples(n: i64, start: f64, per_day: f64, design: f64) -> Value {
        Value::Array((0..n).rev().map(|back| {
            let health = start - per_day * (n - 1 - back) as f64;
            json!({"d": date(back), "full": design * health / 100.0, "design": design})
        }).collect())
    }

    #[test]
    fn wear_projects_the_linear_fall_to_80() {
        // Health 95 - 0.1 a day, last sample today at 92.1: 12.1 points to 80 at 0.1 a day.
        let v = wear(&samples(30, 95.0, 0.1, 50.0), TODAY);
        assert_eq!(v["status"], "ok");
        assert_eq!(v["samples"], 30);
        assert!((v["health"].as_f64().unwrap() - 92.1).abs() < 1e-6);
        assert!((v["months_to_80"].as_f64().unwrap() - 12.1 / 0.1 / MONTH_DAYS).abs() < 1e-6);
    }

    #[test]
    fn wear_needs_thirty_samples() {
        assert_eq!(wear(&samples(29, 95.0, 0.1, 50.0), TODAY)["status"], "insufficient");
        assert_eq!(wear(&samples(30, 95.0, 0.1, 50.0), TODAY)["status"], "ok");
    }

    #[test]
    fn wear_ignores_samples_outside_the_window_and_without_design() {
        let mut v = samples(30, 95.0, 0.1, 50.0);
        // Thirty samples 200 days old do not count.
        let old: Vec<Value> = (0..30).map(|i| json!({"d": date(200 + i), "full": 40.0, "design": 50.0})).collect();
        v.as_array_mut().unwrap().extend(old);
        let v = wear(&v, TODAY);
        assert_eq!(v["samples"], 30);
        // No design capacity: health is unknown.
        let no_design: Vec<Value> = (0..30).map(|i| json!({"d": date(i), "full": 40.0})).collect();
        assert_eq!(wear(&Value::Array(no_design), TODAY)["status"], "insufficient");
    }

    #[test]
    fn wear_no_forecast_when_not_falling() {
        assert_eq!(wear(&samples(30, 90.0, 0.0, 50.0), TODAY)["status"], "no_decline");
        assert_eq!(wear(&samples(30, 85.0, -0.05, 50.0), TODAY)["status"], "no_decline");
    }

    #[test]
    fn wear_already_at_or_below_target() {
        let v = wear(&samples(30, 85.0, 0.2, 50.0), TODAY);
        assert_eq!(v["status"], "at_or_below");
        assert!(v["health"].as_f64().unwrap() <= TARGET_PCT);
    }

    fn line(back: i64, level: &str, kind: &str) -> String {
        format!("{} 10:00:00  boot=abcdef12 seq=7  {level:<8} {kind:<6}  title  |  text", date(back))
    }

    #[test]
    fn event_days_counts_per_day_and_kind() {
        let log = [line(0, "ERROR", "XID"), line(0, "CRITICAL", "XID"), line(0, "ERROR", "NVME"), line(1, "WARN", "OOM")].join("\n");
        let v = event_days(&log, TODAY);
        let days = v["days"].as_array().unwrap();
        assert_eq!(days.len(), EVENT_DAYS as usize);
        assert_eq!(days.last().unwrap()["d"], date(0));
        assert_eq!(days.last().unwrap()["kinds"]["xid"], 2);
        assert_eq!(days.last().unwrap()["kinds"]["nvme"], 1);
        assert_eq!(days[days.len() - 2]["kinds"]["oom"], 1);
        assert_eq!(days[0]["kinds"].as_object().unwrap().len(), 0);
    }

    #[test]
    fn event_days_skips_old_and_foreign_lines() {
        let log = [
            line(EVENT_DAYS, "ERROR", "XID"),        // outside the window
            "not a log line at all".to_string(),
            format!("{} 10:00:00  seq=1 ERROR XID  x", date(0)),  // no boot= token
            "".to_string(),
        ].join("\n");
        let v = event_days(&log, TODAY);
        assert!(v["days"].as_array().unwrap().iter().all(|d| d["kinds"].as_object().unwrap().is_empty()));
    }
}
