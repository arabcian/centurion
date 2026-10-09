//! Field stability evidence: what the machine went through while each setting was in effect.
//!
//! A benchmark sees what a setting gains, never what it risks: a hang every few days, an NVIDIA
//! card that does not come back from D3cold, a lockup under a rare load. This module keeps the
//! record that can show it, at no cost while the machine runs:
//!
//! * exposure - which Optimizations knobs were changed (and to what) from when to when, per boot.
//!   tune-helper adds a segment after every apply/restore, centurion-calibrate marks its own sessions
//!   (`@calibrate`), centurion-boot-guard opens each boot and closes it at a clean shutdown;
//! * events - at a clean shutdown the boot's classified kernel events (health::scan: Xid, GSP
//!   timeouts, machine checks, uncorrectable AER, lockups, D3cold wake failures); a boot that never
//!   reached its shutdown is an unclean end (crash, hang, forced power-off);
//! * a calibration run that was in flight when the machine died: its configuration becomes an
//!   unsafe set of the signature (never picked together again).
//!
//! `field_risk` turns the record into a stability cost per knob: the event rate while the knob
//! was changed against the rate while it was at its boot value (Gamma-Poisson posteriors). A key
//! is charged only with enough exposure on both sides and a credible excess. Keys that were
//! always changed (a boot preset) cannot be judged and are never charged.
//!
//! /var/lib/centurion/exposure.json (root-owned, world-readable):
//!   {"boots": [{"boot", "start", "segs": [{"t", "cfg": {k: v}}], "end"?, "clean"?, "events"?: {kind: n}}]}

use serde_json::{json, Map, Value};
use std::collections::{BTreeMap, BTreeSet};

pub const EXPOSURE: &str = "/var/lib/centurion/exposure.json";
/// The configuration of the calibration run in progress (removed when the session ends).
pub const INFLIGHT: &str = "/var/lib/centurion/calibrate-inflight.json";
const MAX_BOOTS: usize = 150;
const MAX_SEGS: usize = 200;
/// Minimum exposure (hours) on each side before a key can be charged.
const MIN_HOURS: f64 = 3.0;
/// Minimum weighted events while changed.
const MIN_EVENTS: f64 = 2.0;
/// Posterior probability of a 1.5x higher rate needed for a charge.
const P_CHARGE: f64 = 0.8;

fn now() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) }
fn boot_id() -> String { crate::bootguard::boot_id() }

pub fn load() -> Value {
    crate::read_root_file(EXPOSURE, 2 << 20).and_then(|s| serde_json::from_str::<Value>(&s).ok())
        .filter(|v| v["boots"].is_array()).unwrap_or_else(|| json!({"boots": []}))
}

fn save(v: &Value) -> Result<(), String> {
    crate::secure_dir(crate::defaults::DIR)?;
    crate::write_root_file(EXPOSURE, &serde_json::to_vec(v).map_err(|e| e.to_string())?)
}

/// This boot's entry (created if missing).
fn this_boot(v: &mut Value) -> Option<&mut Value> {
    let id = boot_id();
    if id.is_empty() { return None; }
    let boots = v["boots"].as_array_mut()?;
    if !boots.iter().any(|b| b["boot"] == json!(id)) {
        let up = std::fs::read_to_string("/proc/uptime").ok().and_then(|s| s.split('.').next()?.parse::<u64>().ok()).unwrap_or(0);
        boots.push(json!({"boot": id, "start": now().saturating_sub(up), "segs": []}));
        let drop = boots.len().saturating_sub(MAX_BOOTS);
        boots.drain(..drop);
    }
    boots.iter_mut().find(|b| b["boot"] == json!(id))
}

/// Knobs Optimizations has changed right now (tune-helper's journal: every key it holds an
/// original for), with their live values.
pub fn changed_now() -> BTreeMap<String, String> {
    let st: Value = std::fs::read_to_string("/run/centurion/tune/state.json").ok()
        .and_then(|s| serde_json::from_str(&s).ok()).unwrap_or(Value::Null);
    let mut out = BTreeMap::new();
    for e in st["baseline"].as_array().into_iter().flatten() {
        let Some(k) = e[0].as_str().or_else(|| e["key"].as_str()) else { continue };
        if out.contains_key(k) { continue; }
        let v = crate::tune::find(k).and_then(crate::tune::current).unwrap_or_else(|| "?".into());
        out.insert(k.to_owned(), v);
    }
    out
}

/// Adds a segment to this boot when the configuration changed (root; best effort).
pub fn record(cfg: &BTreeMap<String, String>) {
    let mut v = load();
    let Some(b) = this_boot(&mut v) else { return };
    let cfg_v: Map<String, Value> = cfg.iter().map(|(k, x)| (k.clone(), json!(x))).collect();
    let segs = b["segs"].as_array_mut();
    let Some(segs) = segs else { return };
    if segs.last().map_or(false, |s| s["cfg"] == Value::Object(cfg_v.clone())) { return; }
    segs.push(json!({"t": now(), "cfg": cfg_v}));
    let drop = segs.len().saturating_sub(MAX_SEGS);
    segs.drain(..drop);
    let _ = save(&v);
}

/// tune-helper after an apply/restore: the knobs in effect now.
pub fn record_now() { record(&changed_now()); }

/// centurion-calibrate: its sessions are their own exposure (a crash during one is not the presets').
pub fn record_calibration(on: bool) {
    let mut c = changed_now();
    if on { c.insert("@calibrate".into(), "1".into()); }
    record(&c);
}

/// The calibration run about to be measured (fsynced: it must survive a hard hang).
pub fn inflight_set(phase: &str, cfg: &[(String, String)]) {
    let body = json!({"boot": boot_id(), "t": now(), "phase": phase, "cfg": cfg.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()});
    let _ = crate::secure_dir(crate::defaults::DIR).and_then(|_| crate::write_root_file(INFLIGHT, body.to_string().as_bytes()));
}
pub fn inflight_clear() { let _ = std::fs::remove_file(INFLIGHT); }

/// Event weights: what an event says about stability. Corrected errors and warnings count nothing.
fn event_weight(kind: &str, level: &str) -> f64 {
    match (kind, level) {
        ("unclean", _) => 3.0,
        ("lockup", _) | ("gsp", _) | ("d3cold", _) => 2.0,
        (_, "critical") => 2.0,
        ("amdgpu", "error") => 1.0,
        _ => 0.0,
    }
}

/// Weighted event count of one boot entry (at most 6: one boot full of repeated Xids is one bad boot).
fn boot_events(b: &Value) -> f64 {
    let mut e = 0.0;
    if b["clean"] == false { e += event_weight("unclean", ""); }
    for (k, n) in b["events"].as_object().into_iter().flatten() {
        let (kind, level) = k.split_once(':').unwrap_or((k.as_str(), ""));
        e += event_weight(kind, level) * n.as_f64().unwrap_or(0.0).min(3.0);
    }
    e.min(6.0)
}

/// centurion-boot-guard at a clean shutdown: closes this boot with its classified kernel events.
pub fn on_shutdown() {
    let scan = crate::health::scan(0);
    let mut ev: BTreeMap<String, u64> = BTreeMap::new();
    for e in scan["events"].as_array().into_iter().flatten() {
        let k = format!("{}:{}", e["kind"].as_str().unwrap_or("?"), e["level"].as_str().unwrap_or(""));
        *ev.entry(k).or_default() += 1;
    }
    let mut v = load();
    let Some(b) = this_boot(&mut v) else { return };
    b["end"] = json!(now());
    b["clean"] = json!(true);
    b["events"] = json!(ev);
    let _ = save(&v);
    inflight_clear_if_this_boot();
}

fn inflight_clear_if_this_boot() {
    let f: Option<Value> = crate::read_root_file(INFLIGHT, 64 * 1024).and_then(|s| serde_json::from_str(&s).ok());
    if f.map_or(false, |f| f["boot"] == json!(boot_id())) { inflight_clear(); }
}

/// centurion-boot-guard at the start of a boot: an earlier boot without an end ended uncleanly; a
/// calibration run that was in flight then is the prime suspect - its configuration becomes an
/// unsafe set of the signature. Returns messages for the boot log.
pub fn on_boot() -> Vec<String> {
    let mut msgs = Vec::new();
    let cur = boot_id();
    let mut v = load();
    let mut unclean_prev: Option<String> = None;
    if let Some(boots) = v["boots"].as_array_mut() {
        for b in boots.iter_mut() {
            if b["boot"] == json!(cur) || !b["clean"].is_null() { continue; }
            // Last moment known alive: the last segment (or the boot's start).
            let last = b["segs"].as_array().and_then(|s| s.last()).and_then(|s| s["t"].as_u64()).or(b["start"].as_u64()).unwrap_or(0);
            b["clean"] = json!(false);
            b["end"] = json!(last);
            unclean_prev = b["boot"].as_str().map(str::to_owned);
        }
    }
    if unclean_prev.is_some() { let _ = save(&v); }
    // A run that was being measured when the machine died.
    let inflight: Option<Value> = crate::read_root_file(INFLIGHT, 64 * 1024).and_then(|s| serde_json::from_str(&s).ok());
    if let Some(f) = inflight {
        let prev = f["boot"].as_str().unwrap_or("").to_owned();
        if prev != cur {
            let cfg: Vec<(String, String)> = f["cfg"].as_array().into_iter().flatten()
                .filter_map(|x| Some((x[0].as_str()?.to_owned(), x[1].as_str()?.to_owned()))).filter(|(k, _)| !crate::model::is_ctx(k)).collect();
            let clean_shutdown = crate::bootguard::read()["clean_shutdown"] == json!(prev);
            if !clean_shutdown && !cfg.is_empty() {
                if let Some(mut cal) = crate::calib::Calibration::load() {
                    cal.add_unsafe_set(cfg.clone());
                    let _ = crate::secure_dir(crate::defaults::DIR)
                        .and_then(|_| crate::write_root_file(crate::calib::FILE, &serde_json::to_vec(&cal.to_json()).unwrap_or_default()));
                }
                msgs.push(format!("the previous boot died during a calibration run ({}): that combination is now an unsafe set and is never picked again",
                                  cfg.iter().map(|(k, v)| format!("{k}={v}")).collect::<Vec<_>>().join(" ")));
            }
            inflight_clear();
        }
    }
    msgs
}

// ── analysis ─────────────────────────────────────────────────────────────────

/// Field stability of one knob: hours and weighted events with the knob changed / at its boot
/// value, and the posterior probability that its event rate is at least 1.5x higher when changed.
#[derive(Clone, Debug, PartialEq)]
pub struct KeyRisk { pub key: String, pub h_on: f64, pub e_on: f64, pub h_off: f64, pub e_off: f64, pub p: f64, pub risk: f64 }

impl KeyRisk {
    pub fn note(&self) -> String {
        format!("Field stability: {} changed: {:.1} weighted event(s) in {:.0} h, at its boot value: {:.1} in {:.0} h (P(rate >= 1.5x) = {:.2}) -> stability cost {:+.2}.",
                self.key, self.e_on, self.h_on, self.e_off, self.h_off, self.p, self.risk)
    }
}

/// Gamma(shape, 1) sample (Marsaglia-Tsang; shape < 1 boosted).
fn gamma(rng: &mut crate::model::Rng, shape: f64) -> f64 {
    if shape < 1.0 { let u = rng.unit().max(1e-12); return gamma(rng, shape + 1.0) * u.powf(1.0 / shape); }
    let d = shape - 1.0 / 3.0;
    let c = 1.0 / (9.0 * d).sqrt();
    loop {
        let (u1, u2) = (rng.unit().max(1e-12), rng.unit());
        let x = (-2.0 * u1.ln()).sqrt() * (2.0 * std::f64::consts::PI * u2).cos();
        let v = (1.0 + c * x).powi(3);
        if v <= 0.0 { continue; }
        let u = rng.unit().max(1e-12);
        if u.ln() < 0.5 * x * x + d - d * v + d * v.ln() { return d * v; }
    }
}

/// P(rate_on > ratio * rate_off) under Gamma(a0 + e, b0 + h) posteriors (deterministic sampler).
pub fn p_excess(e_on: f64, h_on: f64, e_off: f64, h_off: f64, prior_rate: f64, ratio: f64) -> f64 {
    let (a0, b0) = (1.0, 1.0 / prior_rate.max(1e-6));
    let mut rng = crate::model::Rng::new(0x57AB);
    let n = 4000;
    let mut hit = 0;
    for _ in 0..n {
        let on = gamma(&mut rng, a0 + e_on) / (b0 + h_on);
        let off = gamma(&mut rng, a0 + e_off) / (b0 + h_off);
        if on > ratio * off { hit += 1; }
    }
    hit as f64 / n as f64
}

/// Per knob: exposure and event rates on both sides, and the stability cost a credible excess
/// earns (0 .. -0.5). `boot_value(key)` = the knob's boot value (a segment holding that value
/// counts as "at its boot value"). Keys starting with '@' are bookkeeping, not knobs.
pub fn field_risk(exposure: &Value, now_s: u64, current_boot: &str, boot_value: &dyn Fn(&str) -> Option<String>) -> Vec<KeyRisk> {
    struct B { h: f64, e: f64, share: BTreeMap<String, f64> }
    let mut boots: Vec<B> = Vec::new();
    for b in exposure["boots"].as_array().into_iter().flatten() {
        let start = b["start"].as_u64().unwrap_or(0);
        let open = b["end"].is_null();
        // An earlier boot nobody closed (centurion-boot-guard not enabled then): known alive until its last segment.
        let last_seen = b["segs"].as_array().and_then(|s| s.last()).and_then(|s| s["t"].as_u64()).unwrap_or(start);
        let end = match b["end"].as_u64() { Some(e) => e, None if b["boot"].as_str() == Some(current_boot) => now_s, None => last_seen }.max(start);
        let h = ((end - start) as f64 / 3600.0).max(if b["clean"] == false { 0.25 } else { 0.0 });
        if h <= 0.0 { continue; }
        let segs: Vec<(u64, BTreeMap<String, String>)> = b["segs"].as_array().into_iter().flatten().filter_map(|s| {
            let cfg = s["cfg"].as_object()?.iter().filter_map(|(k, v)| Some((k.clone(), v.as_str()?.to_owned()))).collect();
            Some((s["t"].as_u64()?, cfg))
        }).collect();
        // Time each key spent changed (away from its boot value) within the boot.
        let mut share: BTreeMap<String, f64> = BTreeMap::new();
        let total = (end - start).max(1) as f64;
        for (i, (t, cfg)) in segs.iter().enumerate() {
            let t0 = (*t).max(start).min(end);
            let t1 = segs.get(i + 1).map_or(end, |s| s.0.max(start).min(end));
            let dt = t1.saturating_sub(t0) as f64 / total;
            for (k, v) in cfg {
                if k.starts_with('@') { *share.entry(k.clone()).or_default() += dt; continue; }
                if boot_value(k).as_deref() == Some(v.as_str()) { continue; }
                *share.entry(k.clone()).or_default() += dt;
            }
        }
        // A boot still running has no events recorded yet: it only adds exposure.
        boots.push(B { h, e: if open { 0.0 } else { boot_events(b) }, share });
    }
    let (eh, ee): (f64, f64) = boots.iter().fold((0.0, 0.0), |a, b| (a.0 + b.h, a.1 + b.e));
    let prior_rate = (ee + 0.5) / (eh + 1.0);
    let keys: BTreeSet<String> = boots.iter().flat_map(|b| b.share.keys().cloned()).filter(|k| !k.starts_with('@')).collect();
    let mut out = Vec::new();
    for k in keys {
        let (mut h_on, mut e_on, mut h_off, mut e_off) = (0.0, 0.0, 0.0, 0.0);
        for b in &boots {
            // Calibration time is neither side: what it changed is measured in its own record.
            let cal = b.share.get("@calibrate").copied().unwrap_or(0.0).min(1.0);
            let s = b.share.get(&k).copied().unwrap_or(0.0).min(1.0 - cal);
            let off = (1.0 - cal - s).max(0.0);
            h_on += b.h * s; e_on += b.e * s;
            h_off += b.h * off; e_off += b.e * off;
        }
        if h_on < MIN_HOURS || h_off < MIN_HOURS { continue; }
        let p = p_excess(e_on, h_on, e_off, h_off, prior_rate, 1.5);
        let risk = if e_on >= MIN_EVENTS && p >= P_CHARGE { -0.5 * ((p - P_CHARGE) / (1.0 - P_CHARGE)).clamp(0.0, 1.0).max(0.2) } else { 0.0 };
        out.push(KeyRisk { key: k, h_on, e_on, h_off, e_off, p, risk });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    fn boot(start: u64, end: Option<u64>, clean: Option<bool>, segs: &[(u64, &[(&str, &str)])], events: Value) -> Value {
        let segs: Vec<Value> = segs.iter().map(|(t, c)| json!({"t": t, "cfg": c.iter().map(|(k, v)| ((*k).to_owned(), json!(v))).collect::<Map<String, Value>>()})).collect();
        let mut b = json!({"boot": format!("b{start}"), "start": start, "segs": segs, "events": events});
        if let Some(e) = end { b["end"] = json!(e); }
        if let Some(c) = clean { b["clean"] = json!(c); }
        b
    }

    #[test]
    fn a_knob_that_crashes_is_charged_and_an_innocent_one_is_not() {
        let h = 3600u64;
        let mut boots = Vec::new();
        // 12 boots of 4 h with "bad" changed: 6 of them end uncleanly; 12 boots of 4 h without it: one unclean.
        for i in 0..24u64 {
            let start = i * 10 * h;
            let bad = i % 2 == 0;
            let cfg: Vec<(&str, &str)> = if bad { vec![("vm.bad", "1"), ("vm.ok", "5")] } else { vec![("vm.ok", "5")] };
            let crash = (bad && i % 4 == 0) || i == 23;
            boots.push(boot(start, Some(start + 4 * h), Some(!crash), &[(start, &cfg)], json!({})));
        }
        let ex = json!({"boots": boots});
        let bv = |k: &str| Some(if k == "vm.bad" { "0".to_owned() } else { "1".to_owned() });
        let r = field_risk(&ex, 300 * h, "now", &bv);
        let bad = r.iter().find(|x| x.key == "vm.bad").unwrap();
        assert!(bad.p > 0.9 && bad.risk < -0.1, "{bad:?}");
        assert!(bad.note().contains("vm.bad"));
        // vm.ok was changed in every boot: no "off" exposure, never judged.
        assert!(r.iter().all(|x| x.key != "vm.ok"));
    }

    #[test]
    fn calibration_time_and_open_boots_and_weights() {
        assert_eq!(boot_events(&json!({"clean": false})), 3.0);
        assert_eq!(boot_events(&json!({"clean": true, "events": {"aer:warn": 50, "mce:warn": 2}})), 0.0);
        assert_eq!(boot_events(&json!({"clean": true, "events": {"xid:critical": 9, "lockup:critical": 1}})), 6.0);
        let h = 3600u64;
        // Changed only during calibration: never charged (no exposure outside it).
        let ex = json!({"boots": [boot(0, Some(10 * h), Some(false), &[(0, &[("@calibrate", "1"), ("vm.x", "9")])], json!({}))]});
        assert!(field_risk(&ex, 20 * h, "now", &|_| Some("1".into())).iter().all(|k| k.h_on == 0.0 || k.key != "vm.x" || k.risk == 0.0));
        // An old boot nobody closed counts until its last segment, not until now.
        let ex = json!({"boots": [boot(0, None, None, &[(0, &[("vm.y", "2")]), (h, &[("vm.y", "2")])], json!({})), boot(100 * h, None, None, &[(100 * h, &[])], json!({}))]});
        let r = field_risk(&ex, 110 * h, "b360000", &|_| Some("1".into()));
        assert!(r.is_empty(), "1 h changed is too little exposure to judge: {r:?}");
        // A small, uncertain difference is not charged.
        let p = p_excess(1.0, 10.0, 1.0, 10.0, 0.1, 1.5);
        assert!(p < 0.6, "{p}");
        assert!(p_excess(10.0, 10.0, 0.0, 30.0, 0.1, 1.5) > 0.99);
    }
}
