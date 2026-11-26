//! Game sessions as field evidence: frame times a real game delivered under a known configuration.
//!
//! Logs: FLM CSV (interval_ns), MangoHud CSV (frametime, ms), or one number per line (ms).
//! Comparisons use completed launches with explicit workload labels and matching launch context.

use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Frames needed (after the first 10 % - loading, shader compilation - is dropped).
const MIN_FRAMES: usize = 300;

/// Frame-time figures of one session (ms).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Frames { pub med: f64, pub tail: f64, pub pacing: f64, pub n: usize }

/// Frame times (ms) from a log. FLM CSV: `interval_ns` column; MangoHud CSV: `frametime` (ms);
/// otherwise the first numeric column, taken as ms. Non-positive and absurd values are dropped.
pub fn parse_log(text: &str) -> Option<Vec<f64>> {
    let mut lines = text.lines().map(str::trim).filter(|l| !l.is_empty() && !l.starts_with('#'));
    let mut out = Vec::new();
    let (mut col, mut scale) = (0usize, 1.0f64);
    let mut header_found = false;
    let mut pending: Vec<&str> = Vec::new();
    // MangoHud writes two preamble lines (system info) before its header: look at the first few lines.
    for _ in 0..4 {
        let Some(l) = lines.next() else { break };
        let cells: Vec<&str> = l.split(',').map(str::trim).collect();
        if let Some(i) = cells.iter().position(|c| c.eq_ignore_ascii_case("interval_ns")) { col = i; scale = 1e-6; header_found = true; break; }
        if let Some(i) = cells.iter().position(|c| c.eq_ignore_ascii_case("frametime")) { col = i; scale = 1.0; header_found = true; break; }
        pending.push(l);
    }
    let all: Vec<&str> = if header_found { lines.collect() } else { pending.into_iter().chain(lines).collect() };
    for l in all {
        let Some(c) = l.split(',').nth(col) else { continue };
        let Ok(v) = c.trim().parse::<f64>() else { continue };
        let ms = v * scale;
        if ms > 0.05 && ms < 2000.0 { out.push(ms); }
    }
    (!out.is_empty()).then_some(out)
}

/// Median, tail (mean of the worst 1 %, at least 5 frames) and pacing (mean frame-to-frame change).
pub fn frames(ft: &[f64]) -> Option<Frames> {
    let skip = ft.len() / 10;
    let v = &ft[skip..];
    if v.len() < MIN_FRAMES { return None; }
    let pacing = v.windows(2).map(|w| (w[1] - w[0]).abs()).sum::<f64>() / (v.len() - 1) as f64;
    let mut s = v.to_vec();
    let med = crate::calib::median(&mut s)?;
    let k = (s.len() / 100).max(5);
    let tail = s[s.len() - k..].iter().sum::<f64>() / k as f64;
    Some(Frames { med, tail, pacing: pacing.max(1e-3), n: v.len() })
}

/// The session log of a user (written by centurion-gamemode).
pub fn session_log(home: &Path) -> PathBuf { home.join(".local/state/centurion/game-sessions.jsonl") }

/// Sessions from the log: start and end lines merged by id. {id, start, end?, game, preset?, values, ab?}
pub fn sessions(text: &str) -> Vec<Value> {
    let mut by: BTreeMap<String, Value> = BTreeMap::new();
    for l in text.lines() {
        let Ok(v) = serde_json::from_str::<Value>(l) else { continue };
        let Some(id) = v["id"].as_str().map(str::to_owned) else { continue };
        let e = by.entry(id).or_insert_with(|| json!({}));
        if let (Some(o), Some(n)) = (e.as_object_mut(), v.as_object()) { for (k, x) in n { o.insert(k.clone(), x.clone()); } }
    }
    by.into_values().filter(|v| v["start"].is_u64()).collect()
}

/// A log timestamp identifies a launch only when exactly one window fits.
pub fn match_session(sessions: &[Value], mtime: u64) -> Option<&Value> {
    let mut candidates = sessions.iter().filter(|s| {
        let st = s["start"].as_u64().unwrap_or(u64::MAX);
        let end = s["end"].as_u64().unwrap_or(st.saturating_add(6 * 3600));
        st <= end && st <= mtime && mtime <= end.saturating_add(300)
    });
    let first = candidates.next()?;
    candidates.next().is_none().then_some(first)
}

/// Read back the active settings governed by the comparison's preset.
pub fn readback(values: &Value) -> Option<Value> {
    let values = values.as_object()?;
    // A game launch may soft-park cores without changing the tunable's readback.
    if values.contains_key("cpu.ccd_park") { return None; }
    crate::abverify::baseline(values).ok()
}

/// Verify all planned writes reached the same targets captured before the launch.
pub fn applied_targets(before: &Value, after: &Value) -> bool {
    let Some(before) = before.as_object() else { return false };
    let Some(after) = after.as_object() else { return false };
    if before.is_empty() || before == after || before.len() != after.len() { return false; }
    before.iter().all(|(key, reference)| {
        let Some(reference) = reference.as_array() else { return false };
        let Some(observed) = after.get(key).and_then(Value::as_array) else { return false };
        reference.len() == observed.len() && !reference.is_empty() && reference.iter().zip(observed).all(|(a, b)| {
            a[0] == b[0] && a[1] == b[1] && a[3] == b[3]
                && b[1].as_str().zip(b[2].as_str()).is_some_and(|(planned, active)| planned.trim() == active)
        })
    })
}

/// The configuration a session ran with, as changes against the boot defaults (`boot`): an A
/// session (preset skipped) ran at the defaults - empty.
pub fn session_cfg(s: &Value, boot: &dyn Fn(&str) -> Option<String>) -> Vec<(String, String)> {
    if s["ab"] == "A" || s["preset"].is_null() { return Vec::new(); }
    let mut cfg: Vec<(String, String)> = s["values"].as_object().into_iter().flatten().filter_map(|(k, v)| {
        let v = match v { Value::String(x) => x.clone(), x => x.to_string() };
        (boot(k).as_deref() != Some(v.as_str())).then(|| (k.clone(), v))
    }).collect();
    cfg.sort();
    cfg
}

/// Keep only load-model predictions for the exact preset and reference conditions.
pub fn historical_prediction(history: &[Value], context: &Value, launch: u64) -> Option<Value> {
    let baseline = context["baseline"].as_object()?;
    let preset = context["preset"].as_object()?;
    if !context["boot"].is_string() || baseline.is_empty() || baseline.len() != preset.len() { return None; }
    history.iter().filter_map(|record| {
        let time = record["t"].as_u64().filter(|t| *t > 0 && *t <= launch)?;
        if record["goal"] != "gaming" || record["phase"] != "load" || record["complete"] != true
            || record["verification_v"].as_u64() != Some(crate::abverify::VERSION)
            || record["restore_confirmed"] != true || record["boot_id"] != context["boot"]
            || record["measured_context"] != context["platform"]
            || record["verified_values"].as_object() != Some(preset) { return None; }
        if record["baseline"].as_object() != Some(baseline) { return None; }
        let predicted = record["predicted"].as_array().filter(|v| v.len() == 3)?;
        let changes = predicted[0].as_u64()?;
        if changes == 0 || record["changes"].as_u64() != Some(changes) { return None; }
        let mean = predicted[1].as_f64().filter(|v| v.is_finite())?;
        let sd = predicted[2].as_f64().filter(|v| v.is_finite() && *v >= 0.0)?;
        Some((time, json!({"time": time, "mean": mean, "sd": sd})))
    }).max_by_key(|(time, _)| *time).map(|(_, prediction)| prediction)
}

/// Compare each changed launch against the median of at least two reference launches.
pub fn comparisons(records: &[Value]) -> Vec<Value> {
    let mut groups: BTreeMap<String, Vec<&Value>> = BTreeMap::new();
    for record in records {
        let context = &record["comparison"];
        if !context.is_object() || !record["id"].is_string() { continue; }
        if !["med", "tail", "pacing"].into_iter().all(|k| record[k].as_f64().is_some_and(|v| v > 0.0)) { continue; }
        groups.entry(context.to_string()).or_default().push(record);
    }
    let mut out = Vec::new();
    for (context, records) in groups {
        let references: BTreeMap<&str, &Value> = records.iter().filter(|r| r["ab"] == "A")
            .filter_map(|r| Some((r["id"].as_str()?, *r))).collect();
        if references.len() < 2 { continue; }
        let reference = |key: &str| crate::calib::median(&mut references.values()
            .filter_map(|r| r[key].as_f64()).collect::<Vec<_>>());
        let (Some(med), Some(tail), Some(pacing)) = (reference("med"), reference("tail"), reference("pacing")) else { continue; };
        let group = format!("{:016x}", crate::model::fnv(context.as_bytes()));
        for record in records.into_iter().filter(|r| r["ab"] == "B") {
            if references.contains_key(record["id"].as_str().unwrap_or_default()) { continue; }
            out.push(json!({"id": record["id"], "group": group, "references": references.keys().collect::<Vec<_>>(), "prediction": record["prediction"],
                "med": (1.0 - record["med"].as_f64().unwrap_or(med) / med) * 100.0,
                "tail": (1.0 - record["tail"].as_f64().unwrap_or(tail) / tail) * 100.0,
                "pacing": (1.0 - record["pacing"].as_f64().unwrap_or(pacing) / pacing) * 100.0}));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn logs_parse_in_all_three_shapes() {
        let flm = "frame,flip_ns,interval_ns,slot_mean_ns\n1,100,4000000,4100000\n2,200,4200000,4100000\n";
        assert_eq!(parse_log(flm).unwrap(), vec![4.0, 4.2]);
        let mango = "os,cpu,gpu\nLinux,Ryzen,RTX\nfps,frametime,cpu_load\n240,4.1,30\n238,4.2,31\n";
        assert_eq!(parse_log(mango).unwrap(), vec![4.1, 4.2]);
        assert_eq!(parse_log("8.3\n8.4\n#c\n\n0\n").unwrap(), vec![8.3, 8.4]);
        assert!(parse_log("a,b\nx,y\n").is_none());
    }

    #[test]
    fn comparisons_require_distinct_references_and_identical_context() {
        let ft: Vec<f64> = (0..1000).map(|i| if i % 100 == 99 { 20.0 } else { 8.0 }).collect();
        let f = frames(&ft).unwrap();
        assert!((f.med - 8.0).abs() < 1e-9 && f.tail > 8.0 && f.pacing > 0.0 && f.n == 900);
        assert!(frames(&ft[..200]).is_none());
        let rec = |id: &str, ab: &str, workload: &str, med: f64| json!({
            "id": id, "ab": ab, "comparison": {"workload": workload}, "med": med, "tail": med * 2.0, "pacing": 0.5
        });
        let a = rec("1", "A", "g", 10.0);
        let b = rec("3", "B", "g", 9.0);
        assert!(comparisons(&[a.clone(), a.clone(), b.clone()]).is_empty());
        assert!(comparisons(&[a.clone(), rec("2", "A", "different", 10.0), b.clone()]).is_empty());
        let found = comparisons(&[a, rec("2", "A", "g", 10.0), b]);
        assert_eq!(found.len(), 1);
        assert_eq!(found[0]["references"], json!(["1", "2"]));
        assert!((found[0]["med"].as_f64().unwrap() - 10.0).abs() < 1e-9);
    }

    #[test]
    fn write_readback_checks_all_targets_and_their_identity() {
        let before = json!({"setting":[["path1", "2", "1", null], ["path2", "2", "1", "dev"]]});
        let mut after = before.clone();
        after["setting"][0][2] = json!("2");
        assert!(!applied_targets(&before, &after));
        after["setting"][1][2] = json!("2");
        assert!(applied_targets(&before, &after));
        after["setting"][1][3] = json!("new dev");
        assert!(!applied_targets(&before, &after));
    }

    #[test]
    fn historical_prediction_requires_the_same_recorded_conditions() {
        let context = json!({"boot":"boot","platform":{"power":"ac"},"preset":{"setting":"on"},"baseline":{"setting":[["path","on","off",null]]}});
        let record = json!({"t":10,"goal":"gaming","phase":"load","complete":true,"restore_confirmed":true,
            "verification_v":crate::abverify::VERSION,
            "boot_id":"boot","measured_context":{"power":"ac"},"verified_values":{"setting":"on"},
            "baseline":{"setting":[["path","on","off",null]]},"changes":1,"predicted":[1,0.2,0.1]});
        assert_eq!(historical_prediction(std::slice::from_ref(&record), &context, 10),
            Some(json!({"time":10,"mean":0.2,"sd":0.1})));
        assert!(historical_prediction(std::slice::from_ref(&record), &context, 9).is_none());
        let mut wrong_boot = record.clone();
        wrong_boot["boot_id"] = json!("other");
        assert!(historical_prediction(&[wrong_boot], &context, 10).is_none());
        let mut different = context;
        different["baseline"]["setting"][0][2] = json!("auto");
        assert!(historical_prediction(&[record], &different, 10).is_none());
    }

    #[test]
    fn sessions_merge_and_match() {
        let log = "{\"id\":\"a\",\"start\":100,\"game\":\"x\",\"preset\":\"P\",\"values\":{\"cpu.epp\":\"performance\",\"vm.swappiness\":60},\"ab\":\"B\"}\n\
                   {\"id\":\"a\",\"end\":900}\n{\"id\":\"b\",\"start\":1000,\"game\":\"x\",\"preset\":null,\"values\":{},\"ab\":\"A\"}\n";
        let s = sessions(log);
        assert_eq!(s.len(), 2);
        assert_eq!(match_session(&s, 950).unwrap()["id"], "a");
        assert!(match_session(&s, 1050).is_none());
        assert_eq!(match_session(&s, 2000).unwrap()["id"], "b");
        assert!(match_session(&s, 50).is_none());
        let boot = |k: &str| Some(if k == "vm.swappiness" { "60".to_owned() } else { "balance_performance".to_owned() });
        assert_eq!(session_cfg(&s[0], &boot), vec![("cpu.epp".to_owned(), "performance".to_owned())]);
        assert!(session_cfg(&s[1], &boot).is_empty());
    }
}
