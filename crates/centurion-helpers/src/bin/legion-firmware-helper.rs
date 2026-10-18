//! Root helper for firmware-persistent changes (pkexec target).
//!
//! Everything here survives a reboot or only takes effect at the next one, and
//! a bad value can leave the machine unable to POST or with a black screen.
//! That is why these operations live in their own binary: polkit pins one
//! action per helper path, and com.centurion.firmware.write always
//! asks for the administrator password (auth_admin, no keep, no silent grant
//! in 49-centurion.rules). The day-to-day knobs in the other
//! helpers stay password-less.
//!
//!   {"op": "aod_set", "values": {"tCL": 36, ...}}   BIOS DRAM timing overrides (AodSetupRpl)
//!       values below a field's "safe_min" (SPD / running value, see aod_get) are refused unless
//!       "confirm_below_safe": true
//!   {"op": "aod_restore"}                            write the newest AodSetupRpl backup back
//!   {"op": "set_fw_oc", "key": "pbo_scalar"|"boost_mhz"|"curve_optimizer", "value": n}
//!   {"op": "set_fw_oc_many", "values": {"pbo_scalar": n, ...}}
//!   {"op": "set_gpu_mode", "mode": "hybrid"|"dgpu", "force": bool}   MUX, next boot
//!   {"op": "set_igpu_mode", "mode": 0|1|2, "force": true}             only the forced override;
//!                                                   the guarded path stays in legion-gpu-helper
//! Same contract as the other helpers: one JSON object on stdin, one JSON line out.

use centurion_helpers::{legion_wmi, memory_spd, *};
use serde_json::{json, Value};

fn run() -> Value {
    let req = match read_request(8192) { Ok(v) => v, Err(e) => return e };
    let Some(o) = req.as_object() else { return json!({"ok": false, "error": "payload must be an object"}) };
    let force = o.get("force").and_then(Value::as_bool).unwrap_or(false);
    match o.get("op").and_then(Value::as_str) {
        Some("aod_set") => memory_spd::aod_set(o.get("values"), o.get("confirm_below_safe").and_then(Value::as_bool).unwrap_or(false)),
        Some("aod_restore") => memory_spd::aod_restore(),
        Some("set_fw_oc") => match (o.get("key").and_then(Value::as_str), o.get("value").and_then(Value::as_i64)) {
            (Some(k), Some(v)) => legion_wmi::set_fw_oc(k, v),
            _ => json!({"ok": false, "error": "'key' and 'value' required"}),
        },
        // Several tunes in one privileged call (one password prompt).
        Some("set_fw_oc_many") => match o.get("values").and_then(Value::as_object) {
            Some(m) if !m.is_empty() => {
                // Validate everything first so a bad entry cannot leave a partial change.
                let st = legion_wmi::fw_oc_status();
                let mut todo: Vec<(&String, i64)> = Vec::new();
                let mut errs = Vec::new();
                for (k, v) in m {
                    let Some(n) = v.as_i64() else { errs.push(format!("{k}: value must be an integer")); continue };
                    match st.get("tunes").and_then(|t| t.get(k)) {
                        Some(t) if n >= t["min"].as_i64().unwrap_or(0) && n <= t["max"].as_i64().unwrap_or(0) => todo.push((k, n)),
                        Some(t) => errs.push(format!("{k}: {n} outside {}..{}", t["min"], t["max"])),
                        None => errs.push(format!("{k}: not reported by firmware")),
                    }
                }
                if !errs.is_empty() { return json!({"ok": false, "error": errs.join("; "), "state": "original"}); }
                let mut fields = serde_json::Map::new();
                let mut applied: Vec<(&String, Value)> = Vec::new(); // (key, before) of writes that stuck
                let mut failed = false;
                for (k, n) in &todo {
                    let r = legion_wmi::set_fw_oc(k, *n);
                    let ok = r["ok"] == json!(true);
                    if ok && r["changed"] == json!(true) { applied.push((k, r["before"].clone())); }
                    if !ok { errs.push(r["error"].as_str().unwrap_or(k).to_string()); if r["state"] == json!("requested") || r["state"] == json!("unknown") { applied.push((k, r["before"].clone())); } }
                    fields.insert((*k).clone(), r);
                    if !ok { failed = true; break; }
                }
                if !failed { return json!({"ok": true, "reboot_required": fields.values().any(|r| r["reboot_required"] == json!(true)), "fields": fields}); }
                // Undo what was already written so the set is all-or-nothing; record each undo.
                let mut rolled = serde_json::Map::new();
                for (k, before) in applied.iter().rev() {
                    let Some(b0) = before.as_i64() else { rolled.insert((*k).clone(), json!({"ok": false, "error": "no original value"})); continue };
                    let r = legion_wmi::set_fw_oc(k, b0);
                    if r["ok"] != json!(true) { errs.push(format!("rollback {k}: {}", r["error"].as_str().unwrap_or("failed"))); }
                    rolled.insert((*k).clone(), r);
                }
                for (k, _) in &todo { if !fields.contains_key(*k) { fields.insert((*k).clone(), json!({"ok": true, "skipped": true, "note": "not written (earlier field failed)"})); } }
                let clean = rolled.values().all(|r| r["ok"] == json!(true));
                json!({"ok": false, "error": errs.join("; "), "state": if clean { "original" } else { "unknown" }, "fields": fields, "rollback": rolled, "reboot_required": !clean})
            }
            _ => json!({"ok": false, "error": "'values' object required"}),
        },
        Some("set_gpu_mode") => legion_wmi::set_gpu_mode(o.get("mode").and_then(Value::as_str).unwrap_or(""), force),
        Some("set_igpu_mode") => legion_wmi::set_igpu_mode(o.get("mode").and_then(Value::as_u64).unwrap_or(99), force),
        other => json!({"ok": false, "error": format!("unknown op: {}", other.unwrap_or("None"))}),
    }
}

fn main() { init(); std::process::exit(finish(run())); }
