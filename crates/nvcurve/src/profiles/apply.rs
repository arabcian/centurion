//! Transactional profile application. All entry points (GUI, CLI, autoload)
//! call this one path under ops::gpu_write_lock. No stock-reset interlude.
//! Profile validation and two driver reads occur before the first GPU write.
use super::native::{load_profile, profile_path, ProfileData};
use crate::config::Config;
use crate::hal::{flatten_guard, gpu, limits, snapshot, vfcurve};
use crate::nvapi::{Gpu, CT_POINTS};
use crate::safety::{validate_limits, validate_write, Limits, MAX_CLOCK_COMP_MHZ};
use crate::types::{CurveState, Domain};
use log::{error, info, warn};
use std::collections::BTreeMap;

#[derive(Debug, Default)]
pub struct ApplyOutcome {
    pub errors: Vec<String>,
    pub warnings: Vec<String>,
    /// Exactly what the driver must return after the V/F write.
    pub deltas: vfcurve::PointDeltas,
}

fn checked_limits(idx: u32, p: &ProfileData) -> Result<(), String> {
    let limits = Limits { power_limit_w: p.power_limit_w, mem_offset_mhz: p.mem_offset_mhz,
        mem_locked_min_mhz: p.mem_locked_min_mhz, mem_locked_max_mhz: p.mem_locked_max_mhz,
        gpu_clock_cap_mhz: p.gpu_clock_cap_mhz };
    let invalid = validate_limits(idx, &limits);
    if !invalid.is_empty() {
        return Err(invalid.into_iter().map(|e| e.message).collect::<Vec<_>>().join("; "));
    }
    if p.flatten_from.is_some() != p.flatten_mhz.is_some() {
        return Err("flatten_from and flatten_mhz must be provided together".into());
    }
    let comp = p.gpu_clock_comp_mhz.unwrap_or(0);
    if !(0..=MAX_CLOCK_COMP_MHZ).contains(&comp) {
        return Err(format!("gpu_clock_comp_mhz {comp} outside 0..={MAX_CLOCK_COMP_MHZ}"));
    }
    if let Some(cap) = p.gpu_clock_cap_mhz {
        if !(210..=4000).contains(&cap.checked_add(comp).ok_or("GPU clock cap overflow")?) {
            return Err("GPU clock cap plus compensation outside 210–4000 MHz".into());
        }
    } else if comp != 0 {
        return Err("gpu_clock_comp_mhz needs gpu_clock_cap_mhz".into());
    }
    if p.power_limit_w.is_some_and(|v| !(1..=1000).contains(&v)) {
        return Err("power_limit_w must be within 1..=1000 W".into());
    }
    if p.mem_offset_mhz.is_some_and(|v| i32::try_from(v).is_err()) {
        return Err("mem_offset_mhz cannot fit signed i32".into());
    }
    if p.flatten_mhz.is_some_and(|v| !(210..=4000).contains(&v)) {
        return Err("flatten_mhz outside plausible 210–4000 MHz range".into());
    }
    if p.mem_locked_min_mhz.is_some() && p.mem_locked_max_mhz.is_none() {
        return Err("mem_locked_min_mhz requires mem_locked_max_mhz".into());
    }
    Ok(())
}

/// Non-P0 memory offsets from LACT/other tools must not silently survive a
/// profile switch. Do not clear third-party P-state settings without asking.
pub fn nonzero_other_memory_pstates(idx: u32) -> Result<Vec<String>, String> {
    let nv = crate::nvml::ready()?;
    let h = nv.handle(idx)?;
    if !nv.has("nvmlDeviceGetClockOffsets") { return Ok(Vec::new()); }
    let states = nv.supported_pstates(h).map_err(|e| format!(
        "cannot inspect memory P-states before profile apply: {e}"))?;
    let mut stale = Vec::new();
    for s in states.into_iter().filter(|&s| s != 0) {
        match nv.get_clock_offset(h, crate::nvml::CLOCK_MEM, s) {
            Ok(o) if o.offset_mhz != 0 => stale.push(format!("P{s}: {:+} raw MHz", o.offset_mhz)),
            Ok(_) => {},
            // A driver may refuse an unsupported P-state; do not assume that
            // an arbitrary read failure is evidence of a clean GPU state.
            Err(e) => return Err(format!("cannot verify P{s} memory offset: {e}")),
        }
    }
    Ok(stale)
}

/// Build one full *logical* clock-table request from the currently active
/// point mask. This clears stale CT memory deltas and every unmentioned core
/// point, without a separate reset phase. Never send unknown indices.
fn prepare_deltas(p: &ProfileData, state: &CurveState, mask: &[u8; 32],
                  old_offsets: &[i32], max_delta: i64, warnings: &mut Vec<String>)
    -> Result<vfcurve::PointDeltas, String>
{
    let mut requested = p.deltas()?;
    let active = |i: usize| i < CT_POINTS && mask[i / 8] & (1u8 << (i % 8)) != 0;
    for &i in requested.keys() {
        let i = usize::try_from(i).map_err(|_| "negative profile point index")?;
        if !active(i) { return Err(format!("profile point {i} not in GPU mask")); }
        if !state.points.iter().any(|pt| pt.index == i) {
            return Err(format!("profile point {i} is not a populated V/F point"));
        }
    }
    let memory_points: Vec<i64> = requested.keys().copied().filter(|&i| state.points.iter()
        .any(|pt| pt.index as i64 == i && pt.domain == Domain::Memory)).collect();
    for i in &memory_points { requested.remove(i); }
    if !memory_points.is_empty() {
        warnings.push(format!("Ignored legacy memory-domain curve_deltas {memory_points:?}; NVML controls memory offsets"));
    }
    if let Some((from, mhz)) = p.flatten() {
        requested.extend(vfcurve::flatten_deltas(state, from, mhz)?);
    }
    // Core + memory CT points are set in the same write; stale memory deltas
    // must be cleared before applying an NVML mem_offset_mhz value.
    let mut deltas = vfcurve::PointDeltas::new();
    for pt in &state.points {
        if !active(pt.index) {
            if requested.contains_key(&(pt.index as i64)) {
                return Err(format!("point {} is not programmable", pt.index));
            }
            continue;
        }
        if pt.domain == Domain::Memory { deltas.insert(pt.index as i64, 0); }
        else { deltas.insert(pt.index as i64, requested.remove(&(pt.index as i64)).unwrap_or(0)); }
    }
    if !requested.is_empty() { return Err(format!("unknown GPU point(s): {:?}", requested.keys().collect::<Vec<_>>())); }
    // VFP status may omit indices (or stop before all active CT entries).
    // Check the complete driver table, rather than assuming every point up
    // to state.points.len() has been represented by VFP status. Clear any
    // unrepresented active nonzero delta; fail closed for masked-out points.
    for (i, &old) in old_offsets.iter().enumerate() {
        if old == 0 || deltas.contains_key(&(i as i64)) { continue; }
        if !active(i) { return Err(format!("stale offset {old} kHz on masked-out CT point {i}")); }
        deltas.insert(i as i64, 0);
    }
    let invalid = validate_write(&deltas, max_delta);
    if !invalid.is_empty() { return Err(invalid.join("; ")); }
    // A strict floor check: never silently clamp a prepared offset after the
    // safety guard calculated the intended target.
    for (&i, &d) in &deltas {
        if let Some(pt) = state.points.iter().find(|pt| pt.index as i64 == i) {
            if pt.domain == Domain::Gpu && d < vfcurve::floor_delta_khz(pt) {
                return Err(format!("point {i}: delta {d} is below the safe floor {}", vfcurve::floor_delta_khz(pt)));
            }
        }
    }
    Ok(deltas)
}

fn old_deltas(indices: &vfcurve::PointDeltas, old: &[i32]) -> vfcurve::PointDeltas {
    indices.keys().filter_map(|&i| usize::try_from(i).ok().and_then(|idx| old.get(idx))
        .map(|&v| (i, i64::from(v)))).collect()
}

#[derive(Default)]
struct Changed {
    memory_offset: bool,
    power: bool,
    core_lock: bool,
    memory_lock: bool,
}

/// Best-effort rollback, not an ACID guarantee. Clock lock readback is not
/// exposed by NVML, so preexisting lock values cannot be reconstructed.
fn revert_profile(g: Gpu, idx: u32, previous: &vfcurve::PointDeltas,
                  old_mem: Option<i32>, old_power: Option<u32>, changed_locks: &Changed) -> String {
    let mut errors = Vec::new();
    // Restore NVML memory offset BEFORE any old memory-domain CT offsets;
    // reversing this order could transiently double the VRAM overclock.
    if changed_locks.memory_offset {
        if let Some(old) = old_mem {
            if let Err(e) = limits::set_clock_offsets(None, Some(old), idx) {
                errors.push(format!("NVML memory offset restore: {e}"));
            }
        } else { errors.push("NVML previous memory offset unknown".into()); }
    }
    if let Err(e) = vfcurve::restore_offsets_verified(g, previous) {
        errors.push(format!("CT rollback: {e}"));
    }
    if changed_locks.power {
        if let Some(old) = old_power {
            if let Err(e) = limits::set_power_limit(old, idx) { errors.push(format!("power limit restore: {e}")); }
        } else { errors.push("previous power limit unknown".into()); }
    }
    if changed_locks.core_lock {
        if let Err(e) = limits::reset_gpu_locked_clocks(idx) {
            errors.push(format!("could not release new GPU clock lock: {e}"));
        }
        errors.push("previous GPU clock-lock range cannot be restored (NVML does not expose it)".into());
    }
    if changed_locks.memory_lock {
        if let Err(e) = limits::reset_mem_locked_clocks(idx) {
            errors.push(format!("could not release new VRAM clock lock: {e}"));
        }
        errors.push("previous VRAM clock-lock range cannot be restored (NVML does not expose it)".into());
    }
    if errors.is_empty() { "previous offsets restored and verified".into() }
    else { format!("rollback INCOMPLETE: {}", errors.join("; ")) }
}

/// Single attempt. Caller must hold `ops::gpu_write_lock` across the call.
/// Validation failures never write to the GPU. A write/readback failure
/// triggers one best-effort rollback; repeated retries on an Xid GPU are unsafe.
pub fn apply_profile(gpu_index: usize, name: &str, cfg: &Config) -> Result<ApplyOutcome, String> {
    let path = profile_path(&cfg.profile_dir, name).ok_or("invalid profile name")?;
    let p = load_profile(&path)?;
    checked_limits(gpu_index as u32, &p)?;
    let (g, gname) = gpu::get_gpu(gpu_index).map_err(|e| e.to_string())?;
    let idx = gpu_index as u32;
    let mut out = ApplyOutcome::default();
    if !p.gpu_name.is_empty() && p.gpu_name != gname {
        return Err(format!("profile GPU {:?} differs from actual GPU {:?}", p.gpu_name, gname));
    }
    let stale = nonzero_other_memory_pstates(idx)?;
    if !stale.is_empty() {
        return Err(format!("nonzero memory offsets in other P-states ({}); reset them explicitly before applying", stale.join(", ")));
    }
    let (state, raw) = vfcurve::read_curve_with_raw_ct(g, &gname)?;
    let previous_offsets = vfcurve::parse_ct_entries(raw.bytes()).into_iter().map(|(d, _)| d).collect::<Vec<_>>();
    let mask = vfcurve::get_boost_mask(g)?;
    let deltas = prepare_deltas(&p, &state, &mask, &previous_offsets, cfg.max_delta_khz, &mut out.warnings)?;
    if let (Some((_, mhz)), Some(cap)) = (p.flatten(), p.gpu_clock_cap_mhz) {
        let actual_cap = cap + p.gpu_clock_comp_mhz.unwrap_or(0);
        if actual_cap < mhz { return Err(format!("GPU cap {actual_cap} MHz is below flatten target {mhz} MHz")); }
        if actual_cap != mhz { out.warnings.push(format!("NVML cap is {actual_cap} MHz, flatten plateau {mhz} MHz (compensation is intentional if calibrated)")); }
    }
    if state.points.iter().all(|p| p.domain != Domain::Gpu) {
        return Err("no GPU V/F points available".into());
    }
    flatten_guard::validate_curve_shape(&state, &deltas).map_err(|e| format!("V/F safety preflight: {e}"))?;
    // The V3 struct exceeds the 16-bit version field on Linux NvAPI.
    if p.flatten().is_some() && !state.points.iter().any(|p| p.base_khz.is_some()) {
        out.warnings.push("offset-free V3 tuples unavailable; flatten based on live frequency minus old offset (not a stability guarantee)".into());
    }
    let (confirmation, _raw2) = vfcurve::read_curve_with_raw_ct(g, &gname)?;
    if let Some((from, mhz)) = p.flatten() {
        flatten_guard::verify_flatten_preflight(&state, &confirmation, from, mhz, &deltas)
            .map_err(|e| format!("Flatten safety preflight: {e}"))?;
    } else {
        if confirmation.points.len() != state.points.len() || state.points.iter().zip(&confirmation.points)
            .any(|(p, q)| p.index != q.index || p.domain != q.domain || p.volt_uv != q.volt_uv || p.delta_khz != q.delta_khz) {
            return Err("V/F points changed during preflight".into());
        }
    }
    // Another tool may not honor /run/nvcurve.lock. Re-read exact CT deltas
    // immediately before writing; abort without rollback when already stale.
    let latest = vfcurve::read_clock_offsets(g)?;
    if latest != previous_offsets { return Err("CT changed during preflight; refusing stale write".into()); }
    let previous = old_deltas(&deltas, &previous_offsets);
    if previous.len() != deltas.len() { return Err("cannot capture complete CT rollback baseline".into()); }
    let old_mem = limits::get_clock_offsets(idx).mem_offset_mhz;
    let old_power = limits::get_power_limit(idx).power_limit_w;
    // All fallible range checks and reads above happen before any GPU write.
    if old_mem.is_none() {
        return Err("cannot capture previous NVML memory offset; refusing unverified memory reset".into());
    }
    if p.power_limit_w.is_some() && old_power.is_none() {
        return Err("cannot capture previous power limit for rollback".into());
    }
    if cfg.auto_snapshot && snapshot::save(g, &gname, &cfg.snapshot_dir, cfg.max_snapshots, Some(raw.bytes())).is_none() {
        warn!("Auto-snapshot failed");
    }
    let mut changed_locks = Changed::default();
    let (rc, reason) = vfcurve::write_prepared_offsets(g, &deltas, raw.bytes());
    if rc != 0 {
        let rollback = revert_profile(g, idx, &previous, old_mem, old_power, &changed_locks);
        out.errors.push(format!("CT write failed {rc}: {reason}; {rollback}"));
        return Ok(out);
    }
    // CT must be applied/verified before *any* NVML VRAM offset write.
    let mismatch = vfcurve::read_clock_offsets(g).map(|offs| vfcurve::verify_readback(&deltas, &offs));
    let first_error = match mismatch {
        Err(e) => Some(format!("CT read-back unavailable: {e}")),
        Ok(v) if !v.is_empty() => Some(format!("CT read-back mismatch: {}", v.join("; "))),
        _ => None,
    };
    if let Some(e) = first_error {
        let rollback = revert_profile(g, idx, &previous, old_mem, old_power, &changed_locks);
        out.errors.push(format!("{e}; {rollback}"));
        return Ok(out);
    }
    // Do not convert NVML failures into non-fatal warnings: a mixed CT and
    // NVML configuration must never be marked as an applied profile.
    let apply_limits = (|| -> Result<(), String> {
        let requested_mem = i32::try_from(p.mem_offset_mhz.unwrap_or(0))
            .map_err(|_| "mem_offset_mhz out of i32 range")?;
        if old_mem != Some(requested_mem) {
            changed_locks.memory_offset = true; // setter can partially succeed
            limits::set_clock_offsets(None, Some(requested_mem), idx)?;
        }
        // Clock lock state is not queryable; an unsuccessful partial operation
        // can only release locks and must explicitly report that limitation.
        if let Some(max) = p.mem_locked_max_mhz {
            let min = p.mem_locked_min_mhz.unwrap_or(max);
            changed_locks.memory_lock = true;
            limits::set_mem_locked_clocks(min as u32, max as u32, idx)?;
        } else {
            match limits::reset_mem_locked_clocks(idx) {
                Ok(()) => changed_locks.memory_lock = true,
                Err(e) if e.contains("Not Supported") || e.contains("not found") => {},
                Err(e) => return Err(format!("reset VRAM lock: {e}")),
            }
        }
        let cap = p.gpu_clock_cap_mhz.map(|v| (v + p.gpu_clock_comp_mhz.unwrap_or(0)) as u32);
        if let Some(max) = cap {
            changed_locks.core_lock = true;
            limits::set_gpu_locked_clocks(0, max, idx)?;
        } else {
            match limits::reset_gpu_locked_clocks(idx) {
                Ok(()) => changed_locks.core_lock = true,
                Err(e) if e.contains("Not Supported") || e.contains("not found") => {},
                Err(e) => return Err(format!("reset GPU lock: {e}")),
            }
        }
        let power = limits::get_power_limit(idx);
        let wanted = p.power_limit_w.map(|v| v as u32).or(power.default_power_limit_w);
        if let Some(wanted) = wanted {
            if old_power != Some(wanted) {
                changed_locks.power = true;
                limits::set_power_limit(wanted, idx)?;
            }
        }
        Ok(())
    })();
    if let Err(e) = apply_limits {
        let rollback = revert_profile(g, idx, &previous, old_mem, old_power, &changed_locks);
        out.errors.push(format!("NVML apply failure: {e}; {rollback}"));
        return Ok(out);
    }
    // The NVML setters can change how the driver reports a curve. Verify
    // again after the entire transaction, not only between CT and NVML.
    let final_check = vfcurve::read_clock_offsets(g).map(|offs| vfcurve::verify_readback(&deltas, &offs));
    let final_error = match final_check {
        Err(e) => Some(format!("post-NVML CT read-back unavailable: {e}")),
        Ok(v) if !v.is_empty() => Some(format!("post-NVML CT mismatch: {}", v.join("; "))),
        _ => None,
    };
    let final_error = final_error.or_else(|| {
        let wanted = p.mem_offset_mhz.unwrap_or(0) as i32;
        let got = limits::get_clock_offsets(idx).mem_offset_mhz;
        (got != Some(wanted)).then(|| format!("NVML memory offset mismatch: requested {wanted}, read {got:?}"))
    });
    let final_error = final_error.or_else(|| {
        let wanted = p.power_limit_w.map(|w| w as u32);
        let got = limits::get_power_limit(idx).power_limit_w;
        wanted.and_then(|w| (got != Some(w)).then(|| format!("power limit mismatch: requested {w}, read {got:?}")))
    });
    if let Some(e) = final_error {
        let rollback = revert_profile(g, idx, &previous, old_mem, old_power, &changed_locks);
        out.errors.push(format!("Final verification: {e}; {rollback}"));
        return Ok(out);
    }
    if p.gpu_clock_cap_mhz.is_some() || p.mem_locked_max_mhz.is_some() {
        out.warnings.push("NVML lock setters succeeded, but the driver does not expose the prior/current locked range for exact read-back".into());
    }
    out.deltas = deltas;
    Ok(out)
}

/// Intentionally no automatic retries: repeating a failed OC write on an Xid
/// GPU or while GSP recovers can turn a transient error into a fatal one.
/// This wrapper is retained for existing autoload callers.
pub fn apply_with_retry(gpu_index: usize, name: &str, cfg: &Config, _max_retries: u32) -> bool {
    match crate::ops::apply_profile_verified(gpu_index, name, cfg, 1) {
        Ok(msg) => { info!("Autoload {msg}"); true },
        Err(e) => { warn!("Autoload failed on GPU {gpu_index} without retry: {e}"); false },
    }
}

/// Boot-time autoload (caller holds GPU write lock). Returns exit status.
pub fn run_autoload(cfg: &Config) -> i32 {
    if unsafe { libc::geteuid() } != 0 {
        eprintln!("nvcurve autoload: must run as root");
        return 1;
    }
    if cfg.auto_load_profiles.is_empty() {
        info!("No auto-load profiles configured.");
        return 0;
    }
    let gpus = match gpu::discover_gpus() {
        Ok(g) => g,
        Err(e) => { error!("Failed to initialize NvAPI: {e}"); return 1; }
    };
    if gpus.is_empty() { warn!("No GPUs discovered."); }
    let key_to_idx: BTreeMap<String, usize> = gpus.iter().map(|g| (g.stable_key(), g.index)).collect();
    let mut plan: BTreeMap<usize, (&str, &str)> = BTreeMap::new();
    for (key, prof) in &cfg.auto_load_profiles {
        if prof.is_empty() { continue; }
        let by_index = key.strip_prefix("idx:").and_then(|n| n.parse::<usize>().ok())
            .filter(|n| gpus.iter().any(|g| g.index == *n));
        match (key_to_idx.get(key), by_index) {
            (Some(&idx), _) => { plan.insert(idx, (key, prof)); }
            (None, Some(idx)) => { plan.entry(idx).or_insert((key, prof)); }
            (None, None) => warn!("Auto-load: no GPU found with key {key:?} — skipping"),
        }
    }
    let mut failed = 0usize;
    for (&idx, &(key, prof)) in &plan {
        info!("Auto-loading profile {prof:?} on GPU {idx} ({key})");
        if !apply_with_retry(idx, prof, cfg, 1) { failed += 1; }
    }
    crate::hal::monitoring::shutdown_nvml();
    if failed > 0 { error!("Auto-load: {failed} profile(s) failed"); 1 } else { 0 }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::VfPoint;
    fn state() -> CurveState {
        CurveState { points: vec![
            VfPoint { index: 0, freq_khz: 1_000_000, base_khz: None,
                volt_uv: 700_000, delta_khz: 0, domain: Domain::Gpu },
            VfPoint { index: 1, freq_khz: 1_100_000, base_khz: None,
                volt_uv: 710_000, delta_khz: 0, domain: Domain::Gpu },
            VfPoint { index: 2, freq_khz: 2_000_000, base_khz: None,
                volt_uv: 0, delta_khz: 400_000, domain: Domain::Memory },
        ], timestamp: 0.0, gpu_name: String::new(), domain_source: "flags" }
    }
    #[test]
    fn preparation_clears_memory_and_stale_ct_in_one_table() {
        let mut p = ProfileData { name: "test".into(), ..Default::default() };
        p.curve_deltas.insert("0".into(), serde_json::json!(100_000));
        p.curve_deltas.insert("2".into(), serde_json::json!(400_000));
        let mask = [0b0001_0111u8; 32]; // 0,1,2,4 active
        let mut old = vec![0; CT_POINTS]; old[2] = 400_000; old[4] = -200_000;
        let mut warnings = Vec::new();
        let d = prepare_deltas(&p, &state(), &mask, &old, 500_000, &mut warnings).unwrap();
        assert_eq!(d[&0], 100_000);
        assert_eq!(d[&1], 0);
        assert_eq!(d[&2], 0);
        assert_eq!(d[&4], 0);
        assert_eq!(warnings.len(), 1);
    }
    #[test]
    fn invalid_point_fails_before_any_writer() {
        let mut p = ProfileData { name: "test".into(), ..Default::default() };
        p.curve_deltas.insert("254".into(), serde_json::json!(1));
        let mask = [0xFFu8; 32];
        let old = vec![0; CT_POINTS];
        assert!(prepare_deltas(&p, &state(), &mask, &old, 500_000, &mut Vec::new()).is_err());
        p.curve_deltas.clear(); p.curve_deltas.insert("-1".into(), serde_json::json!(1));
        assert!(prepare_deltas(&p, &state(), &mask, &old, 500_000, &mut Vec::new()).is_err());
    }
}
