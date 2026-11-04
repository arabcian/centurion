//! Higher-level operations shared by the CLI and the root helper.

use crate::atomicio::{ensure_dir, write_json};
use crate::config::{Config, PERSISTENT_CONFIG_FILE};
use crate::hal::{gpu, limits, monitoring, sensors, vfcurve};
use crate::nvml;
use crate::profiles::apply::apply_profile;
use crate::profiles::native::{load_profile, profile_path};
use crate::types::CurveState;
use serde_json::{json, Map, Value};
use std::path::Path;
use std::time::Duration;

const WRITE_LOCK: &str = "/run/nvcurve.lock";

/// Cross-process lock around GPU writes. The root helper (GUI, scenes, centurion-gamemode) and
/// `nvcurve autoload` / CLI writes are separate processes: two of them interleaving a reset with
/// another's apply (or NVML lock/offset changes with a curve write) left a mixed state no profile
/// describes. Held until the returned File is dropped; waits up to 60 s for the other writer.
pub fn gpu_write_lock() -> Result<std::fs::File, String> {
    use std::os::unix::fs::OpenOptionsExt;
    let f = std::fs::OpenOptions::new().read(true).write(true).create(true).mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(WRITE_LOCK)
        .map_err(|e| format!("cannot open {WRITE_LOCK}: {e}"))?;
    for _ in 0..600 {
        if unsafe { libc::flock(std::os::unix::io::AsRawFd::as_raw_fd(&f), libc::LOCK_EX | libc::LOCK_NB) } == 0 {
            return Ok(f);
        }
        let e = std::io::Error::last_os_error();
        if e.kind() != std::io::ErrorKind::WouldBlock {
            return Err(format!("cannot acquire {WRITE_LOCK}: {e}"));
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    Err("another nvcurve GPU write is still running (waited 60 s)".into())
}

/// Try acquiring the same lock without waiting. Optional private-register
/// sensor polls should skip a sample rather than waiting behind a GPU write.
pub fn gpu_try_lock() -> Result<Option<std::fs::File>, String> {
    use std::os::unix::fs::OpenOptionsExt;
    use std::os::fd::AsRawFd;
    let f = std::fs::OpenOptions::new().read(true).write(true).create(true).mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(WRITE_LOCK)
        .map_err(|e| format!("cannot open {WRITE_LOCK}: {e}"))?;
    if unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } == 0 {
        return Ok(Some(f));
    }
    let e = std::io::Error::last_os_error();
    if e.kind() == std::io::ErrorKind::WouldBlock { Ok(None) }
    else { Err(format!("cannot lock {WRITE_LOCK}: {e}")) }
}

/// `{gpu, current_voltage_uV, vf_curve:[{index,freq_kHz,volt_uV,freq_offset_kHz,domain}]}`
/// — the shape the GUI reads from /run/nvcurve-gui/*.json.
pub fn curve_json(gpu_name: &str, state: &CurveState, voltage: Option<u32>) -> Value {
    let pts: Vec<Value> = state.points.iter().filter(|p| p.freq_khz > 0 || p.volt_uv > 0).map(|p| json!({
        "index": p.index, "freq_kHz": p.freq_khz, "volt_uV": p.volt_uv,
        "freq_offset_kHz": p.delta_khz, "domain": p.domain,
        // Offset-free stock clock (V3 base tuple, null without it). freq_kHz − delta is
        // NOT the stock clock where the driver clamps the point; the GUI edits against this.
        "base_kHz": p.base_khz,
    })).collect();
    json!({"gpu": gpu_name, "current_voltage_uV": voltage, "vf_curve": pts, "domain_source": state.domain_source})
}

/// Read the curve, retrying briefly if the driver is still settling after a write.
pub fn read_curve_retry(gpu_index: usize, retries: u32) -> Result<Value, String> {
    let mut last = String::from("unknown error");
    for attempt in 0..=retries {
        let (g, name) = gpu::get_gpu(gpu_index).map_err(|e| e.to_string())?;
        match vfcurve::read_curve(g, &name) {
            Ok(s) => {
                let mut v = curve_json(&name, &s, monitoring::read_voltage(g).ok());
                // The memory offset the GUI shows must come from where it is written
                // (NVML, effective MHz) — the ClockBoostTable's memory-domain points
                // use another unit and are clamped by the driver.
                let offs = limits::get_clock_offsets(gpu_index as u32);
                v["mem_offset_mhz"] = json!(offs.mem_offset_mhz);
                v["gpc_offset_mhz"] = json!(offs.gpc_offset_mhz);
                return Ok(v);
            }
            Err(e) => last = e,
        }
        if attempt < retries { std::thread::sleep(Duration::from_millis(150)); }
    }
    Err(last)
}

/// Shared one-shot profile operation used by GUI, CLI and autoload.
/// The caller holds the cross-process GPU lock. No separate reset, no blind
/// retry, and rollback is delegated to the transaction that knows which
/// NVML changes were attempted. `max_retries` is accepted for old callers but
/// deliberately ignored: reapplying OC after an Xid can wedge the driver.
pub fn apply_profile_verified(gpu_index: usize, name: &str, cfg: &Config, _max_retries: u32)
    -> Result<String, String>
{
    let out = apply_profile(gpu_index, name, cfg)?;
    if !out.errors.is_empty() { return Err(out.errors.join("; ")); }
    let mut msg = format!("Profile '{name}' applied and verified.");
    for warning in out.warnings { msg.push('\n'); msg.push_str(&warning); }
    Ok(msg)
}

/// Hotspot/VRAM temperatures, throttle reasons, PowerMizer and the driver's
/// offset ranges in one object (`nvcurve sensors --json`, helper op).
pub fn sensors_json(gpu_index: usize) -> Value {
    let arch = nvml::ready().ok().and_then(|n| n.architecture(n.handle(gpu_index as u32).ok()?).ok());
    let s = gpu::get_gpu(gpu_index).ok().map(|(g, _)| sensors::read(g, arch)).unwrap_or_default();
    let t = limits::get_throttle(gpu_index as u32);
    let pm = limits::get_power_mizer(gpu_index as u32).ok();
    let r = limits::get_offset_ranges(gpu_index as u32);
    json!({
        "architecture": arch,
        "hotspot_c": s.hotspot_c, "vram_c": s.vram_c,
        "vram_partitions": s.vram_partitions, "channels": s.channels, "sensor_source": s.source,
        "throttle": t.map(|t| json!({"mask": t.mask, "reasons": t.reasons, "descriptions": t.descriptions})),
        "power_mizer": pm,
        "offset_ranges": {"gpc_mhz": r.gpc, "mem_mhz": r.mem},
    })
}

/// One prepared zero-table write, exact read-back and one rollback attempt;
/// no separate stock-curve interlude or stale read-back acceptance.
/// Reset only the requested V/F domain (None = both GPU and memory).
/// Same prepared-write/read-back model as profile apply, without modifying
/// NVML locks. The caller holds gpu_write_lock.
pub fn reset_vf_offsets_verified(gpu_index: usize, domain: Option<crate::types::Domain>) -> Result<String, String> {
    use crate::types::Domain;
    let (g, _) = gpu::get_gpu(gpu_index).map_err(|e| e.to_string())?;
    let (state, raw) = vfcurve::read_curve_with_raw_ct(g, "")?;
    let mask = vfcurve::get_boost_mask(g)?;
    let old = vfcurve::parse_ct_entries(raw.bytes());
    let mut zero = vfcurve::PointDeltas::new();
    for (i, &(d, _)) in old.iter().enumerate() {
        let active = mask[i / 8] & (1u8 << (i % 8)) != 0;
        let old_domain = state.points.iter().find(|p| p.index == i).map(|p| p.domain);
        let wanted = domain.is_none() || domain == old_domain;
        if active && wanted { zero.insert(i as i64, 0); }
        else if d != 0 && !active && domain.is_none() {
            return Err(format!("nonzero offset on masked-out point {i}, refusing partial reset"));
        } else if d != 0 && active && domain == Some(Domain::Gpu) && old_domain.is_none() {
            return Err(format!("cannot classify nonzero CT point {i} for GPU-only reset"));
        }
    }
    if state.points.is_empty() || zero.is_empty() { return Err("no programmable GPU points in requested domain".into()); }
    let before: vfcurve::PointDeltas = zero.keys().map(|&i| (i, old[i as usize].0 as i64)).collect();
    let latest = vfcurve::read_clock_offsets(g)?;
    if latest != old.iter().map(|&(d, _)| d).collect::<Vec<_>>() {
        return Err("ClockBoostTable changed during reset preflight".into());
    }
    let (rc, why) = vfcurve::write_prepared_offsets(g, &zero, raw.bytes());
    if rc != 0 {
        let rollback = vfcurve::restore_offsets_verified(g, &before);
        return Err(format!("Reset CT write failed {rc}: {why}; rollback attempt: {rollback:?}"));
    }
    let mismatch = vfcurve::read_clock_offsets(g).map(|now| vfcurve::verify_readback(&zero, &now));
    if !matches!(&mismatch, Ok(v) if v.is_empty()) {
        let err = mismatch.map(|v| v.join("; ")).unwrap_or_else(|e| e);
        let rollback = vfcurve::restore_offsets_verified(g, &before);
        return Err(format!("Reset read-back failed: {err}; rollback attempt: {rollback:?}"));
    }
    Ok(format!("{} V/F offsets reset and verified", match domain {
        None => "all", Some(Domain::Gpu) => "GPU", Some(Domain::Memory) => "memory" }))
}

/// GUI Reset Curve also releases its core cap; memory NVML offsets/locks are
/// deliberately left untouched (Reset All handles those separately).
pub fn reset_gpu_curve_verified(gpu_index: usize) -> Result<String, String> {
    let summary = reset_vf_offsets_verified(gpu_index, None)?;
    match limits::reset_gpu_locked_clocks(gpu_index as u32) {
        Ok(()) => Ok(format!("{summary}; GPU cap released")),
        Err(e) if e.contains("Not Supported") || e.contains("not found") =>
            Ok(format!("{summary}; core lock API unavailable")),
        Err(e) => Err(format!("{summary}; core cap release failed: {e}")),
    }
}

/// Everything back to stock, in the order LACT's reset uses too: curve
/// offsets (every point, both domains), NVML offsets in every P-state, core
/// and memory clock locks, power limit, PowerMizer → Auto. Each step is
/// attempted even if an earlier one fails; returns (done, errors).
pub fn reset_everything(gpu_index: usize) -> (Vec<String>, Vec<String>) {
    let (mut done, mut errs) = (Vec::new(), Vec::new());
    let idx = gpu_index as u32;
    match reset_gpu_curve_verified(gpu_index) {
        Ok(msg) => done.push(msg),
        Err(e) => { errs.push(format!("V/F reset: {e}")); return (done, errs); }
    }
    match limits::reset_all_clock_offsets(idx) {
        Ok(v) if v.is_empty() => done.push("NVML clock offsets already 0 in every P-state".into()),
        Ok(v) => done.extend(v),
        Err(e) => errs.push(format!("clock offsets: {e}")),
    }
    for (what, r) in [("memory clock lock", limits::reset_mem_locked_clocks(idx))] {
        match r {
            Ok(()) => done.push(format!("{what} released")),
            // "not supported" on a GPU without locks is not a failed reset
            Err(e) if e.contains("Not Supported") || e.contains("not found") => {}
            Err(e) => errs.push(format!("{what}: {e}")),
        }
    }
    let pl = limits::get_power_limit(idx);
    if let (Some(cur), Some(def)) = (pl.power_limit_w, pl.default_power_limit_w) {
        if cur != def {
            match limits::set_power_limit(def, idx) {
                Ok(()) => done.push(format!("power limit {cur} W → {def} W (default)")),
                Err(e) => errs.push(format!("power limit: {e}")),
            }
        }
    }
    if let Ok(pm) = limits::get_power_mizer(idx) {
        if pm.current != "auto" && pm.supported.contains(&"auto") {
            match limits::set_power_mizer(idx, "auto") {
                Ok(()) => done.push(format!("PowerMizer {} → auto", pm.current)),
                Err(e) => errs.push(format!("PowerMizer: {e}")),
            }
        }
    }
    (done, errs)
}

// ── Persistent config (/etc/nvcurve/config.json) as a raw JSON object ──────
// Edited as a raw object so keys this version doesn't know are preserved.

pub fn persistent_load(path: &str) -> Map<String, Value> {
    let p = Path::new(path);
    if !p.exists() { return Map::new(); }
    match std::fs::read_to_string(p).map_err(|e| e.to_string())
        .and_then(|t| serde_json::from_str::<Value>(&t).map_err(|e| e.to_string())) {
        Ok(Value::Object(m)) => m,
        Ok(_) => { eprintln!("nvcurve: warning: {path} is not a JSON object — ignoring"); Map::new() }
        Err(e) => { eprintln!("nvcurve: warning: could not read {path}: {e}"); Map::new() }
    }
}

pub fn persistent_save(path: &str, m: &Map<String, Value>) -> std::io::Result<()> {
    let p = Path::new(path);
    if let Some(dir) = p.parent() { ensure_dir(dir, 0o755)?; }
    write_json(p, &Value::Object(m.clone()), 0o644)
}

/// Stable key of GPU `index`. None if discovery worked but the index doesn't
/// exist; "idx:N" if discovery itself failed (can't validate).
pub fn gpu_stable_key_offline(index: usize) -> Option<String> {
    match gpu::discover_gpus() {
        Ok(v) => v.into_iter().find(|g| g.index == index).map(|g| g.stable_key()),
        Err(_) => Some(format!("idx:{index}")),
    }
}

/// The default profile of GPU `index`: by stable key, else by the index-keyed entry.
pub fn default_profile_for(cfg: &Config, index: usize) -> Option<String> {
    gpu_stable_key_offline(index).and_then(|k| cfg.auto_load_profiles.get(&k).cloned())
        .or_else(|| cfg.auto_load_profiles.get(&format!("idx:{index}")).cloned())
}

fn migrate_legacy(m: &mut Map<String, Value>) {
    if let Some(old) = m.remove("auto_load_profile") {
        let e = m.entry("auto_load_profiles").or_insert_with(|| json!({}));
        if let Value::Object(o) = e { o.entry("idx:0").or_insert(old); }
    }
}

/// Set (Some) or clear (None) GPU `index`'s default profile.
pub fn set_default_profile(index: usize, name: Option<&str>) -> Result<(), String> {
    let mut m = persistent_load(PERSISTENT_CONFIG_FILE);
    migrate_legacy(&mut m);
    let key = gpu_stable_key_offline(index).ok_or_else(|| format!("GPU {index} not found"))?;
    let entry = m.entry("auto_load_profiles").or_insert_with(|| json!({}));
    if !entry.is_object() { *entry = json!({}); }
    let profiles = entry.as_object_mut().unwrap();
    // One entry per GPU: drop the index-keyed one (legacy / stored while discovery failed) when the GPU
    // now has a stable key, or autoload would see two defaults for the same card.
    let by_index = format!("idx:{index}");
    if key != by_index { profiles.remove(&by_index); }
    match name {
        Some(n) => { profiles.insert(key, json!(n)); }
        None => { profiles.remove(&key); }
    }
    if profiles.is_empty() { m.remove("auto_load_profiles"); }
    persistent_save(PERSISTENT_CONFIG_FILE, &m).map_err(|e| format!("cannot write {PERSISTENT_CONFIG_FILE}: {e}"))
}

/// Remove every auto-load entry pointing at `name` (any GPU). Returns true if any.
pub fn clear_default_references(name: &str) -> Result<bool, String> {
    let mut m = persistent_load(PERSISTENT_CONFIG_FILE);
    migrate_legacy(&mut m);
    let Some(Value::Object(profiles)) = m.get_mut("auto_load_profiles") else { return Ok(false) };
    let before = profiles.len();
    profiles.retain(|_, v| v.as_str() != Some(name));
    let changed = profiles.len() != before;
    if profiles.is_empty() { m.remove("auto_load_profiles"); }
    if changed {
        persistent_save(PERSISTENT_CONFIG_FILE, &m).map_err(|e| format!("cannot write {PERSISTENT_CONFIG_FILE}: {e}"))?;
    }
    Ok(changed)
}
