//! ClockBoostTable snapshots (port of hal/snapshot.py).

use crate::atomicio::{ensure_dir, write_bytes, write_json};
use crate::hal::vfcurve::{self, get_boost_mask, read_clock_table_raw};
use crate::hal::gpu;
use crate::safety::validate_write;
use crate::nvapi::MAX_DELTA_KHZ;
use crate::nvapi::*;
use crate::timefmt;
use crate::types::SnapshotInfo;
use log::{error, info};
use serde_json::json;
use std::io::Read;
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};

fn offsets(raw: &[u8]) -> Vec<i32> {
    if raw.len() < CT_BASE { return Vec::new(); }
    (0..(raw.len() - CT_BASE) / CT_STRIDE)
        .map(|i| read_i32(raw, CT_BASE + i * CT_STRIDE + CT_DELTA_OFF)).collect()
}

/// Save the current table (or `raw` if the caller already has it).
/// Returns the .bin path.
pub fn save(gpu: Gpu, gpu_name: &str, dir: &str, max_snapshots: usize, raw: Option<&[u8]>)
    -> Option<PathBuf>
{
    let owned;
    let raw = match raw {
        Some(r) => r,
        None => match read_clock_table_raw(gpu) {
            Ok(b) => { owned = b; owned.bytes() }
            Err(e) => { error!("Failed to read ClockBoostTable: {e}"); return None; }
        },
    };
    let dir = Path::new(dir);
    if let Err(e) = ensure_dir(dir, 0o755) {
        error!("Cannot create snapshot dir {}: {e}", dir.display());
        return None;
    }
    let ts = timefmt::stamp();
    let bin = dir.join(format!("clock_boost_table_{ts}.bin"));
    let meta_path = dir.join(format!("clock_boost_table_{ts}.json"));
    if let Err(e) = write_bytes(&bin, raw, 0o644) {
        error!("Snapshot write failed: {e}");
        return None;
    }
    let offs = offsets(raw);
    let nonzero = offs.iter().filter(|&&o| o != 0).count();
    // Store a stable UUID in new snapshots; older snapshots retain the GPU
    // name and must still be checked before restore.
    let gpu_uuid = gpu::enumerate_gpus().ok().and_then(|handles| {
        let idx = handles.iter().position(|&h| h == gpu)? as u32;
        let n = crate::nvml::ready().ok()?;
        n.uuid(n.handle(idx).ok()?).ok()
    });
    let meta = json!({
        "gpu": gpu_name, "gpu_uuid": gpu_uuid, "timestamp": timefmt::iso_now(), "file": bin.to_string_lossy(),
        "size": raw.len(), "offsets_kHz": offs, "nonzero_offsets": nonzero,
    });
    if let Err(e) = write_json(&meta_path, &meta, 0o644) {
        error!("Snapshot metadata write failed: {e}; discarding incomplete snapshot");
        let _ = std::fs::remove_file(&bin);
        return None;
    }
    info!("Snapshot saved: {} ({} bytes, {nonzero} non-zero offsets)", bin.display(), raw.len());
    if max_snapshots > 0 { prune(dir, max_snapshots); }
    Some(bin)
}

fn bins_sorted(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(dir).into_iter().flatten().filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.ends_with(".bin")).collect();
    v.sort();
    v
}

fn prune(dir: &Path, max: usize) {
    let bins = bins_sorted(dir);
    if bins.len() <= max { return; }
    for name in &bins[..bins.len() - max] {
        if name.len() <= 4 { continue; }
        let stem = &name[..name.len() - 4];
        for ext in [".bin", ".json"] { let _ = std::fs::remove_file(dir.join(format!("{stem}{ext}"))); }
    }
}

/// Resolve which snapshot file to restore; a caller-supplied path must
/// resolve (through symlinks) inside `dir`.
pub fn resolve_restore_path(dir: &str, filepath: Option<&str>) -> Result<PathBuf, String> {
    let d = Path::new(dir);
    let real_dir = std::fs::canonicalize(d).map_err(|e| format!("{dir}: {e}"))?;
    let cand = match filepath {
        // Newest *regular file* only: a planted symlink named e.g. "z.bin"
        // would otherwise sort last and be picked implicitly.
        None => {
            let newest = bins_sorted(d).into_iter().rev()
                .find(|n| std::fs::symlink_metadata(d.join(n)).is_ok_and(|m| m.file_type().is_file()))
                .ok_or_else(|| format!("No snapshot .bin files in {dir}"))?;
            d.join(newest)
        }
        Some(fp) if Path::new(fp).is_absolute() => PathBuf::from(fp),
        Some(fp) => d.join(fp),
    };
    let shown = filepath.unwrap_or("(newest)");
    let real = std::fs::canonicalize(&cand).map_err(|_| format!("Snapshot file not found: {shown}"))?;
    // Containment is checked on every path, implicit or caller-supplied.
    if !real.starts_with(&real_dir) {
        return Err(format!("Refusing to restore from outside snapshot_dir: {shown}"));
    }
    Ok(real)
}

/// A snapshot contains driver-private words that must never be replayed as a
/// raw NvAPI request. Restore ONLY checked per-point signed frequency deltas
/// into a freshly acquired live table, preserving driver-owned fields.
pub fn restore(gpu: Gpu, dir: &str, filepath: Option<&str>) -> Result<(), String> {
    let path = resolve_restore_path(dir, filepath)?;
    if !path.is_file() { return Err(format!("Snapshot file not found: {}", path.display())); }
    let mut raw = Vec::with_capacity(CT_SIZE + 1);
    std::fs::OpenOptions::new().read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(&path)
        .and_then(|f| f.take(CT_SIZE as u64 + 1).read_to_end(&mut raw))
        .map_err(|e| format!("{}: {e}", path.display()))?;
    if raw.len() != CT_SIZE {
        return Err(format!("Snapshot size mismatch: expected {CT_SIZE}, got {}", raw.len()));
    }
    let vw = read_u32(&raw, 0);
    let expected = (1u32 << 16) | CT_SIZE as u32;
    if vw != expected { return Err(format!("Version mismatch: 0x{vw:08X} (expected 0x{expected:08X})")); }
    let path_meta = path.with_extension("json");
    let meta: serde_json::Value = serde_json::from_str(&crate::atomicio::read_regular(&path_meta, 64 * 1024)
        .map_err(|e| format!("Snapshot metadata is mandatory: {e}"))?)
        .map_err(|e| format!("Invalid snapshot metadata: {e}"))?;
    let Some(saved_name) = meta["gpu"].as_str().filter(|n| !n.is_empty()) else {
        return Err("Snapshot metadata has no GPU identity; refusing unsafe restore".into());
    };
    let live_name = gpu::gpu_name(gpu);
    if live_name.is_empty() || saved_name != live_name {
        return Err(format!("Snapshot belongs to {saved_name:?}, active GPU is {live_name:?}"));
    }
    if meta["size"].as_u64() != Some(CT_SIZE as u64) {
        return Err("Snapshot metadata size does not match ClockBoostTable".into());
    }
    if let Some(expected_uuid) = meta["gpu_uuid"].as_str() {
        let handles = gpu::enumerate_gpus().map_err(|e| e.to_string())?;
        let idx = handles.iter().position(|&h| h == gpu)
            .ok_or("Cannot locate GPU for snapshot UUID verification")? as u32;
        let n = crate::nvml::ready()?;
        let got = n.uuid(n.handle(idx)?)?;
        if got != expected_uuid { return Err(format!("Snapshot GPU UUID {expected_uuid} != active {got}")); }
    }
    let current_mask = get_boost_mask(gpu)?;
    let saved_mask = &raw[4..36];
    // Only actual programmable points are accepted. A driver update or a
    // different device mask must never write an invented point.
    let mut deltas = vfcurve::PointDeltas::new();
    for (i, (d, _)) in vfcurve::parse_ct_entries(&raw).into_iter().enumerate() {
        let active = current_mask[i / 8] & (1u8 << (i % 8)) != 0;
        let saved_active = saved_mask[i / 8] & (1u8 << (i % 8)) != 0;
        if active != saved_active {
            return Err(format!("Snapshot point-mask mismatch at point {i}"));
        }
        if !active && d != 0 {
            return Err(format!("Snapshot stores a nonzero delta on masked-out point {i}"));
        }
        if active { deltas.insert(i as i64, i64::from(d)); }
    }
    let errors = validate_write(&deltas, MAX_DELTA_KHZ);
    if !errors.is_empty() { return Err(format!("Snapshot offset safety validation: {}", errors.join("; "))); }
    if deltas.is_empty() { return Err("Snapshot has no active V/F points".into()); }
    if let Some(saved) = meta["offsets_kHz"].as_array() {
        if saved.len() != CT_POINTS || saved.iter().enumerate().any(|(i, x)| x.as_i64() !=
            Some(i64::from(read_i32(&raw, CT_BASE + i * CT_STRIDE + CT_DELTA_OFF)))) {
            return Err("Snapshot metadata offset array differs from saved binary".into());
        }
    } else { return Err("Snapshot metadata has no offset array".into()); }

    let live = read_clock_table_raw(gpu)?;
    let old_offsets: vfcurve::PointDeltas = deltas.keys()
        .map(|&i| (i, i64::from(read_i32(live.bytes(), CT_BASE + i as usize * CT_STRIDE + CT_DELTA_OFF)))).collect();
    info!("Restoring validated offsets (not raw driver fields) from {}", path.display());
    let (rc, reason) = vfcurve::write_prepared_offsets(gpu, &deltas, live.bytes());
    if rc != 0 {
        let rollback = vfcurve::restore_offsets_verified(gpu, &old_offsets);
        return Err(format!("Snapshot CT write failed {rc}: {reason}; rollback attempt: {rollback:?}"));
    }
    let mismatch = vfcurve::read_clock_offsets(gpu).map(|got| vfcurve::verify_readback(&deltas, &got));
    if !matches!(&mismatch, Ok(v) if v.is_empty()) {
        let observed = mismatch.map(|v| v.join("; ")).unwrap_or_else(|e| e);
        // Best effort restoring original CT offsets; exact success is not
        // guaranteed if the driver entered an Xid recovery state.
        let rollback = vfcurve::restore_offsets_verified(gpu, &old_offsets);
        return Err(format!("Snapshot read-back failed: {observed}; rollback attempt: {rollback:?}"));
    }
    Ok(())
}

pub fn list_snapshots(dir: &str) -> Vec<SnapshotInfo> {
    let d = Path::new(dir);
    let mut names: Vec<String> = std::fs::read_dir(d).into_iter().flatten().filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.ends_with(".json")).collect();
    names.sort_by(|a, b| b.cmp(a));
    names.into_iter().filter_map(|n| {
        let p = d.join(&n);
        let v: serde_json::Value = serde_json::from_str(&crate::atomicio::read_regular(&p, 64 * 1024).ok()?).ok()?;
        let default_bin = p.with_extension("bin").to_string_lossy().into_owned();
        Some(SnapshotInfo {
            filepath: v["file"].as_str().map(str::to_owned).unwrap_or(default_bin),
            timestamp: v["timestamp"].as_str().unwrap_or("").into(),
            gpu: v["gpu"].as_str().unwrap_or("").into(),
            nonzero_offsets: v["nonzero_offsets"].as_u64().unwrap_or(0),
            size: v["size"].as_u64().unwrap_or(0),
        })
    }).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn restore_path_containment() {
        let base = std::env::temp_dir().join(format!("nvc-snap-{}", std::process::id()));
        let snaps = base.join("snaps");
        std::fs::create_dir_all(&snaps).unwrap();
        std::fs::write(snaps.join("clock_boost_table_1.bin"), b"x").unwrap();
        std::fs::write(snaps.join("clock_boost_table_2.bin"), b"x").unwrap();
        std::fs::write(base.join("evil.bin"), b"x").unwrap();
        std::os::unix::fs::symlink(base.join("evil.bin"), snaps.join("link.bin")).unwrap();
        let d = snaps.to_str().unwrap();
        assert!(resolve_restore_path(d, None).unwrap().ends_with("clock_boost_table_2.bin"));
        assert!(resolve_restore_path(d, Some("clock_boost_table_1.bin")).is_ok());
        assert!(resolve_restore_path(d, Some("../evil.bin")).is_err());
        assert!(resolve_restore_path(d, Some("link.bin")).is_err());
        assert!(resolve_restore_path(d, Some(base.join("evil.bin").to_str().unwrap())).is_err());
        std::fs::remove_dir_all(&base).unwrap();
    }

    #[test]
    fn prune_keeps_newest() {
        let d = std::env::temp_dir().join(format!("nvc-prune-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        for i in 0..5 {
            std::fs::write(d.join(format!("clock_boost_table_{i}.bin")), b"").unwrap();
            std::fs::write(d.join(format!("clock_boost_table_{i}.json")), b"{}").unwrap();
        }
        prune(&d, 10); // under limit: nothing removed (the Python bug)
        assert_eq!(bins_sorted(&d).len(), 5);
        prune(&d, 2);
        assert_eq!(bins_sorted(&d), vec!["clock_boost_table_3.bin", "clock_boost_table_4.bin"]);
        assert!(!d.join("clock_boost_table_0.json").exists());
        std::fs::remove_dir_all(&d).unwrap();
    }
}
