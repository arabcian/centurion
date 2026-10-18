//! Apply saved profiles to hardware (port of profiles/apply.py).
//!
//! Order (matches LACT): memory lock → memory offset → power limit → curve.

use super::native::{load_profile, profile_path};
use crate::config::Config;
use crate::hal::{gpu, limits, monitoring, snapshot, vfcurve};
use crate::safety::{validate_limits, validate_write, LimitField, Limits, MAX_CLOCK_COMP_MHZ};
use crate::types::Domain;
use log::{error, info, warn};
use std::collections::BTreeMap;

#[derive(Debug, Default)]
pub struct ApplyOutcome {
    /// Curve problems — the profile did not apply.
    pub errors: Vec<String>,
    /// Secondary settings that were rejected or failed — non-fatal.
    pub warnings: Vec<String>,
    /// Curve deltas actually sent (flatten resolved): what a read-back must find.
    pub deltas: BTreeMap<i64, i64>,
}

fn to_u32(v: i64) -> Option<u32> { u32::try_from(v).ok() }
fn to_i32(v: i64) -> Option<i32> { i32::try_from(v).ok() }

pub fn apply_profile(gpu_index: usize, name: &str, cfg: &Config) -> Result<ApplyOutcome, String> {
    let path = profile_path(&cfg.profile_dir, name).ok_or_else(|| format!("invalid profile name: {name:?}"))?;
    let mut p = load_profile(&path)?;
    let (g, gname) = gpu::get_gpu(gpu_index).map_err(|e| e.to_string())?;
    let idx = gpu_index as u32;
    let mut out = ApplyOutcome::default();

    let lim = Limits {
        power_limit_w: p.power_limit_w,
        mem_offset_mhz: p.mem_offset_mhz,
        mem_locked_min_mhz: p.mem_locked_min_mhz,
        mem_locked_max_mhz: p.mem_locked_max_mhz,
        gpu_clock_cap_mhz: p.gpu_clock_cap_mhz,
    };
    for e in validate_limits(idx, &lim) {
        out.warnings.push(format!("Rejected: {}", e.message));
        match e.field {
            LimitField::PowerLimit => p.power_limit_w = None,
            LimitField::MemOffset => p.mem_offset_mhz = None,
            LimitField::MemLocked => { p.mem_locked_min_mhz = None; p.mem_locked_max_mhz = None; }
            LimitField::GpuCap => p.gpu_clock_cap_mhz = None,
        }
    }

    // The profile defines every one of these settings: a field it does not carry goes back to
    // its default. Only the core cap used to be handled that way, so switching from a profile with
    // a memory clock lock / power limit / memory offset to one without kept the previous profile's.
    // (Resetting something that is not set is harmless; errors there are noise.)
    match p.mem_locked_max_mhz.and_then(to_u32) {
        Some(max) => {
            let min = p.mem_locked_min_mhz.and_then(to_u32).unwrap_or(max);
            if let Err(e) = limits::set_mem_locked_clocks(min, max, idx) {
                out.warnings.push(format!("Mem locked clocks: {e}"));
            }
        }
        None => { let _ = limits::reset_mem_locked_clocks(idx); }
    }
    let comp = match p.gpu_clock_comp_mhz.unwrap_or(0) {
        c @ 0..=MAX_CLOCK_COMP_MHZ => c,
        c => { out.warnings.push(format!("Rejected: gpu_clock_comp_mhz {c} is outside 0-{MAX_CLOCK_COMP_MHZ} MHz")); 0 }
    };
    match p.gpu_clock_cap_mhz.map(|c| (c + comp).min(4000)).and_then(to_u32) {
        Some(cap) => if let Err(e) = limits::set_gpu_locked_clocks(0, cap, idx) {
            out.warnings.push(format!("Core clock cap: {e}"));
        },
        None => { let _ = limits::reset_gpu_locked_clocks(idx); }
    }
    match p.mem_offset_mhz.and_then(to_i32) {
        Some(off) => if let Err(e) = limits::set_clock_offsets(None, Some(off), idx) {
            out.warnings.push(format!("Mem offset: {e}"));
        },
        // Written only when one is actually set: no NVML write on GPUs that never had an offset.
        None => if limits::get_clock_offsets(idx).mem_offset_mhz.map_or(false, |cur| cur != 0) {
            if let Err(e) = limits::set_clock_offsets(None, Some(0), idx) {
                out.warnings.push(format!("Mem offset reset: {e}"));
            }
        },
    }
    match p.power_limit_w.and_then(to_u32) {
        Some(w) => if let Err(e) = limits::set_power_limit(w, idx) {
            out.warnings.push(format!("Power limit: {e}"));
        },
        None => {
            let pl = limits::get_power_limit(idx);
            if let (Some(cur), Some(def)) = (pl.power_limit_w, pl.default_power_limit_w) {
                if cur != def {
                    if let Err(e) = limits::set_power_limit(def, idx) {
                        out.warnings.push(format!("Power limit reset: {e}"));
                    }
                }
            }
        }
    }

    if p.curve_deltas.is_empty() && p.flatten().is_none() {
        let (rc, d) = vfcurve::reset_offsets(g, false);
        if rc != 0 { out.errors.push(format!("Curve reset failed ({rc}): {d}")); }
        return Ok(out);
    }
    let mut deltas = match p.deltas() {
        Ok(d) => d,
        Err(e) => { out.errors.push(format!("Curve: malformed curve_deltas in profile {name:?}: {e}")); return Ok(out); }
    };
    // One live read serves the domain filter, the flatten, the snapshot and the write baseline.
    let (state, raw) = match vfcurve::read_curve_with_raw_ct(g, "") {
        Ok(v) => v,
        Err(e) => { out.errors.push(format!("Curve: cannot read the live curve: {e}")); return Ok(out); }
    };
    // The memory clock is set only through NVML (mem_offset_mhz). ClockBoostTable deltas on
    // memory-domain points are a second, separate offset that the driver ADDS to the NVML one —
    // legacy (pre-NVML) profiles and `nvcurve profile save` carry them, so every autoload /
    // gamemode / scene apply overclocked the VRAM twice. Never write them.
    let mem_pts: Vec<i64> = deltas.keys().copied().filter(|&i| usize::try_from(i).ok()
        .and_then(|i| state.points.get(i)).map_or(false, |pt| pt.domain == Domain::Memory)).collect();
    if !mem_pts.is_empty() {
        for i in &mem_pts { deltas.remove(i); }
        out.warnings.push(format!("Ignored memory-domain curve_deltas (points {mem_pts:?}): the memory \
                                   offset is mem_offset_mhz only — re-save this profile to drop them"));
    }
    if let Some((from, mhz)) = p.flatten() {
        deltas.extend(vfcurve::flatten_deltas(&state, from, mhz));
    }
    if deltas.is_empty() {
        let (rc, d) = vfcurve::reset_offsets(g, false);
        if rc != 0 { out.errors.push(format!("Curve reset failed ({rc}): {d}")); }
        return Ok(out);
    }
    let errs = validate_write(&deltas, cfg.max_delta_khz);
    if !errs.is_empty() {
        out.errors.push(format!("Curve: {}", errs.join("; ")));
        return Ok(out);
    }
    // Stock first: every GPU-domain point the profile does not name goes back to 0, so a profile
    // lands on defaults and not on top of the previous profile's offsets.
    for pt in state.points.iter().filter(|pt| pt.domain == Domain::Gpu) { deltas.entry(pt.index as i64).or_insert(0); }
    if cfg.auto_snapshot && snapshot::save(g, &gname, &cfg.snapshot_dir, cfg.max_snapshots, Some(raw.bytes())).is_none() {
        warn!("Auto-snapshot failed");
    }
    let (rc, d) = vfcurve::write_offsets(g, &deltas, false, false, Some(raw.bytes()));
    if rc != 0 { out.errors.push(format!("Curve write failed ({rc}): {d}")); }
    out.deltas = deltas;
    Ok(out)
}

/// Apply with read-back verification, retrying with 1s/2s/4s backoff.
pub fn apply_with_retry(gpu_index: usize, name: &str, cfg: &Config, max_retries: u32) -> bool {
    let Some(path) = profile_path(&cfg.profile_dir, name) else {
        warn!("Auto-load profile name {name:?} is invalid — skipping GPU {gpu_index}");
        return false;
    };
    if let Err(e) = load_profile(&path) {
        warn!("Auto-load profile {name:?} unavailable ({e}) — skipping GPU {gpu_index}");
        return false;
    }

    for attempt in 1..=max_retries {
        // verified against what was written (a flatten is resolved at apply time)
        let (errs, warns, expected) = match apply_profile(gpu_index, name, cfg) {
            Ok(o) => (o.errors, o.warnings, o.deltas),
            Err(e) => (vec![e], vec![], BTreeMap::new()),
        };
        if !warns.is_empty() {
            warn!("Auto-load attempt {attempt}/{max_retries} non-fatal warnings: {}", warns.join("; "));
        }
        if !errs.is_empty() {
            warn!("Auto-load attempt {attempt}/{max_retries} errors: {}", errs.join("; "));
        } else if expected.is_empty() {
            info!("Auto-load profile {name:?} applied on GPU {gpu_index} (attempt {attempt}/{max_retries})");
            return true;
        } else {
            match gpu::get_gpu(gpu_index).map_err(|e| e.to_string()).and_then(|(g, _)| vfcurve::read_clock_offsets(g)) {
                Err(e) => warn!("Auto-load attempt {attempt}/{max_retries}: read-back failed: {e}"),
                Ok(offs) => {
                    let mism: Vec<String> = expected.iter().filter_map(|(&i, &v)| {
                        let got = *offs.get(usize::try_from(i).ok()?)? as i64;
                        (!vfcurve::readback_matches(v, got)).then(|| format!("pt{i}: expected {:+.0}MHz got {:+.0}MHz",
                                                    v as f64 / 1000.0, got as f64 / 1000.0))
                    }).collect();
                    if mism.is_empty() {
                        info!("Auto-load profile {name:?} verified on GPU {gpu_index} (attempt {attempt}/{max_retries})");
                        return true;
                    }
                    warn!("Auto-load attempt {attempt}/{max_retries}: read-back mismatch — {}", mism.join("; "));
                }
            }
        }
        if attempt < max_retries {
            let delay = 1u64 << (attempt - 1);
            info!("Retrying auto-load in {delay}s…");
            std::thread::sleep(std::time::Duration::from_secs(delay));
        }
    }
    warn!("Auto-load profile {name:?} failed after {max_retries} attempts on GPU {gpu_index}");
    false
}

/// Boot-time autoload. Returns the process exit code.
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
    // "idx:N" is what set_default_profile stores when GPU discovery failed at that moment (and what the
    // legacy single-profile setting migrates to). It never equals a stable key, so such an entry used to
    // be skipped at every boot: resolve it by index. A stable-key entry for the same GPU wins.
    let mut plan: BTreeMap<usize, (&str, &str)> = BTreeMap::new();
    for (key, prof) in &cfg.auto_load_profiles {
        if prof.is_empty() { continue; }
        let by_index = key.strip_prefix("idx:").and_then(|n| n.parse::<usize>().ok()).filter(|n| gpus.iter().any(|g| g.index == *n));
        match (key_to_idx.get(key), by_index) {
            (Some(&idx), _) => { plan.insert(idx, (key, prof)); }
            (None, Some(idx)) => { plan.entry(idx).or_insert((key, prof)); }
            (None, None) => warn!("Auto-load: no GPU found with key {key:?} — skipping"),
        }
    }
    let mut failed = 0usize;
    for (&idx, &(key, prof)) in &plan {
        info!("Auto-loading profile {prof:?} on GPU {idx} ({key})");
        if !apply_with_retry(idx, prof, cfg, 3) { failed += 1; }
    }
    monitoring::shutdown_nvml();
    if failed > 0 { error!("Auto-load: {failed} profile(s) failed"); 1 } else { 0 }
}
