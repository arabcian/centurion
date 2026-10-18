//! Root helper for the Ryzen Curve Optimizer applet.
//! Port of ryzen_curve_optimizer_helper.py -- same JSON protocol:
//!   {"op": "set_coall", "params": {"value": -20}}
//!   {"op": "set_coper_batch", "params": {"entries": [{ccd,ccx,core,coper}, ...]}}
//!   {"op": "reset"}
//!   {"op": "probe"}
//!       read-only: detected family, SMN backend, allowed offset range (no CO write).
//!   {"op": "monitor_stream", "params": {"interval_ms": n}}
//!       read-only: one line of per-CPU VID volts / core watts + package watts
//!       (ryzen_mon) every interval (500..10000 ms) until stdin reaches EOF.
//!       The request line must end with '\n' so stdin can stay open.
//!
//! The SMU is driven natively (centurion_helpers::smu): family table, mailbox addresses and
//! opcodes are fixed constants cross-checked against RyzenAdj, the answer is polled for at most
//! one second, and no opcode ever comes from the request. Out-of-range values are rejected,
//! never clamped.

use centurion_helpers::*;
use centurion_helpers::smu::Smu;
use centurion_helpers::stream;
use serde_json::{json, Map, Value};
use std::collections::HashSet;
use std::time::{Duration, Instant};

const MAX_STDIN_BYTES: usize = 64 * 1024;
const MAX_ENTRIES: usize = 64;
const FIELD_MAX: i64 = 0xF;
// Outer sanity bounds only; the per-family range (smu::Spec) is authoritative.
const CO_MIN: i64 = -50;
const CO_MAX: i64 = 20;

struct Invalid(String);

fn require_int(v: Option<&Value>, name: &str, lo: i64, hi: i64) -> Result<i64, Invalid> {
    let n = match v {
        Some(Value::Number(n)) if n.is_i64() || n.is_u64() => n.as_i64(),
        Some(other) => {
            let t = match other {
                Value::Null => "NoneType", Value::Bool(_) => "bool", Value::Number(_) => "float",
                Value::String(_) => "str", Value::Array(_) => "list", Value::Object(_) => "dict",
            };
            return Err(Invalid(format!("{name} must be an integer, got {t}")));
        }
        None => return Err(Invalid(format!("{name} must be an integer, got NoneType"))),
    };
    match n {
        Some(n) if (lo..=hi).contains(&n) => Ok(n),
        _ => Err(Invalid(format!("{name} out of range: {} (allowed {lo}..{hi})",
            v.map(|x| x.to_string()).unwrap_or_default()))),
    }
}

fn open_smu() -> Result<Smu, Value> {
    Smu::open().map_err(|e| json!({"ok": false, "error": e, "message": e}))
}

fn op_probe(_: &Map<String, Value>) -> Result<Value, Invalid> {
    Ok(match open_smu() {
        Ok(smu) => {
            let s = smu.spec();
            json!({"ok": true, "family": s.name, "mailbox": s.mailbox, "backend": smu.backend(),
                   "per_core": s.per_core, "max_ccd": s.max_ccd, "co_min": s.co_min, "co_max": s.co_max})
        }
        Err(e) => e,
    })
}

fn op_set_coall(p: &Map<String, Value>) -> Result<Value, Invalid> {
    let v = require_int(p.get("value"), "value", CO_MIN, CO_MAX)?;
    Ok(apply_coall(v))
}

fn apply_coall(v: i64) -> Value {
    let mut smu = match open_smu() { Ok(s) => s, Err(e) => return e };
    if let Err(e) = smu.spec().check_coall(v) { return json!({"ok": false, "error": e, "message": e}); }
    match smu.set_coall(v) {
        Ok(()) => json!({"ok": true, "message": "OK", "applied": {"coall": v}}),
        Err(e) => json!({"ok": false, "error": e.message(), "message": e.message(), "applied": {"coall": v}}),
    }
}

fn op_set_coper_batch(p: &Map<String, Value>) -> Result<Value, Invalid> {
    let entries = match p.get("entries") {
        Some(Value::Array(a)) if !a.is_empty() => a,
        _ => return Err(Invalid("entries must be a non-empty list".into())),
    };
    if entries.len() > MAX_ENTRIES {
        return Err(Invalid(format!("too many entries: {} (max {MAX_ENTRIES})", entries.len())));
    }

    // Validate everything before touching the CPU: no half-applied curves.
    let mut validated = Vec::with_capacity(entries.len());
    let mut seen = HashSet::new();
    for (i, e) in entries.iter().enumerate() {
        let e = e.as_object().ok_or_else(|| Invalid(format!("entry[{i}] must be an object")))?;
        let ccd = require_int(e.get("ccd"), &format!("entry[{i}].ccd"), 0, FIELD_MAX)?;
        let ccx = require_int(e.get("ccx"), &format!("entry[{i}].ccx"), 0, FIELD_MAX)?;
        let core = require_int(e.get("core"), &format!("entry[{i}].core"), 0, FIELD_MAX)?;
        let coper = require_int(e.get("coper"), &format!("entry[{i}].coper"), CO_MIN, CO_MAX)?;
        if !seen.insert((ccd, ccx, core)) {
            return Err(Invalid(format!("duplicate slot in batch: ccd={ccd} ccx={ccx} core={core}")));
        }
        validated.push((ccd, ccx, core, coper));
    }

    let mut smu = match open_smu() { Ok(s) => s, Err(e) => return Ok(e) };
    for (i, &(ccd, ccx, core, coper)) in validated.iter().enumerate() {
        smu.spec().check_coper(ccd, ccx, core, coper).map_err(|m| Invalid(format!("entry[{i}]: {m}")))?;
    }

    // The SMU cannot be read back, so the value to restore is the caller's "prev" (the last applied
    // offset it knows of) and otherwise 0 = stock.
    let prevs: Vec<Option<i64>> = entries.iter().map(|e| e.get("prev").and_then(Value::as_i64).filter(|v| (CO_MIN..=CO_MAX).contains(v))).collect();
    let mut all_ok = true;
    let mut touched: Vec<usize> = Vec::new(); // entries that may have reached the SMU
    let mut results: Vec<Value> = Vec::new();
    for (i, &(ccd, ccx, core, coper)) in validated.iter().enumerate() {
        let (ok, msg) = match smu.set_coper(ccd, ccx, core, coper) {
            Ok(()) => { touched.push(i); (true, "OK".to_owned()) }
            Err(e) => {
                if e.is_fatal() { touched.push(i); } // bus died mid-write: may have landed
                (false, e.message().to_owned())
            }
        };
        all_ok &= ok;
        results.push(json!({"ok": ok, "message": msg, "ccd": ccd, "ccx": ccx, "core": core, "coper": coper}));
        if !ok { break; } // all-or-nothing: stop at the first failure and undo
    }
    if all_ok { return Ok(json!({"ok": true, "results": results})); }
    for &(ccd, ccx, core, coper) in validated.iter().skip(results.len()) {
        results.push(json!({"ok": false, "skipped": true, "message": "not written (earlier entry failed)", "ccd": ccd, "ccx": ccx, "core": core, "coper": coper}));
    }
    let mut rolled: Vec<Value> = Vec::new();
    let mut explicit = true;
    let mut clean = true;
    for &i in touched.iter().rev() {
        let (ccd, ccx, core, _) = validated[i];
        let back = prevs[i].unwrap_or_else(|| { explicit = false; 0 });
        let r = smu.set_coper(ccd, ccx, core, back);
        clean &= r.is_ok();
        rolled.push(json!({"ok": r.is_ok(), "message": r.err().map_or("OK".to_owned(), |e| e.message().to_owned()), "ccd": ccd, "ccx": ccx, "core": core, "coper": back}));
    }
    // "original" only when every undo went to a value the caller named; otherwise the cores are at
    // stock (0), which may differ from what was active before this call.
    let state = if !clean { "unknown" } else if explicit { "original" } else { "stock" };
    Ok(json!({"ok": false, "results": results, "rollback": rolled, "state": state,
              "note": "SMU offsets cannot be read back; undo writes the caller's 'prev' per entry, else 0 (stock)"}))
}

fn op_reset(_: &Map<String, Value>) -> Result<Value, Invalid> {
    Ok(apply_coall(0))
}

/// Samples until the GUI closes stdin (tab hidden / app gone) or stdout goes away.
fn monitor_stream(p: &Map<String, Value>) -> Value {
    let mut s = match centurion_helpers::ryzen_mon::Sampler::open() {
        Ok(s) => s,
        Err(e) => return json!({"ok": false, "error": e}),
    };
    let interval = p.get("interval_ms").and_then(Value::as_u64).unwrap_or(1000).clamp(500, 10_000);
    let mut pending = Vec::new();
    // Hard ceiling: a GUI that hangs with the pipe open cannot keep a root sampler alive forever.
    let stop_at = Instant::now() + Duration::from_secs(4 * 3600);
    loop {
        if !stream::emit_line(&s.sample()) || Instant::now() >= stop_at { break; }
        if stream::wait_stdin(interval, &mut pending).is_none() { break; }
    }
    json!({"ok": true, "stream_end": true})
}

fn run() -> Value {
    let req = match stream::read_request_line(MAX_STDIN_BYTES) { Ok(v) => v, Err(e) => return e };
    let Some(obj) = req.as_object() else {
        return json!({"ok": false, "error": "payload must be a JSON object"});
    };
    let op = obj.get("op");
    if op.and_then(Value::as_str) == Some("monitor_stream") {  // read-only: allowed during calibration
        let empty = Map::new();
        return monitor_stream(obj.get("params").and_then(Value::as_object).unwrap_or(&empty));
    }
    let handler: fn(&Map<String, Value>) -> Result<Value, Invalid> =
        match op.and_then(Value::as_str) {
            Some("set_coall") => op_set_coall,
            Some("set_coper_batch") => op_set_coper_batch,
            Some("reset") => op_reset,
            Some("probe") => op_probe,
            _ => return json!({"ok": false, "error": format!("unknown op: {}",
                     op.map(|v| v.to_string()).unwrap_or_else(|| "None".into()))}),
        };
    if let Some(r) = refuse_during_calibration() { return r; }
    let empty = Map::new();
    let params = match obj.get("params") {
        None | Some(Value::Null) => &empty,
        Some(Value::Object(m)) => m,
        Some(_) => return json!({"ok": false, "error": "params must be a JSON object"}),
    };
    let out = handler(params).unwrap_or_else(|Invalid(m)| json!({"ok": false, "error": format!("invalid request: {m}")}));
    if out["ok"] == true { record_state(op.and_then(Value::as_str).unwrap_or(""), params); }
    out
}

/// What the SMU now holds (tmpfs: a reboot resets both): calibration records it as context, since
/// a curve offset changes the clock every temperature buys. All-core offsets replace the state;
/// per-core batches update their cores.
fn record_state(op: &str, params: &Map<String, Value>) {
    let mut st: Value = read_root_file(centurion_helpers::calctx::CO_STATE, 64 * 1024).and_then(|s| serde_json::from_str(&s).ok())
        .filter(|v: &Value| v["state"].is_object() || v["state"].is_string()).unwrap_or_else(|| json!({"state": "reset"}));
    match op {
        "reset" => st = json!({"state": "reset"}),
        "set_coall" => st = json!({"state": {"all": params.get("value").cloned().unwrap_or(Value::Null)}}),
        "set_coper_batch" => {
            if !st["state"].is_object() { st["state"] = json!({}); }
            for e in params.get("entries").and_then(Value::as_array).into_iter().flatten() {
                let k = format!("{}.{}.{}", e["ccd"], e["ccx"], e["core"]);
                st["state"][k] = e["coper"].clone();
            }
        }
        _ => return,
    }
    if secure_dir("/run/centurion").is_ok() { let _ = write_root_file(centurion_helpers::calctx::CO_STATE, st.to_string().as_bytes()); }
}

fn main() {
    init();
    std::process::exit(finish(run()));
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ints_strict() {
        assert!(require_int(Some(&json!(true)), "v", -50, 20).is_err());
        assert!(require_int(Some(&json!(-20.0)), "v", -50, 20).is_err());
        assert!(require_int(Some(&json!(21)), "v", -50, 20).is_err());
        assert_eq!(require_int(Some(&json!(-50)), "v", -50, 20).ok(), Some(-50));
    }
    #[test]
    fn duplicate_rejected_before_hardware() {
        let p = json!({"entries": [{"ccd":0,"ccx":0,"core":1,"coper":-10},
                                    {"ccd":0,"ccx":0,"core":1,"coper":-5}]});
        assert!(op_set_coper_batch(p.as_object().unwrap()).is_err());
    }
}
