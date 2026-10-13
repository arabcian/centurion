//! Live per-core VID and power for the Ryzen Curve tab, without CoreFreq's
//! kernel module: the same MSRs corefreqk reads on Zen, through the stock
//! `msr` driver (/dev/cpu/N/msr, read-only).
//!
//!   0xC001_0293  HW P-state status   CpuVid = bits 21:14 (per SMT thread)
//!   0xC001_0299  RAPL power unit     energy unit = 1 / 2^ESU J, ESU = bits 12:8
//!   0xC001_029A  core energy status  32-bit free-running counter (per core)
//!   0xC001_029B  package energy      32-bit free-running counter
//!
//! VID → volts follows CoreFreq's per-family formulas (coretypes.h):
//! Zen/Zen2/Zen3 (SVI2) 1.55 − 0.00625·VID; Rembrandt, Zen4, Zen5 0.00625·VID.

use serde_json::{json, Map, Value};
use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::os::unix::fs::{FileExt, OpenOptionsExt};
use std::time::Instant;

pub const MSR_HW_PSTATE_STATUS: u64 = 0xC001_0293;
pub const MSR_RAPL_PWR_UNIT: u64 = 0xC001_0299;
pub const MSR_CORE_ENERGY: u64 = 0xC001_029A;
pub const MSR_PKG_ENERGY: u64 = 0xC001_029B;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum VidKind { Svi2, Linear }

/// Formula for this family/model, None where CoreFreq has none.
pub fn vid_kind(family: u32, model: u32) -> Option<VidKind> {
    match family {
        0x17 => Some(VidKind::Svi2),
        // Zen4 (Genoa/Raphael/Phoenix/Hawk Point/Bergamo/Siena) and Zen3+ Rembrandt: linear; Zen3: SVI2.
        0x19 => Some(match model { 0x10..=0x1F | 0x40..=0x4F | 0x60..=0x7F | 0xA0..=0xAF => VidKind::Linear, _ => VidKind::Svi2 }),
        0x1A => Some(VidKind::Linear),
        _ => None,
    }
}

pub fn vid_to_volts(kind: VidKind, vid: u32) -> f64 {
    match kind {
        VidKind::Svi2 => 1.55 - 0.00625 * vid as f64,
        VidKind::Linear => 0.00625 * vid as f64,
    }
}

fn cpuinfo_num(key: &str) -> Option<u32> {
    crate::cpuinfo_head().lines()
        .find(|l| l.split(':').next().map(str::trim) == Some(key))
        .and_then(|l| l.split(':').nth(1)).and_then(|v| v.trim().parse().ok())
}

fn online_cpus() -> Vec<u32> {
    let s = std::fs::read_to_string("/sys/devices/system/cpu/online").unwrap_or_default();
    let mut out = Vec::new();
    for part in s.trim().split(',').filter(|p| !p.is_empty()) {
        let mut it = part.split('-');
        let lo: u32 = match it.next().and_then(|v| v.parse().ok()) { Some(v) => v, None => continue };
        let hi: u32 = it.next().and_then(|v| v.parse().ok()).unwrap_or(lo);
        if hi >= lo && hi - lo < 4096 { out.extend(lo..=hi); }
    }
    out
}

fn rdmsr(f: &File, addr: u64) -> Option<u64> {
    let mut b = [0u8; 8];
    (f.read_at(&mut b, addr).ok()? == 8).then(|| u64::from_ne_bytes(b))
}

pub struct Sampler {
    kind: Option<VidKind>,
    joules_per_unit: f64,
    files: HashMap<u32, File>,
    prev_core: HashMap<u32, u32>,
    prev_pkg: Option<u32>,
    prev_t: Option<Instant>,
    start: Instant,
}

impl Sampler {
    pub fn open() -> Result<Sampler, String> {
        if !crate::cpuinfo_head().lines().any(|l| l.starts_with("vendor_id") && l.contains("AuthenticAMD")) {
            return Err("not an AMD CPU".into());
        }
        let family = cpuinfo_num("cpu family").unwrap_or(0);
        if family < 0x17 { return Err(format!("CPU family {family:#x} is not Zen")); }
        let kind = vid_kind(family, cpuinfo_num("model").unwrap_or(0));
        if !std::path::Path::new("/dev/cpu/0/msr").exists() { crate::modprobe("msr"); }
        let mut s = Sampler { kind, joules_per_unit: 0.0, files: HashMap::new(), prev_core: HashMap::new(),
                              prev_pkg: None, prev_t: None, start: Instant::now() };
        s.refresh_files();
        let f = s.files.values().next().ok_or("/dev/cpu/*/msr: cannot open (msr module missing?)")?;
        let unit = rdmsr(f, MSR_RAPL_PWR_UNIT).ok_or("RAPL unit MSR not readable")?;
        s.joules_per_unit = 1.0 / (1u64 << ((unit >> 8) & 0x1F)) as f64;
        Ok(s)
    }

    /// CPUs come and go (CCD park, SMT toggle): keep one open fd per online CPU.
    fn refresh_files(&mut self) {
        let online = online_cpus();
        self.files.retain(|c, _| online.contains(c));
        for c in online {
            if self.files.contains_key(&c) { continue; }
            if let Ok(f) = OpenOptions::new().read(true).custom_flags(libc::O_CLOEXEC).open(format!("/dev/cpu/{c}/msr")) {
                self.files.insert(c, f);
            }
        }
        self.prev_core.retain(|c, _| self.files.contains_key(c));
    }

    /// One sample: {"ok", "t_ms", "pkg_w", "cpus": {"N": {"v", "w"}}}. Power needs two
    /// samples, so the first one carries volts only.
    pub fn sample(&mut self) -> Value {
        self.refresh_files();
        let now = Instant::now();
        let dt = self.prev_t.map(|p| now.duration_since(p).as_secs_f64()).filter(|d| *d > 0.05);
        let watts = |prev: Option<u32>, cur: u32, jpu: f64| -> Option<f64> {
            Some(cur.wrapping_sub(prev?) as f64 * jpu / dt?)
        };
        let round = |x: f64, k: f64| (x * k).round() / k;
        let mut cpus = Map::new();
        let mut cpu_ids: Vec<u32> = self.files.keys().copied().collect();
        cpu_ids.sort_unstable();
        for c in &cpu_ids {
            let f = &self.files[c];
            let mut o = Map::new();
            if let (Some(kind), Some(st)) = (self.kind, rdmsr(f, MSR_HW_PSTATE_STATUS)) {
                o.insert("v".into(), json!(round(vid_to_volts(kind, ((st >> 14) & 0xFF) as u32), 10000.0)));
            }
            if let Some(e) = rdmsr(f, MSR_CORE_ENERGY).map(|e| e as u32) {
                if let Some(w) = watts(self.prev_core.get(c).copied(), e, self.joules_per_unit) {
                    o.insert("w".into(), json!(round(w, 1000.0)));
                }
                self.prev_core.insert(*c, e);
            }
            cpus.insert(c.to_string(), Value::Object(o));
        }
        let mut pkg_w = Value::Null;
        if let Some(e) = cpu_ids.first().and_then(|c| rdmsr(&self.files[c], MSR_PKG_ENERGY)).map(|e| e as u32) {
            if let Some(w) = watts(self.prev_pkg, e, self.joules_per_unit) { pkg_w = json!(round(w, 100.0)); }
            self.prev_pkg = Some(e);
        }
        self.prev_t = Some(now);
        json!({"ok": true, "t_ms": now.duration_since(self.start).as_millis() as u64, "pkg_w": pkg_w,
               "volts": self.kind.is_some(), "cpus": cpus})
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn formulas_match_corefreq() {
        assert_eq!(vid_kind(0x1A, 0x44), Some(VidKind::Linear));   // Fire Range / Granite Ridge
        assert_eq!(vid_kind(0x19, 0x61), Some(VidKind::Linear));   // Raphael
        assert_eq!(vid_kind(0x19, 0x21), Some(VidKind::Svi2));     // Vermeer
        assert_eq!(vid_kind(0x15, 0), None);
        assert!((vid_to_volts(VidKind::Linear, 176) - 1.1).abs() < 1e-9);
        assert!((vid_to_volts(VidKind::Svi2, 64) - 1.15).abs() < 1e-9);
    }
}
