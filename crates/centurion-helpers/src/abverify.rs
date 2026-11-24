//! Evidence binding for a verified (not manually overridden) autotune Apply.
//! Called under the tuning lock; this does not lock external kernel/firmware writers.
use serde_json::{json, Map, Value};
use crate::tune;

pub const VERSION: u64 = 3;

/// Bind the exact concrete write plan and its original values. Do not accept
/// a mixed/summary value or a replaced storage target as evidence.
pub fn baseline(values: &Map<String, Value>) -> Result<Value, String> {
    let mut evidence = Map::new();
    for (key, value) in values {
        let t = tune::find(key).ok_or_else(|| format!("unknown key {key}"))?;
        let valid = tune::validate(t, value)?;
        let mut plan = tune::plan(t, &valid)?;
        plan.sort_by(|a, b| a.0.cmp(&b.0));
        if plan.is_empty() || plan.windows(2).any(|w| w[0].0 == w[1].0) { return Err(format!("{key}: ambiguous or empty target plan")); }
        let mut rows = Vec::new();
        for (path, data) in plan {
            let stamp = if tune::scoped_storage(t) { Some(tune::storage_stamp(t, &path).ok_or("missing storage identity")?) } else { None };
            let original = tune::baseline_value(t, &path).ok_or_else(|| format!("{key}: baseline unavailable"))?;
            if tune::scoped_storage(t) && tune::storage_stamp(t, &path) != stamp { return Err(format!("{key}: target replaced")); }
            rows.push(json!([path, data, original, stamp]));
        }
        evidence.insert(key.clone(), Value::Array(rows));
    }
    Ok(Value::Object(evidence))
}

/// Pure receipt checks, before any context/baseline read. Old or partial
/// records are display-only. Integer JSON fields must actually be integers.
pub fn candidate(v: &Value, name: &str, values: &Map<String, Value>, boot: &str, now: u64) -> bool {
    if boot.is_empty() || v["verification_v"].as_u64() != Some(VERSION) || v["preset"].as_str() != Some(name)
        || v["boot_id"].as_str() != Some(boot) || v["verified_values"].as_object() != Some(values)
        || v["restore_confirmed"] != true || v["coverage_confirmed"] != true || v["context_confirmed"] != true
        || !v["ended_early"].is_null() || v["paired"] != true || v["complete"] != true || v["passed"] != true
        || v["oom_kills"]["boot"].as_u64() != Some(0) || v["oom_kills"]["preset"].as_u64() != Some(0)
        || !v["skipped"].as_array().is_some_and(|s| s.is_empty()) || !v["baseline"].is_object() || !v["measured_context"].is_object() { return false; }
    let Some(t) = v["t"].as_u64() else { return false };
    if t == 0 || t > now || now - t > 24 * 3600 { return false; }
    let Some(n) = v["rounds"].as_u64().filter(|n| (3..=12).contains(n)) else { return false };
    let Some(planned) = v["planned_rounds"].as_u64().filter(|p| *p >= n && *p <= 12) else { return false };
    if n != planned && v["sequential"]["decided_at"].as_u64() != Some(n) { return false; }
    let Some(ci) = v["decision_ci"].as_array().filter(|a| a.len() == 2) else { return false };
    ci[0].as_f64().zip(ci[1].as_f64()).is_some_and(|(lo, hi)| lo.is_finite() && hi.is_finite() && lo > 0.0 && hi >= lo)
}

pub fn authorize(name: &str, values: &Map<String, Value>) -> Result<(), String> {
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_err(|e| e.to_string())?.as_secs();
    let boot = crate::bootguard::boot_id();
    let raw = crate::read_root_file(crate::calib::FILE, 8 << 20).ok_or("verification signature unavailable")?;
    let sig: Value = serde_json::from_str(&raw).map_err(|e| e.to_string())?;
    let records = sig["verify"].as_array().ok_or("verification records unavailable")?;
    // Only the latest matching preset/value receipt can grant permission. A
    // newer failed measurement must not be bypassed by an older positive one.
    let record = records.iter().rev().find(|v| v["preset"].as_str() == Some(name) && v["verified_values"].as_object() == Some(values))
        .ok_or("no exact verification receipt")?;
    if !candidate(record, name, values, &boot, now) { return Err("latest verification is incomplete, expired or invalid".into()); }
    let current = crate::calctx::capture(None);
    if current["power"] == "?" || current != record["measured_context"] { return Err("measured power/firmware context changed; verify again".into()); }
    if baseline(values)? != record["baseline"] { return Err("verified target plan or baseline changed; verify again".into()); }
    if crate::calctx::capture(None) != current { return Err("context changed during verification recheck".into()); }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn receipt_rejects_legacy_incomplete_or_inconsistent() {
        let values = json!({"cpu.boost": "1"});
        let mut r = json!({"verification_v": VERSION, "preset":"p", "boot_id":"b", "verified_values":values,
            "restore_confirmed":true,"coverage_confirmed":true,"context_confirmed":true,"paired":true,"complete":true,"passed":true,
            "oom_kills":{"boot":0,"preset":0},"skipped":[],"baseline":{},"measured_context":{},"t":100,"rounds":3,"planned_rounds":4,
            "sequential":{"decided_at":3},"decision_ci":[0.01,0.2],"ended_early":null});
        let ok = |v: &Value| candidate(v,"p",values.as_object().unwrap(),"b",101);
        assert!(ok(&r));
        for key in ["coverage_confirmed","context_confirmed","restore_confirmed","complete","passed"] {
            let mut bad = r.clone(); bad[key] = json!(false); assert!(!ok(&bad), "{key}");
        }
        r["sequential"]["decided_at"] = json!(2); assert!(!ok(&r));
        r["sequential"]["decided_at"] = json!(3); r["verification_v"] = json!(2); assert!(!ok(&r));
    }
}
