//! Experimental Legion GPU power knobs.
//!
//! Two write paths, one per knob, decided by whether the firmware exposes a
//! range for it in /sys/class/firmware-attributes:
//!
//!   ranged (min!=max)   → sysfs firmware-attributes current_value
//!     gpu_nv_ac_offset  CPU+GPU total processing power offset   (fw 10..130)
//!     gpu_temp          GPU thermal target                       (fw 75..87)
//!
//!   unranged (min==max==0, kernel refuses sysfs writes) → acpi_call \_SB.GZFD.WMAE
//!     gpu_nv_ctgp       cTGP                     feature 0x02020000 (GPL2)
//!     gpu_nv_ppab       Dynamic Boost ceiling    feature 0x02010000 (GPL1)
//!     gpu_nv_cpu_boost  Dynamic Boost floor      feature 0x020B0000 (G1PL)
//!
//! All confirmed on Cihan's Legion by decompiling the BIOS and by live reads
//! (base TGP 0x50=80, ceiling/floor 0x19=25). WMAE ABI, also confirmed live:
//!   get: \_SB.GZFD.WMAE 0x0 0x11 {id[0],id[1],id[2],id[3]}
//!   set: \_SB.GZFD.WMAE 0x0 0x12 {id..., value(dword LE)}
//! After a set the firmware does Notify(NPCF,0xC0) itself, so nvidia-powerd
//! re-reads with no /dev/mem and no daemon. Effective only in the Custom
//! platform profile (ODV1==3); other profiles let SGPS overwrite them.
//!
//! Reads always come back through the same channel that owns the knob (sysfs
//! for ranged, WMAE-get for unranged), so the tab shows the true value even
//! when the sysfs current_value of an unranged attribute is stale.

use serde_json::{json, Value};
use std::fs::OpenOptions;
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};

const FW_BASE: &str = "/sys/class/firmware-attributes";
const ACPI_CALL: &str = "/proc/acpi/call";

/// cTGP range. The legal ceiling depends on the GPU model: the vBIOS max power
/// limit (nvidia-smi) minus the Dynamic Boost ceiling, e.g. RTX 5080 Laptop
/// 175 W → 150 W. Read live while the dGPU is awake and cached, so the range
/// is still right while it sleeps; CTGP_FALLBACK_MAX only before first probe.
pub const CTGP_MIN: i64 = 5;
pub const CTGP_FALLBACK_MAX: i64 = 150;
const DYN_BOOST_MAX: i64 = 25;
const CTGP_CACHE: &str = "/var/cache/centurion/ctgp_max";

fn ctgp_cap() -> i64 {
    // The GPU's maximum power limit is a property of the card: once probed, the cached value is used.
    // status() / apply used to run nvidia-smi on every call while the dGPU was awake, which reset its
    // idle timer each time (the GPU never reached D3cold with the Firmware tab polling).
    let cached = std::fs::read_to_string(CTGP_CACHE).ok().and_then(|t| t.trim().parse::<i64>().ok())
        .filter(|&c| (CTGP_MIN..=300).contains(&c));
    if let Some(c) = cached { return c; }
    if let Some((_, maxp)) = nvidia_power_limits().filter(|&(_, m)| m > DYN_BOOST_MAX) {
        let cap = (maxp - DYN_BOOST_MAX).max(CTGP_MIN);
        let _ = std::fs::create_dir_all("/var/cache/centurion");
        let _ = std::fs::write(CTGP_CACHE, cap.to_string());
        return cap;
    }
    CTGP_FALLBACK_MAX
}

pub struct Feat {
    pub key: &'static str,
    pub attr: &'static str,   // firmware-attributes name (also read source)
    pub id: u32,              // WMAE feature id (0 = ranged/sysfs only)
    pub label: &'static str,
    pub unit: &'static str,
    pub via_acpi: bool,       // unranged: write through WMAE
    pub lo: i64, pub hi: i64, // GUI range hint; the sysfs range wins when present
}

pub const FEATURES: &[Feat] = &[
    Feat { key: "ctgp",       attr: "gpu_nv_ctgp",      id: 0x0202_0000, label: "cTGP",                  unit: "W",  via_acpi: true,  lo: CTGP_MIN, hi: CTGP_FALLBACK_MAX },
    Feat { key: "boost_up",   attr: "gpu_nv_ppab",      id: 0x0201_0000, label: "Dynamic Boost ceiling", unit: "W",  via_acpi: true,  lo: 1,  hi: 25 },
    Feat { key: "boost_down", attr: "gpu_nv_cpu_boost", id: 0x020B_0000, label: "Dynamic Boost floor",   unit: "W",  via_acpi: true,  lo: 1,  hi: 25 },
    Feat { key: "ac_offset",  attr: "gpu_nv_ac_offset", id: 0,           label: "CPU+GPU total offset",  unit: "W",  via_acpi: false, lo: 10, hi: 130 },
    Feat { key: "gpu_temp",   attr: "gpu_temp",         id: 0,           label: "GPU temp target",       unit: "°C", via_acpi: false, lo: 75, hi: 87 },
    // CPU limits: written through sysfs; the WMAE id (same numbering as the
    // kernel's lenovo-wmi-other: SPPT 1, SPL 2, FPPT 3, TEMP 4 — matched
    // against this BIOS's DSDT) is only a fallback for when the attribute has
    // vanished from sysfs. The GUI enables that fallback only after it has seen
    // the WMAE read-back equal the sysfs value on this machine.
    Feat { key: "spl",        attr: "ppt_pl1_spl",      id: 0x0102_0000, label: "CPU sustained power (PL1)", unit: "W",  via_acpi: false, lo: 5,  hi: 200 },
    Feat { key: "sppt",       attr: "ppt_pl2_sppt",     id: 0x0101_0000, label: "CPU short-term power (PL2)", unit: "W", via_acpi: false, lo: 5,  hi: 250 },
    Feat { key: "fppt",       attr: "ppt_pl3_fppt",     id: 0x0103_0000, label: "CPU peak power (PL3)",      unit: "W",  via_acpi: false, lo: 5,  hi: 250 },
    Feat { key: "cpu_temp",   attr: "cpu_temp",         id: 0x0104_0000, label: "CPU temperature limit",     unit: "°C", via_acpi: false, lo: 60, hi: 105 },
];

pub fn feat(key: &str) -> Option<&'static Feat> { FEATURES.iter().find(|f| f.key == key) }

/// Current value of one firmware knob through its owning channel (sysfs, or WMAE for the
/// unranged GPU knobs). Root. The GPU knobs must only be read while the dGPU is usable
/// (`crate::dgpu_usable`): in iGPU-only mode a WMI GPU call can hang in the kernel.
pub fn read_feature(key: &str) -> Result<i64, String> {
    let f = feat(key).ok_or_else(|| format!("unknown knob '{key}'"))?;
    if use_wmae(f) && !acpi_available() { modprobe_acpi_call(); }
    read_value(f)
}

// ── firmware-attributes (sysfs) ─────────────────────────────────────────────

fn attr_dir(attr: &str) -> Option<PathBuf> {
    let rd = std::fs::read_dir(FW_BASE).ok()?;
    for e in rd.flatten() {
        let d = e.path().join("attributes").join(attr);
        if d.join("current_value").is_file() { return Some(d); }
    }
    None
}

fn read_i64(p: &Path) -> Option<i64> { std::fs::read_to_string(p).ok()?.trim().parse().ok() }

struct SysRange { cur: i64, min: i64, max: i64, def: Option<i64>, step: i64, ranged: bool }

fn sysfs_read(attr: &str) -> Option<SysRange> {
    let d = attr_dir(attr)?;
    let cur = read_i64(&d.join("current_value"))?;
    let min = read_i64(&d.join("min_value")).unwrap_or(0);
    let max = read_i64(&d.join("max_value")).unwrap_or(0);
    let step = read_i64(&d.join("scalar_increment")).unwrap_or(0);
    let def = read_i64(&d.join("default_value"));
    Some(SysRange { cur, min, max, def, step, ranged: !(min == 0 && max == 0 && step == 0) })
}

/// Write a ranged attribute through fwattr-helper's own path — but we are the
/// root helper, so write directly here after re-validating against the live
/// range (never trust the GUI's bounds).
fn sysfs_write(attr: &str, value: i64) -> Result<(), String> {
    let d = attr_dir(attr).ok_or_else(|| format!("{attr}: attribute not present"))?;
    let r = sysfs_read(attr).ok_or_else(|| format!("{attr}: metadata unreadable"))?;
    if value < 1 { return Err(ZERO_REFUSED.into()); }
    if r.ranged {
        if value < r.min || value > r.max {
            return Err(format!("{value} outside firmware range [{}, {}]", r.min, r.max));
        }
        if r.step > 1 && (value - r.min) % r.step != 0 {
            return Err(format!("{value} is not on the firmware's {}-step grid from {}", r.step, r.min));
        }
    } else if let Some(f) = FEATURES.iter().find(|f| f.attr == attr) {
        // No firmware range published: fall back to the feature's own
        // envelope instead of writing an unchecked integer to the EC.
        if value < f.lo || value > f.hi {
            return Err(format!("{value} outside {}..{} {} (firmware publishes no range)", f.lo, f.hi, f.unit));
        }
    } else {
        return Err(format!("{attr}: no range known; refusing"));
    }
    crate::sysfs_write(&d.join("current_value"), value.to_string().as_bytes())
        .map_err(|e| format!("{attr}: {e}"))
}

// ── acpi_call (WMAE) ────────────────────────────────────────────────────────

fn acpi_available() -> bool { Path::new(ACPI_CALL).exists() }

fn modprobe_acpi_call() { crate::modprobe("acpi_call"); }

/// One acpi_call transaction. The module keeps a single global result
/// buffer: two concurrent callers (the GUI's status poll and an apply, or a
/// second tool) can read each other's result. The write→read pair is
/// therefore done under an exclusive flock on the proc file, and the reply
/// is bounded (the module's buffer is small; a runaway read never grows).
pub(crate) fn acpi_raw(expr: &str) -> Result<String, String> {
    let r = acpi_raw_inner(expr);
    if !acpi_is_read(expr) {
        crate::wlog::log("acpi", &format!("{expr} -> {}", match &r { Ok(o) => format!("ok {}", o.chars().take(40).collect::<String>()), Err(e) => format!("err {e}") }));
    }
    r
}

/// A call the caller knows to be a pure read: never logged.
pub(crate) fn acpi_raw_read(expr: &str) -> Result<String, String> { acpi_raw_inner(expr) }

// The Lenovo WMI methods live under one ACPI device (\_SB.GZFD on every machine seen so far), but the
// path was hard-coded here while fan_table found its method through the WMI bus. Same discovery now:
// the method blocks with a known GUID are resolved fully (device path + "WM" + object_id); WMAC / WQA6
// use the device those blocks sit under. Anything that cannot be found falls back to the verified path.
const OTHER_METHOD_GUID: &str = "DC2A8805-3A8C-41BA-A6F7-092E0089CD3B";  // LENOVO_OTHER_METHOD
const GAMEZONE_GUID: &str = "887B54E3-DDDC-4B2C-8B88-68A26A8835D0";      // LENOVO_GAMEZONE_DATA
const FALLBACK_DEV: &str = "\\_SB.GZFD";

struct WmiPaths { other: String, gamezone: String, dev: String }

fn wmi() -> &'static WmiPaths {
    static P: std::sync::OnceLock<WmiPaths> = std::sync::OnceLock::new();
    P.get_or_init(|| {
        let gz = crate::fan_table::wmi_method_path(GAMEZONE_GUID).ok();
        let other = crate::fan_table::wmi_method_path(OTHER_METHOD_GUID).ok();
        let dev = gz.as_deref().or(other.as_deref()).and_then(|p| p.rsplit_once('.')).map(|(d, _)| d.to_owned())
            .unwrap_or_else(|| FALLBACK_DEV.to_owned());
        WmiPaths { other: other.unwrap_or_else(|| format!("{dev}.WMAE")), gamezone: gz.unwrap_or_else(|| format!("{dev}.WMAA")), dev }
    })
}

/// Pure reads (status polls, WQA6 tune reads, get methods) are not logged.
/// Decided from the parsed call -- `<path> <instance> <method> …` -- not from a text pattern: the fan
/// table read is sent as "0x05" and never matched the old " 0x5" pattern, so it filled writes.log.
fn acpi_is_read(expr: &str) -> bool {
    let mut it = expr.split_whitespace();
    let path = it.next().unwrap_or("");
    let method = it.nth(1).and_then(parse_u64);
    let w = wmi();
    if path.ends_with(".WQA6") { return true; }
    if path == w.other { return method == Some(0x11); }
    if path == w.gamezone {
        return matches!(method, Some(0x15 | 0x17 | 0x18 | 0x1A | 0x28 | 0x29 | 0x2B | 0x2D | 0x2F | 0x31 | 0x32 | 0x37 | 0x38 | 0x3E | 0x3F | 0x40));
    }
    if path.ends_with(".HALS") { return true; }  // VPC2004 status word
    // LENOVO_FAN_METHOD: Fan_Get_FullSpeed, Fan_Get_Table
    path.ends_with(".WMAB") && matches!(method, Some(0x01 | 0x05))
}

fn acpi_raw_inner(expr: &str) -> Result<String, String> {
    use std::io::{Read, Write};
    const MAX_REPLY: u64 = 4096;
    let mut f = OpenOptions::new().read(true).write(true).custom_flags(libc::O_CLOEXEC).open(ACPI_CALL)
        .map_err(|e| format!("{ACPI_CALL}: {e}"))?;
    let _lock = crate::FdLock::exclusive(&f).map_err(|e| format!("{ACPI_CALL} lock: {e}"))?;
    f.write_all(expr.as_bytes()).map_err(|e| format!("acpi_call write: {e}"))?;
    // One large read(): acpi_call clears its result after the first read, and
    // read_to_end's small probe read loses any reply longer than the probe
    // (buffers such as WQA6 came back empty; short integers happened to fit).
    let mut raw = vec![0u8; MAX_REPLY as usize];
    let n = f.read(&mut raw).map_err(|e| format!("acpi_call read: {e}"))?;
    raw.truncate(n);
    let out = String::from_utf8_lossy(&raw);
    let out = out.trim_matches(char::from(0)).trim().to_owned();
    if out.starts_with("Error") { return Err(format!("acpi_call: {out}")); }
    Ok(out)
}

fn parse_u64(s: &str) -> Option<u64> {
    let s = s.trim();
    if let Some(h) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) { u64::from_str_radix(h, 16).ok() } else { s.parse().ok() }
}

fn wmae_get(id: u32) -> Result<i64, String> {
    let b = id.to_le_bytes();
    let expr = format!("{} 0x0 0x11 {{0x{:02x}, 0x{:02x}, 0x{:02x}, 0x{:02x}}}", wmi().other, b[0], b[1], b[2], b[3]);
    parse_u64(&acpi_raw(&expr)?).map(|v| v as i64).ok_or_else(|| "WMAE get: unparseable".into())
}

/// dGPU temperature as the EC measures it (WMAE GPUCurrentTemperature,
/// 0x05050000). An EC field read: it does not touch the NVIDIA driver, so it
/// neither wakes a suspended GPU nor resets its idle timer. None = the EC has
/// no plausible reading (GPU powered off reads 0). Err = no acpi_call / the
/// firmware does not implement the feature.
pub fn ec_gpu_temp() -> Result<Option<i64>, String> {
    if !acpi_available() { modprobe_acpi_call(); }
    if !acpi_available() { return Err("acpi_call is not loaded (modprobe acpi_call)".into()); }
    let t = wmae_get(0x0505_0000)?;
    Ok((1..=125).contains(&t).then_some(t))
}

/// Fan speeds as the capability interface reports them (names as in
/// LenovoLegionToolkit's CapabilityID): 0x0403000{1,2,4} = CPU / GPU / PCH-or-
/// third fan, in RPM. Read only when the firmware's capability list marks the
/// id valid and readable — without a list nothing here is asked for — and only
/// on systems where no hwmon driver already shows the fans (newer kernels read
/// the very same ids in lenovo-wmi-other; asking twice would just load the EC).
pub const EC_FANS: &[(&str, u32)] = &[("cpu", 0x0403_0001), ("gpu", 0x0403_0002), ("pch", 0x0403_0004)];

fn hwmon_has_fans() -> bool {
    std::fs::read_dir("/sys/class/hwmon").into_iter().flatten().flatten().any(|e| {
        std::fs::read_dir(e.path()).into_iter().flatten().flatten().any(|f| {
            let n = f.file_name();
            let n = n.to_string_lossy();
            n.starts_with("fan") && n.ends_with("_input")
        })
    })
}

/// The fan readings worth streaming on this machine (decided once per process).
pub fn ec_fans() -> Vec<(&'static str, u32)> {
    if hwmon_has_fans() { return Vec::new(); }
    EC_FANS.iter().copied().filter(|&(_, id)| cap_usable(id, CAP_GET)).collect()
}

/// {"cpu": rpm, …} — keys only for readings that came back plausible.
pub fn ec_fans_read(fans: &[(&'static str, u32)]) -> serde_json::Map<String, Value> {
    let mut m = serde_json::Map::new();
    for &(name, id) in fans {
        if let Ok(rpm @ 0..=20_000) = wmae_get(id) { m.insert(name.into(), json!(rpm)); }
    }
    m
}

/// Charger adequacy as the firmware judges it (GameZone 62 IsACFitForOC and
/// 47 GetPowerChargeMode, both 1 when the adapter can feed the machine's full
/// power budget — LenovoLegionToolkit's "low wattage charger" test).
/// Some(true) = on AC but the firmware considers the charger too weak.
/// None = on battery, or the firmware does not answer both questions.
pub fn charger_weak() -> Option<bool> {
    let online = std::fs::read_dir("/sys/class/power_supply").ok()?.flatten().any(|e| {
        let rd = |f: &str| std::fs::read_to_string(e.path().join(f)).map(|s| s.trim().to_owned()).unwrap_or_default();
        rd("type") == "Mains" && rd("online") == "1"
    });
    if !online || !acpi_available() { return None; }
    // Offered only where the SmartFan interface exists: both methods belong to it.
    if !wmaa(0x2B, 0).map_or(false, |v| v > 0) { return None; }
    let (fit, mode) = (wmaa(0x3E, 0).ok()?, wmaa(0x2F, 0).ok()?);
    if fit > 1 || !(1..=2).contains(&mode) { return None; }  // not the documented answers: say nothing
    Some(!(fit == 1 && mode == 1))
}

fn wmae_set(id: u32, value: i64) -> Result<(), String> {
    let a = id.to_le_bytes();
    let v = (value as u32).to_le_bytes();
    let expr = format!("{} 0x0 0x12 {{0x{:02x},0x{:02x},0x{:02x},0x{:02x},0x{:02x},0x{:02x},0x{:02x},0x{:02x}}}",
                       wmi().other, a[0], a[1], a[2], a[3], v[0], v[1], v[2], v[3]);
    acpi_raw(&expr).map(|_| ())
}

// ── GPU mode (MUX) ──────────────────────────────────────────────────────────
//
// \_SB.GZFD.WMAA = GameZone WMI interface 887B54E3-DDDC-4B2C-8B88-68A26A8835D0.
// From the DSDT of SMCN19WW (and 20WW):
//   0x28 IsSupportGSync  → constant 2
//   0x29 GetGSyncStatus  → EC MSMF bit: 1 = dGPU direct (MUX to NVIDIA), 0 = hybrid
//   0x2A SetGSyncStatus  → only queues SMI 0xCA sub-command 0x26 (1) / 0x25 (0) and
//                          returns 0; MSMF is NOT touched.
// Verified on the 16AFR10H: the SMI request does switch the MUX at the next boot,
// but MSMF keeps describing the *running* mode until then. The firmware exposes
// no readable "pending" value, so the requested mode is remembered here
// (root-owned, tagged with the boot id) and reported as next_boot until the
// reboot happens. Reading MSMF back right after the set — as before — always
// looked like a failure, and the GUI reverted to the running mode.
// WMAE 0x00210000 (EC GMDM) does not select the MUX (tested: GMDM=1 + reboot
// left dGPU mode); it is only reported, raw.

const GZ_GSYNC_SUPPORTED: u8 = 0x28;
const GZ_GSYNC_GET: u8 = 0x29;
const GZ_GSYNC_SET: u8 = 0x2A;
const FEAT_GMDM: u32 = 0x0021_0000;
const PENDING_DIR: &str = "/var/lib/centurion";
const PENDING_FILE: &str = "/var/lib/centurion/gpu-mode-pending.json";

fn wmaa(method: u8, arg: u64) -> Result<u64, String> {
    let out = acpi_raw(&format!("{} 0x0 0x{method:x} 0x{arg:x}", wmi().gamezone))?;
    parse_u64(&out).ok_or_else(|| format!("WMAA 0x{method:x}: unparseable reply '{out}'"))
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Igpu { Amd, Intel }

/// Integrated display controller visible on the PCI bus (hybrid mode running).
/// Legion/LOQ ship with both AMD and Intel CPUs: checking only for an AMD
/// display function reported every Intel machine in hybrid mode as "dGPU
/// (MUX) only" and pointed the iGPU-only guard at the wrong driver.
fn igpu_present() -> Option<Igpu> {
    std::fs::read_dir("/sys/bus/pci/devices").into_iter().flatten().flatten().find_map(|e| {
        let rd = |f: &str| std::fs::read_to_string(e.path().join(f)).ok().map(|s| s.trim().to_owned());
        if !rd("class").map_or(false, |c| c.starts_with("0x03")) { return None; }
        match rd("vendor").as_deref() { Some("0x1002") => Some(Igpu::Amd), Some("0x8086") => Some(Igpu::Intel), _ => None }
    })
}

/// The iGPU this machine uses in hybrid mode. In dGPU (MUX) mode the iGPU is
/// hidden from the bus, so the CPU vendor decides.
fn igpu_kind() -> Igpu {
    igpu_present().unwrap_or(if crate::tune::cpu_vendor() == crate::tune::Vendor::Intel { Igpu::Intel } else { Igpu::Amd })
}

/// The kernel has (or has loaded) the driver for that iGPU.
fn igpu_driver_available(k: Igpu) -> bool {
    match k {
        Igpu::Amd => crate::tune::kmod_available("amdgpu", "drivers/gpu/drm/amd/amdgpu"),
        Igpu::Intel => crate::tune::kmod_available("i915", "drivers/gpu/drm/i915")
            || crate::tune::kmod_available("xe", "drivers/gpu/drm/xe"),
    }
}

fn igpu_driver_name(k: Igpu) -> &'static str {
    match k { Igpu::Amd => "amdgpu (CONFIG_DRM_AMDGPU)", Igpu::Intel => "i915 or xe (CONFIG_DRM_I915 / CONFIG_DRM_XE)" }
}

fn mode_name(dgpu: bool) -> &'static str { if dgpu { "dgpu" } else { "hybrid" } }

/// Running mode: the PCI bus is authoritative (the iGPU is hidden in dGPU
/// mode); MSMF only when the bus cannot tell.
fn active_is_dgpu() -> bool { igpu_present().is_none() }

/// Mode requested for the next boot during *this* boot, if any.
fn pending_request() -> Option<bool> {
    let v: Value = serde_json::from_str(&crate::read_root_file(PENDING_FILE, 4096)?).ok()?;
    if v["boot_id"].as_str()? != crate::bootguard::boot_id() { return None; }  // a reboot happened: done
    match v["mode"].as_str()? { "dgpu" => Some(true), "hybrid" => Some(false), _ => None }
}

pub fn gpu_mode_status() -> Value {
    if !acpi_available() { modprobe_acpi_call(); }
    let active_dgpu = active_is_dgpu();
    if !acpi_available() {
        return json!({"ok": false, "error": "acpi_call is not loaded (modprobe acpi_call)", "active": mode_name(active_dgpu)});
    }
    let supported = wmaa(GZ_GSYNC_SUPPORTED, 0).map(|v| v != 0).unwrap_or(false);
    let next = pending_request().unwrap_or(active_dgpu);
    json!({
        "ok": true,
        "supported": supported,
        "active": mode_name(active_dgpu),
        "next_boot": mode_name(next),
        "reboot_pending": next != active_dgpu,
        "amdgpu_driver": igpu_driver_available(igpu_kind()),  // key name kept for the GUI: "iGPU driver present"
        "msmf": wmaa(GZ_GSYNC_GET, 0).ok(),
        "gmdm_raw": wmae_get(FEAT_GMDM).ok(),
    })
}

/// `mode`: "hybrid" | "dgpu". Hybrid puts the internal panel on the iGPU, so
/// it is refused when the running kernel has no amdgpu driver (black screen
/// at the next boot) unless `force` is set. Takes effect at the next boot.
pub fn set_gpu_mode(mode: &str, force: bool) -> Value {
    let dgpu = match mode { "dgpu" => true, "hybrid" => false, _ => return json!({"ok": false, "error": "mode must be 'hybrid' or 'dgpu'"}) };
    if !acpi_available() { modprobe_acpi_call(); }
    if !acpi_available() { return json!({"ok": false, "error": "acpi_call is not loaded (modprobe acpi_call)"}); }
    if !wmaa(GZ_GSYNC_SUPPORTED, 0).map_or(false, |v| v != 0) {
        return json!({"ok": false, "error": "the firmware does not report GPU mode switching support"});
    }
    let kind = igpu_kind();
    if !dgpu && !force && !igpu_driver_available(kind) {
        return json!({"ok": false, "needs_force": true,
            "error": format!("this kernel has no {} driver: in hybrid mode the internal display is driven by the iGPU and would stay black. Build it first.", igpu_driver_name(kind))});
    }
    match wmaa(GZ_GSYNC_SET, dgpu as u64) {
        Ok(0) => {}
        Ok(v) => return json!({"ok": false, "error": format!("the firmware rejected the request (SetGSyncStatus returned {v})")}),
        Err(e) => return json!({"ok": false, "error": e}),
    }
    // Remember the request: the firmware has no readable pending state.
    let rec = json!({"mode": mode_name(dgpu), "boot_id": crate::bootguard::boot_id()});
    let saved = crate::secure_dir(PENDING_DIR)
        .and_then(|_| crate::write_root_file(PENDING_FILE, rec.to_string().as_bytes()));
    let active_dgpu = active_is_dgpu();
    let mut out = json!({"ok": true, "active": mode_name(active_dgpu), "next_boot": mode_name(dgpu),
                         "reboot_pending": dgpu != active_dgpu});
    if let Err(e) = saved { out["note"] = json!(format!("requested, but the pending state could not be recorded ({e}); the menu may show the running mode until reboot")); }
    out
}

// ── platform profile ────────────────────────────────────────────────────────

pub fn platform_profile() -> Option<String> {
    std::fs::read_to_string("/sys/firmware/acpi/platform_profile").ok().map(|s| s.trim().to_owned())
}
fn in_custom() -> bool { platform_profile().as_deref() == Some("custom") }

/// Runtime-PM state of the NVIDIA dGPU ("active", "suspended", …), if present.
fn dgpu_runtime_status() -> Option<String> {
    for e in std::fs::read_dir("/sys/bus/pci/devices").ok()?.flatten() {
        let d = e.path();
        let rd = |f: &str| std::fs::read_to_string(d.join(f)).ok().map(|s| s.trim().to_owned());
        if rd("vendor").as_deref() == Some("0x10de") && rd("class").map_or(false, |c| c.starts_with("0x03")) {
            return rd("power/runtime_status");
        }
    }
    None
}

/// nvidia-smi Max/Min Power Limit — advisory only, never a hard limit here.
///
/// Opt-in ({"op":"status","envelope":true}) and skipped while the dGPU is
/// runtime-suspended: nvidia-smi powers the GPU up just to answer, and the
/// plain status poll used to do that on every Firmware Attributes load —
/// including at login, with the app still hidden in the tray. Bounded by a
/// timeout because nvidia-smi can hang on a wedged GPU, and this runs as root
/// inside a pkexec call the GUI is waiting on.
fn nvidia_power_limits() -> Option<(i64, i64)> {
    use std::io::Read;
    use std::process::{Command, Stdio};
    const TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);
    if dgpu_runtime_status().as_deref() == Some("suspended") { return None; }
    let exe = ["/usr/bin/nvidia-smi", "/bin/nvidia-smi", "/opt/bin/nvidia-smi"].iter().find(|p| crate::trusted_path(Path::new(p)))?;
    let mut child = Command::new(exe).args(["-q", "-d", "POWER"]).env_clear().env("PATH", "/usr/bin:/bin")
        .stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::null()).spawn().ok()?;
    let mut out = child.stdout.take()?;
    let reader = std::thread::spawn(move || { let mut s = Vec::new(); let _ = (&mut out).take(256 * 1024).read_to_end(&mut s); s });
    let start = std::time::Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(_)) => break,
            Ok(None) if start.elapsed() < TIMEOUT => std::thread::sleep(std::time::Duration::from_millis(20)),
            _ => { let _ = child.kill(); let _ = child.wait(); return None; }
        }
    }
    let text = String::from_utf8_lossy(&reader.join().ok()?).into_owned();
    let grab = |label: &str| text.lines().find(|l| l.contains(label))
        .and_then(|l| l.split(':').nth(1)).and_then(|v| v.trim().split_whitespace().next())
        .and_then(|n| n.parse::<f64>().ok()).map(|f| f as i64);
    Some((grab("Min Power Limit")?, grab("Max Power Limit")?))
}

// ── read one knob (through its owning channel) ──────────────────────────────

/// The value goes through WMAE: always for the unranged GPU knobs, and as a
/// fallback for an attribute that has disappeared from sysfs.
const VERIFIED_FILE: &str = "/var/lib/centurion/wmae-verified.json";

/// Key tying a WMAE verification to this exact machine + BIOS.
fn verify_key() -> String {
    let d = |n: &str| std::fs::read_to_string(format!("/sys/class/dmi/id/{n}")).map(|s| s.trim().to_owned()).unwrap_or_default();
    format!("{}|{}", d("product_name"), d("bios_version"))
}

/// Feature ids whose WMAE read-back matched sysfs on this machine/BIOS.
/// Recorded by the helper itself (root-owned), never taken from the GUI.
fn verified_ids() -> Vec<u32> {
    crate::read_root_file(VERIFIED_FILE, 4096).and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .filter(|v| v["key"].as_str() == Some(verify_key().as_str()))
        .and_then(|v| v["ids"].as_array().map(|a| a.iter().filter_map(|x| x.as_u64().map(|x| x as u32)).collect()))
        .unwrap_or_default()
}

fn record_verified(id: u32) {
    let mut ids = verified_ids();
    if ids.contains(&id) { return; }
    ids.push(id);
    let rec = json!({"key": verify_key(), "ids": ids});
    let _ = crate::secure_dir("/var/lib/centurion")
        .and_then(|_| crate::write_root_file(VERIFIED_FILE, rec.to_string().as_bytes()));
}

fn use_wmae(f: &Feat) -> bool { f.via_acpi || (f.id != 0 && sysfs_read(f.attr).is_none()) }

fn read_value(f: &Feat) -> Result<i64, String> {
    if use_wmae(f) { wmae_get(f.id) } else {
        sysfs_read(f.attr).map(|r| r.cur).ok_or_else(|| format!("{}: not present", f.attr))
    }
}

/// 0 is never written: the firmware takes it as "feature off", the kernel then
/// stops exposing the attribute, and only a power-profile reset from Windows
/// brought it back.
const ZERO_REFUSED: &str = "0 is refused: the firmware treats it as \"feature off\" and the attribute disappears from sysfs";

// ── operations ──────────────────────────────────────────────────────────────

pub fn status(want_envelope: bool) -> Value {
    if !acpi_available() { modprobe_acpi_call(); }
    let mut out = json!({
        "ok": true,
        "acpi": acpi_available(),
        "profile": platform_profile(),
        "custom": in_custom(),
    });
    let mut vals = serde_json::Map::new();
    for f in FEATURES {
        let sys = sysfs_read(f.attr);
        let mut o = json!({"label": f.label, "unit": f.unit, "via_acpi": f.via_acpi, "attr": f.attr,
                           "present": sys.is_some(), "wmae_fallback": f.id != 0 && sys.is_none()});
        // Cross-check for the GUI: WMAE read-back of a sysfs-backed limit.
        if !f.via_acpi && f.id != 0 && sys.is_some() && acpi_available() {
            if let Ok(w) = wmae_get(f.id) {
                o["wmae"] = json!(w);
                if sys.as_ref().map(|r| r.cur) == Some(w) { record_verified(f.id); }
            }
        }
        // Same channel rule as read_value(), reusing the sysfs read above.
        let value = if f.via_acpi || (f.id != 0 && sys.is_none()) { wmae_get(f.id) } else {
            sys.as_ref().map(|r| r.cur).ok_or_else(|| format!("{}: not present", f.attr))
        };
        match value {
            Ok(v) => { o["value"] = json!(v); }
            Err(e) => { o["error"] = json!(e); }
        }
        // Range: firmware's when it exposes one, else the feature's GUI hint.
        if let Some(s) = &sys {
            if s.ranged { o["min"] = json!(s.min); o["max"] = json!(s.max); o["step"] = json!(s.step.max(1)); o["ranged"] = json!(true); }
            if let Some(d) = s.def { o["default"] = json!(d); }
        }
        if o.get("ranged").is_none() {
            let hi = if f.key == "ctgp" { ctgp_cap() } else { f.hi };
            o["min"] = json!(f.lo); o["max"] = json!(hi); o["step"] = json!(1); o["ranged"] = json!(false);
        }
        vals.insert(f.key.into(), o);
    }
    // cTGP recommended envelope from nvidia-smi (advisory, opt-in).
    if let Some((minp, maxp)) = want_envelope.then(nvidia_power_limits).flatten() {
        let ceil = vals.get("boost_up").and_then(|v| v["value"].as_i64()).unwrap_or(25);
        out["envelope"] = json!({"gpu_min_w": minp, "gpu_max_w": maxp, "ctgp_max_w": (maxp - ceil).max(minp), "ctgp_min_w": minp});
    }
    out["values"] = Value::Object(vals);
    if !acpi_available() {
        out["note"] = json!("acpi_call not loaded — the cTGP / boost knobs can't be written (modprobe acpi_call).");
    }
    out
}

/// Apply pre-validated integer values by key.
pub fn apply(values: &serde_json::Map<String, Value>) -> Value {
    let mut results = Vec::new();
    let mut all = true;
    let need_acpi = values.keys().any(|k| feat(k).map(use_wmae).unwrap_or(false));
    if need_acpi && !acpi_available() { modprobe_acpi_call(); }

    // Validate the whole request first: a bad value anywhere means nothing
    // is written, instead of a half-applied set of EC power limits.
    let mut plan: Vec<(&String, &'static Feat, i64)> = Vec::with_capacity(values.len());
    for (key, jv) in values {
        let checked = (|| {
            let f = feat(key).ok_or_else(|| format!("unknown knob '{key}'"))?;
            let v = jv.as_i64().ok_or_else(|| format!("{key}: not an integer"))?;
            if v < 1 { return Err(format!("{}: {ZERO_REFUSED}", f.label)); }
            if !f.via_acpi && use_wmae(f) && !verified_ids().contains(&f.id) {
                return Err(format!("{}: sysfs attribute missing and the WMAE fallback was never verified \
                    against it on this machine/BIOS; refusing", f.label));
            }
            if use_wmae(f) {
                // No firmware range here; a sanity clamp so a typo can't send
                // something absurd to the EC. cTGP: the GPU's own vBIOS maximum
                // (nvidia-smi Max Power Limit, minus the Dynamic Boost headroom it
                // includes) when the driver can report it — a cTGP above what the
                // board is designed to deliver is never a valid target — else
                // the wide sanity ceiling.
                let cap = if f.key == "ctgp" { ctgp_cap() } else { f.hi };
                if v < f.lo || v > cap { return Err(format!("{} must be {}..{cap} {}", f.label, f.lo, f.unit)); }
            } else {
                // Same check sysfs_write() does at write time, done here too so
                // a bad sysfs value rejects the whole request before any WMAE
                // knob (sorted earlier, e.g. ctgp before spl) has been written.
                let r = sysfs_read(f.attr).ok_or_else(|| format!("{}: attribute not present", f.attr))?;
                let (lo, hi) = if r.ranged { (r.min, r.max) } else { (f.lo, f.hi) };
                if v < lo || v > hi { return Err(format!("{}: {v} outside {lo}..{hi} {}", f.label, f.unit)); }
                if r.ranged && r.step > 1 && (v - r.min) % r.step != 0 {
                    return Err(format!("{}: {v} is not on the firmware's {}-step grid from {}", f.label, r.step, r.min));
                }
            }
            Ok((f, v))
        })();
        match checked {
            Ok((f, v)) => plan.push((key, f, v)),
            Err(m) => { all = false; results.push(json!({"what": key, "ok": false, "message": m})); }
        }
    }
    if !all {
        return json!({"ok": false, "results": results, "error": "request rejected; nothing was written"});
    }

    // Keys arrive alphabetically (fppt, spl, sppt): write the CPU limits in firmware-safe order.
    let keys: Vec<(&str, Option<i64>, i64)> = plan.iter().map(|(_, f, v)| (f.attr, read_value(f).ok(), *v)).collect();
    let order = crate::fw_write_order(&keys);
    let plan: Vec<_> = order.into_iter().map(|i| plan[i]).collect();

    for (key, f, v) in plan {
        let r = (|| {
            if use_wmae(f) {
                if !acpi_available() { return Err("acpi_call not available".into()); }
                // Preflight: the id must answer a read on this firmware before it is written.
                wmae_get(f.id).map_err(|e| format!("{}: WMAE id not readable on this firmware ({e}); refusing to write", f.label))?;
                wmae_set(f.id, v)?;
                let back = wmae_get(f.id)?;
                if back != v { return Err(format!("readback {back} ≠ {v}")); }
                Ok(format!("{} = {} {} (WMAE)", f.label, back, f.unit))
            } else {
                sysfs_write(f.attr, v)?;
                let back = read_value(f)?;
                if back != v { return Err(format!("readback {back} ≠ {v}")); }
                Ok(format!("{} = {} {}", f.label, back, f.unit))
            }
        })();
        let failed = r.is_err();
        all &= !failed;
        results.push(match r { Ok(m) => json!({"what": key, "ok": true, "message": m}),
                               Err(m) => json!({"what": key, "ok": false, "message": m}) });
        if failed { break; }  // never keep writing EC limits after one failed
    }
    json!({"ok": all, "results": results, "custom": in_custom(),
           "note": if in_custom() { Value::Null } else { json!("Not in the Custom platform profile — the firmware overwrites these on the next profile change.") }})
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn feature_map() {
        assert_eq!(feat("ctgp").unwrap().id, 0x0202_0000);
        assert_eq!(feat("boost_up").unwrap().id, 0x0201_0000);
        assert_eq!(feat("boost_down").unwrap().id, 0x020B_0000);
        assert!(feat("ctgp").unwrap().via_acpi);
        assert!(!feat("ac_offset").unwrap().via_acpi);
        assert_eq!(feat("ac_offset").unwrap().attr, "gpu_nv_ac_offset");
        assert_eq!(feat("boost_down").unwrap().attr, "gpu_nv_cpu_boost");
        assert_eq!(feat("spl").unwrap().id, 0x0102_0000);
        assert_eq!(feat("sppt").unwrap().attr, "ppt_pl2_sppt");
        assert!(FEATURES.iter().all(|f| f.lo >= 1), "no knob may allow 0");
    }
    #[test]
    fn capability_list() {
        let c = parse_capdata00("{0x01, 0x00, 0x01, 0x03, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}").unwrap();
        assert_eq!(c, Cap { id: 0x0301_0001, flags: 7, default: 0 });
        assert_eq!(parse_capdata00("[0x30000, 0x1, 0x0]"), Some(Cap { id: 0x0003_0000, flags: 1, default: 0 }));
        assert_eq!(parse_capdata00("{0x01, 0x00}"), None);
        assert_eq!(parse_capdata00("0x0"), None);
        let list = [c, Cap { id: 0x0003_0000, flags: CAP_VALID | CAP_GET, default: 0 }, Cap { id: 0x0402_0000, flags: 6, default: 0 }];
        assert!(cap_usable_in(Some(&list), 0x0301_0001, CAP_GET | CAP_SET));
        assert!(cap_usable_in(Some(&list), 0x0003_0000, CAP_GET));
        assert!(!cap_usable_in(Some(&list), 0x0003_0000, CAP_GET | CAP_SET), "listed read-only: never written");
        assert!(!cap_usable_in(Some(&list), 0x0402_0000, CAP_GET), "listed but not valid");
        assert!(!cap_usable_in(Some(&list), 0x001A_0000, 0));
        assert!(!cap_usable_in(None, 0x0301_0001, 0), "no list: nothing new is offered");
    }
    #[test]
    fn switch_tables() {
        // GameZone method numbers: Lenovo MOF WmiMethodId (IsSupport / Get / Set).
        let w = GZ_TOGGLES.iter().find(|t| t.key == "super_key_lock").unwrap();
        assert_eq!((w.sup, w.get, w.set), (21, 23, 22));
        let t = GZ_TOGGLES.iter().find(|t| t.key == "touchpad_lock").unwrap();
        assert_eq!((t.sup, t.get, t.set), (24, 26, 25));
        assert_eq!(WMAE_TOGGLES.iter().find(|t| t.key == "flip_to_start").map(|t| (t.id, t.verified)), Some((0x0003_0000, false)));
        assert_eq!((HALS_USB_SUPPORT, HALS_USB_STATE, HALS_USB_BATTERY), (0x40, 0x80, 0x8000));
        assert_eq!((FANM_GET_FULLSPEED, FANM_SET_FULLSPEED), (1, 2));
        assert_eq!(first_value("0x1"), Some(1));
        assert_eq!(first_value("{0x00, 0x00, 0x00, 0x00}"), Some(0));
        assert_eq!(first_value("garbage"), None);
        assert_eq!(cap_name(0x0101_FF00), Some("CPU short-term power limit"));
        assert_eq!(cap_name(0x0403_0004), Some("PCH fan speed"));
        assert_eq!(power_modes(0b1_0000_0000_0001_0111), vec!["quiet", "balanced", "performance", "extreme", "custom"]);
    }
    #[test]
    fn wmae_id_le() {
        assert_eq!(0x0202_0000u32.to_le_bytes(), [0, 0, 2, 2]);
        assert_eq!(0x020B_0000u32.to_le_bytes(), [0, 0, 0x0b, 2]);
    }
}

// ── GameZone extras (DSDT of SMCN19/20WW, \_SB.GZFD.WMAA) ──────────────────────
//   0x31 IsSupportOD : 1 only if the panel has Over Drive (PANT bit 1) — the firmware
//                      itself says no on panels without it (e.g. OLED), so we never
//                      offer the toggle there
//   0x32 GetODStatus / 0x33 SetODStatus(0|1): panel GPIO 0x4A + EC 0x7F
//   0x3F IsSupportIGPUMode (3 = supported), 0x40 GetIGPUModeStatus (EC REJF),
//   0x41 SetIGPUModeStatus (EC WEJF): 0 default, 1 iGPU only (dGPU cut off), 2 auto

/// Over Drive through the capability interface (WMAE 0x001A0000) — for models
/// whose GameZone block does not implement IsSupportOD. Not on Legion 7 / Pro 7
/// from Gen 10 on: their firmware lists the capability on panels without it.
const FEAT_OVERDRIVE: u32 = 0x001A_0000;

#[derive(Clone, Copy, PartialEq, Eq)]
enum OdBackend { GameZone, Wmae }

fn od_backend() -> Option<OdBackend> {
    if wmaa(0x31, 0).map_or(false, |v| v == 1) { return Some(OdBackend::GameZone); }
    let mi = crate::machine::info();
    let excluded = matches!(mi.series, crate::machine::Series::Legion7 | crate::machine::Series::LegionPro7) && mi.generation >= 10;
    (!excluded && cap_usable(FEAT_OVERDRIVE, CAP_GET | CAP_SET)).then_some(OdBackend::Wmae)
}

fn od_get(b: OdBackend) -> Option<bool> {
    let v = match b { OdBackend::GameZone => wmaa(0x32, 0).ok()?, OdBackend::Wmae => wmae_get(FEAT_OVERDRIVE).ok()? as u64 };
    (v <= 1).then_some(v == 1)
}

pub fn panel_extras() -> Value {
    if !acpi_available() { modprobe_acpi_call(); }
    if !acpi_available() { return json!({"ok": false, "error": "acpi_call is not loaded"}); }
    let odb = od_backend();
    let od = odb.and_then(od_get);
    let ig_sup = wmaa(0x3F, 0).map(|v| v == 3).unwrap_or(false);
    let ig = if ig_sup { wmaa(0x40, 0).ok().filter(|&v| v <= 2) } else { None };
    json!({"ok": true, "od_supported": od.is_some(), "od": od, "igpu_supported": ig_sup, "igpu_mode": ig})
}

pub fn set_panel_od(on: bool) -> Value {
    if !acpi_available() { modprobe_acpi_call(); }
    let Some(b) = od_backend() else {
        return json!({"ok": false, "error": "this panel has no Over Drive (firmware reports unsupported — e.g. OLED)"});
    };
    if od_get(b).is_none() { return json!({"ok": false, "error": "the firmware does not report an Over Drive state"}); }
    let w = match b { OdBackend::GameZone => wmaa(0x33, on as u64).map(|_| ()), OdBackend::Wmae => wmae_set(FEAT_OVERDRIVE, on as i64) };
    if let Err(e) = w { return json!({"ok": false, "error": e}); }
    match od_get(b) {
        Some(v) if v == on => json!({"ok": true, "od": on}),
        Some(v) => json!({"ok": false, "error": format!("read-back {} after setting Over Drive", v as u8)}),
        None => json!({"ok": false, "error": "Over Drive state unreadable after the write"}),
    }
}

pub fn set_igpu_mode(mode: u64, force: bool) -> Value {
    if mode > 2 { return json!({"ok": false, "error": "mode must be 0 (default), 1 (iGPU only) or 2 (auto)"}); }
    // iGPU-only cuts the dGPU off. In dGPU (MUX direct) mode the panel hangs on
    // the dGPU, and without amdgpu nothing can drive it: a black screen either
    // way. Same guard as set_gpu_mode, overridable with force.
    if mode == 1 && !force {
        let Some(kind) = igpu_present() else {
            return json!({"ok": false, "needs_force": true,
                "error": "the machine is running in dGPU (MUX direct) mode: the display is on the NVIDIA GPU, and \"iGPU only\" would cut it off. Switch the GPU mode to hybrid and reboot first."});
        };
        if !igpu_driver_available(kind) {
            return json!({"ok": false, "needs_force": true,
                "error": format!("this kernel has no {} driver: with the dGPU cut off nothing could drive the display.", igpu_driver_name(kind))});
        }
    }
    // Support check first: the driver release below must not happen for a request
    // the firmware is going to refuse anyway.
    if !acpi_available() { modprobe_acpi_call(); }
    match wmaa(0x3F, 0) {
        Ok(3) => {}
        Ok(_) => return json!({"ok": false, "error": "iGPU mode not supported by this firmware"}),
        Err(e) => return json!({"ok": false, "error": e}),
    }
    // Both iGPU-only and Auto (on battery) make the EC eject the dGPU's slot. The
    // NVIDIA driver must be gone by then, or its remove hangs holding the PCI lock.
    if mode != 0 && igpu_present().is_some() {
        if let Err(v) = crate::dgpu::release() { return v; }
    }
    if let Err(e) = wmaa(0x41, mode) { return json!({"ok": false, "error": e}); }
    json!({"ok": true, "igpu_mode": wmaa(0x40, 0).ok()})
}

// ── EC Full Speed ("turbo fan") via WMAE ────────────────────────────────────
//
// WMAE feature 0x04020000 = EC field FNST (byte 0x8B bit 0, right after the
// F9F0..F9FA fan-table bytes). Get 0x11 returns 0/1; set 0x12 writes the bit
// under the firmware's own LFCM mutex. Verified on the 16AFR10H (SMCN19WW):
// 1 → fans 1800/1800/2500 → 5300/5500/7400 RPM within seconds, 0 → back to the
// EC curve. The bit survives reboots (Windows' Full Speed switch is the same
// flag), which is why a kernel without pwm1_enable/legion_laptop could see the
// fans stuck at max and not clear them. Used only when neither sysfs backend
// exists.

const FEAT_FAN_FULLSPEED: u32 = 0x0402_0000;

// Firmware older than the 0x04020000 capability has the same switch as two
// methods of LENOVO_FAN_METHOD (92549549-…, the block the fan table uses):
// WmiMethodId 1 Fan_Get_FullSpeed → boolean, 2 Fan_Set_FullSpeed(boolean).
// LenovoLegionToolkit picks between the two the same way: the capability when
// the firmware lists it, the fan method otherwise.
const FANM_GET_FULLSPEED: u8 = 0x01;
const FANM_SET_FULLSPEED: u8 = 0x02;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FullSpeedBackend { Wmae, FanMethod }

impl FullSpeedBackend {
    pub fn name(self) -> &'static str { match self { Self::Wmae => "wmae", Self::FanMethod => "fanmethod" } }
}

/// First value of an acpi_call reply that is an integer or a short buffer.
fn first_value(out: &str) -> Option<u64> {
    let t = out.trim();
    if t.starts_with('{') { return parse_buf(t).first().map(|&b| b as u64); }
    parse_u64(t)
}

fn fanm_fullspeed_get() -> Result<bool, String> {
    let m = crate::fan_table::fan_method_path()?;
    let out = acpi_raw_read(&format!("{m} 0 0x{FANM_GET_FULLSPEED:02x} 0"))?;
    match first_value(&out) {
        Some(0) => Ok(false),
        Some(1) => Ok(true),
        _ => Err(format!("Fan_Get_FullSpeed returned '{}'; not a full-speed interface", out.chars().take(40).collect::<String>())),
    }
}

/// Which interface answers on this firmware, with the current state.
///   BIOS family verified with WMAE (SMCN): WMAE, unchanged
///   capability list present: 0x04020000 listed → WMAE, not listed → fan method
///   no list: WMAE as before (the verified path), the fan method only when the
///   WMAE getter does not answer 0/1.
fn fullspeed_probe() -> Result<(FullSpeedBackend, bool), String> {
    if !acpi_available() { modprobe_acpi_call(); }
    if !acpi_available() { return Err("acpi_call is not loaded (modprobe acpi_call)".into()); }
    let wmae = || match wmae_get(FEAT_FAN_FULLSPEED)? {
        0 => Ok(false),
        1 => Ok(true),
        v => Err(format!("WMAE full-speed getter returned {v}; not a Legion FNST interface")),
    };
    // Boards on which the WMAE switch was verified live keep it whatever the list says.
    const WMAE_VERIFIED_BIOS: &[&str] = &["SMCN"];  // Legion Pro 7 16AFR10H
    if WMAE_VERIFIED_BIOS.iter().any(|p| crate::machine::info().bios.prefix.eq_ignore_ascii_case(p)) {
        return wmae().map(|on| (FullSpeedBackend::Wmae, on));
    }
    match capabilities() {
        Some(l) if l.iter().any(|c| c.id == FEAT_FAN_FULLSPEED) => wmae().map(|on| (FullSpeedBackend::Wmae, on)),
        Some(_) => fanm_fullspeed_get().map(|on| (FullSpeedBackend::FanMethod, on)),
        None => match wmae() {
            Ok(on) => Ok((FullSpeedBackend::Wmae, on)),
            Err(e) => fanm_fullspeed_get().map(|on| (FullSpeedBackend::FanMethod, on)).map_err(|e2| format!("{e}; {e2}")),
        },
    }
}

/// Ok(on) when the firmware has an EC full-speed switch this helper can read.
pub fn fan_fullspeed_get() -> Result<bool, String> { fullspeed_probe().map(|(_, on)| on) }

pub fn fan_fullspeed_backend() -> Result<(FullSpeedBackend, bool), String> { fullspeed_probe() }

/// Writes the switch and returns the read-back state.
pub fn fan_fullspeed_set(on: bool) -> Result<bool, String> {
    let (backend, _) = fullspeed_probe()?; // capability check before any write
    match backend {
        FullSpeedBackend::Wmae => wmae_set(FEAT_FAN_FULLSPEED, on as i64)?,
        FullSpeedBackend::FanMethod => {
            let m = crate::fan_table::fan_method_path()?;
            // The WMI argument is a buffer holding the boolean (as Windows passes it).
            acpi_raw(&format!("{m} 0 0x{FANM_SET_FULLSPEED:02x} b{:02x}000000", on as u8))?;
        }
    }
    let (_, now) = fullspeed_probe()?;
    if now != on {
        return Err(format!("EC kept full speed {} after the write{}", if now { "on" } else { "off" },
            if backend == FullSpeedBackend::FanMethod { " (on this firmware the switch only works in the Custom profile)" } else { "" }));
    }
    Ok(now)
}

// ── Capability list (LENOVO_CAPABILITY_DATA_00) ─────────────────────────────
//
// WMI data block 362A3AFE-3D96-4665-8530-96DAD5BB300E: one instance per feature
// the firmware implements, {u32 id, u32 flags, u32 default}. flags as the
// kernel's lenovo-wmi-capdata reads them: bit0 valid, bit1 readable, bit2
// writable. It is the firmware's own answer to "does this machine have feature
// X" — GetFeatureValue on an id the firmware does not know commonly returns 0,
// which looks exactly like "supported, off". Every WMAE feature added for
// other models is therefore offered only when it is listed here.

const CAPDATA00_GUID: &str = "362A3AFE-3D96-4665-8530-96DAD5BB300E";
const CAP_CACHE: &str = "/run/centurion/capdata00.json";
pub const CAP_VALID: u32 = 1;
pub const CAP_GET: u32 = 2;
pub const CAP_SET: u32 = 4;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Cap { pub id: u32, pub flags: u32, pub default: u32 }

/// One instance as acpi_call prints it: a 12-byte buffer, or a 3-integer package.
fn parse_capdata00(reply: &str) -> Option<Cap> {
    let r = reply.trim();
    if r.starts_with('[') {
        let v: Vec<u64> = r.trim_matches(|c| c == '[' || c == ']').split(',').filter_map(|t| parse_u64(t.trim())).collect();
        if v.len() != 3 || v.iter().any(|&x| x > u32::MAX as u64) { return None; }
        return Some(Cap { id: v[0] as u32, flags: v[1] as u32, default: v[2] as u32 });
    }
    if !r.starts_with('{') { return None; }
    let b = parse_buf(r);
    if b.len() < 12 { return None; }
    let u = |o: usize| u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
    Some(Cap { id: u(0), flags: u(4), default: u(8) })
}

fn read_capdata00() -> Option<Vec<Cap>> {
    let (path, n) = crate::fan_table::wmi_query_path(CAPDATA00_GUID).ok()?;
    if !acpi_available() { modprobe_acpi_call(); }
    if !acpi_available() || n == 0 || n > 256 { return None; }
    let mut v = Vec::with_capacity(n as usize);
    for i in 0..n {
        // One unreadable instance makes the whole list untrustworthy: a feature
        // could then look "not listed" only because its entry failed to parse.
        v.push(parse_capdata00(&acpi_raw_read(&format!("{path} 0x{i:x}")).ok()?)?);
    }
    // An all-zero table is a firmware stub, not a list.
    if v.iter().all(|c| c.id == 0) { return None; }
    Some(v)
}

/// The firmware's capability list; None when the block is absent or unreadable.
/// Read once per boot (it is static firmware data) and kept in /run.
pub fn capabilities() -> Option<&'static [Cap]> {
    static C: std::sync::OnceLock<Option<Vec<Cap>>> = std::sync::OnceLock::new();
    C.get_or_init(|| {
        let cached = crate::read_root_file(CAP_CACHE, 64 * 1024).and_then(|t| serde_json::from_str::<Value>(&t).ok())
            .and_then(|v| v.as_array().map(|a| a.iter().filter_map(|e| {
                let u = |i: usize| e.get(i).and_then(Value::as_u64).filter(|&x| x <= u32::MAX as u64).map(|x| x as u32);
                Some(Cap { id: u(0)?, flags: u(1)?, default: u(2)? })
            }).collect::<Vec<_>>())).filter(|v| !v.is_empty());
        if cached.is_some() { return cached; }
        let list = read_capdata00()?;
        let j: Vec<Value> = list.iter().map(|c| json!([c.id, c.flags, c.default])).collect();
        let _ = crate::secure_dir("/run/centurion").and_then(|_| crate::write_root_file(CAP_CACHE, Value::Array(j).to_string().as_bytes()));
        Some(list)
    }).as_deref()
}

/// Listed, valid and carrying every bit of `need` (CAP_GET / CAP_SET).
fn cap_usable_in(list: Option<&[Cap]>, id: u32, need: u32) -> bool {
    list.map_or(false, |l| l.iter().any(|c| c.id == id && c.flags & (CAP_VALID | need) == (CAP_VALID | need)))
}
fn cap_usable(id: u32, need: u32) -> bool { cap_usable_in(capabilities(), id, need) }

// ── Firmware on/off switches ────────────────────────────────────────────────
//
// Feature names and ids follow LenovoLegionToolkit (CapabilityID, GameZone
// method names); method numbers are the WmiMethodId values of the Lenovo MOF
// as published in the kernel's Documentation/wmi/devices/lenovo-wmi-*.rst.
// A switch is offered only where the firmware itself reports it.
//
//   WMAE (GetFeatureValue 0x11 / SetFeatureValue 0x12), plain 0/1 EC bits:
//     instant_boot_ac     0x03010001   power on when AC is plugged in
//     instant_boot_usbpd  0x03010002   power on from a USB-PD charger
//     fnq_custom          0x00100000   Custom mode in the Fn+Q cycle
//       (these three were verified live on the 16AFR10H and keep their old
//        rule: offered when the getter answers 0/1)
//     flip_to_start       0x00030000   power on when the lid is opened — only
//       when the capability list carries it; otherwise through the UEFI
//       variable FBSWIF the older firmware keeps it in (see below)
//   GameZone WMAA, each with its own IsSupport… method:
//     super_key_lock   21 IsSupportDisableWinKey / 23 GetWinKeyStatus / 22 SetWinKeyStatus
//     touchpad_lock    24 IsSupportDisableTP     / 26 GetTPStatus     / 25 SetTPStatus
//       (1 = key / touchpad disabled)
//   VPC2004 (the ACPI device behind ideapad_laptop), HALS status / SALS command:
//     usb_charge_battery  HALS bit 15, SALS 0x13 on / 0x12 off — "always-on USB"
//       also while the laptop runs on battery. The kernel exposes bits 6/7
//       (usb_charging); this sub-switch only exists next to it.

struct WmaeToggle { key: &'static str, id: u32, verified: bool }
const WMAE_TOGGLES: &[WmaeToggle] = &[
    WmaeToggle { key: "instant_boot_ac", id: 0x0301_0001, verified: true },
    WmaeToggle { key: "instant_boot_usbpd", id: 0x0301_0002, verified: true },
    WmaeToggle { key: "fnq_custom", id: 0x0010_0000, verified: true },
    WmaeToggle { key: "flip_to_start", id: 0x0003_0000, verified: false },
];

struct GzToggle { key: &'static str, sup: u8, get: u8, set: u8 }
const GZ_TOGGLES: &[GzToggle] = &[
    GzToggle { key: "super_key_lock", sup: 0x15, get: 0x17, set: 0x16 },
    GzToggle { key: "touchpad_lock", sup: 0x18, get: 0x1A, set: 0x19 },
];

const KEY_USB_BATTERY: &str = "usb_charge_battery";
const KEY_FLIP: &str = "flip_to_start";

/// Switches a write showed this firmware does not really have (the request was
/// accepted and nothing changed). Remembered per machine + BIOS so the option
/// is not offered again.
const UNSUPPORTED_FILE: &str = "/var/lib/centurion/toggle-unsupported.json";

fn unsupported_keys() -> Vec<String> {
    crate::read_root_file(UNSUPPORTED_FILE, 4096).and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .filter(|v| v["key"].as_str() == Some(verify_key().as_str()))
        .and_then(|v| v["keys"].as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(str::to_owned)).collect()))
        .unwrap_or_default()
}

fn record_unsupported(key: &str) {
    let mut keys = unsupported_keys();
    if keys.iter().any(|k| k == key) { return; }
    keys.push(key.to_owned());
    let rec = json!({"key": verify_key(), "keys": keys});
    let _ = crate::secure_dir("/var/lib/centurion")
        .and_then(|_| crate::write_root_file(UNSUPPORTED_FILE, rec.to_string().as_bytes()));
}

fn wmae_toggle_offered(t: &WmaeToggle) -> bool { t.verified || cap_usable(t.id, CAP_GET | CAP_SET) }

// VPC2004 --------------------------------------------------------------------

const HALS_USB_SUPPORT: u64 = 1 << 6;
const HALS_USB_STATE: u64 = 1 << 7;
const HALS_USB_BATTERY: u64 = 1 << 15;
const SALS_USB_BATTERY_OFF: u64 = 0x12;
const SALS_USB_BATTERY_ON: u64 = 0x13;

/// ACPI path of the VPC2004 device (\_SB.PCI0.LPC0.EC0.VPC0 on most models).
fn vpc_path() -> Option<String> {
    let mut v: Vec<PathBuf> = std::fs::read_dir("/sys/bus/acpi/devices").ok()?.flatten().map(|e| e.path())
        .filter(|p| p.file_name().map_or(false, |n| n.to_string_lossy().starts_with("VPC2004:"))).collect();
    v.sort();
    v.iter().find_map(|p| crate::fan_table::short_acpi_path(&std::fs::read_to_string(p.join("path")).ok()?))
}

fn hals(vpc: &str) -> Result<u64, String> {
    let out = acpi_raw_read(&format!("{vpc}.HALS"))?;
    parse_u64(&out).ok_or_else(|| format!("HALS: unparseable reply '{out}'"))
}

/// Some(on) when the machine has USB charging (HALS bit 6) — the on-battery
/// sub-switch is only meaningful there.
fn usb_battery_get() -> Option<bool> {
    let h = hals(&vpc_path()?).ok()?;
    (h & HALS_USB_SUPPORT != 0).then_some(h & HALS_USB_BATTERY != 0)
}

fn usb_battery_set(on: bool) -> Result<(), String> {
    let vpc = vpc_path().ok_or("no VPC2004 ACPI device on this machine")?;
    let h = hals(&vpc)?;
    if h & HALS_USB_SUPPORT == 0 { return Err("the firmware reports no USB charging support".into()); }
    if on && h & HALS_USB_STATE == 0 {
        return Err("turn on \"USB charging when off\" first: this switch only extends it to battery power".into());
    }
    acpi_raw(&format!("{vpc}.SALS 0x{:x}", if on { SALS_USB_BATTERY_ON } else { SALS_USB_BATTERY_OFF }))?;
    let now = hals(&vpc)? & HALS_USB_BATTERY != 0;
    if now != on {
        record_unsupported(KEY_USB_BATTERY);
        return Err("the firmware accepted the request but kept the old state: this model has no separate on-battery switch (it will not be offered again)".into());
    }
    Ok(())
}

// Flip to start: UEFI variable ----------------------------------------------
//
// Firmware without the 0x00030000 capability keeps the setting in the UEFI
// variable FBSWIF-d743491e-f484-4952-a87d-8d5dd189b70c: 4 data bytes, byte 0 =
// enabled (LenovoLegionToolkit FlipToStartUEFIFeature and LenovoLegionLinux
// flip_to_start agree on name, GUID and layout). Only byte 0 is changed; the
// attribute dword and the other three bytes are written back as read. A
// variable of any other size is left alone.

const FBSWIF_VAR: &str = "/sys/firmware/efi/efivars/FBSWIF-d743491e-f484-4952-a87d-8d5dd189b70c";

fn fbswif_read() -> Option<[u8; 8]> { std::fs::read(FBSWIF_VAR).ok()?.try_into().ok() }

fn fbswif_set(on: bool) -> Result<(), String> {
    let mut v = fbswif_read().ok_or("the FBSWIF variable is missing or not 4 bytes; not touched")?;
    if (v[4] != 0) == on { return Ok(()); }
    v[4] = on as u8;
    crate::wlog::log("efivar", &format!("FBSWIF byte0 = {}", on as u8));
    crate::memory_spd::efivar_write(FBSWIF_VAR, &v)?;
    match fbswif_read() {
        Some(b) if b == v => Ok(()),
        _ => Err("FBSWIF read-back differs after the write".into()),
    }
}

enum FlipBackend { Wmae, Uefi }
fn flip_backend() -> Option<FlipBackend> {
    if cap_usable(0x0003_0000, CAP_GET | CAP_SET) { Some(FlipBackend::Wmae) }
    else if capabilities().map_or(true, |l| !l.iter().any(|c| c.id == 0x0003_0000)) && fbswif_read().is_some() { Some(FlipBackend::Uefi) }
    else { None }
}

/// {"ok":true,"toggles":{key:bool}} — only what this firmware reports.
/// (Kept under the historical name: the Home tab asks with "wmae_toggle".)
pub fn wmae_toggles_get() -> Value {
    if !acpi_available() { modprobe_acpi_call(); }
    let acpi = acpi_available();
    let hidden = unsupported_keys();
    let mut m = serde_json::Map::new();
    let mut put = |k: &str, v: bool| { if !hidden.iter().any(|h| h == k) { m.insert(k.into(), json!(v)); } };
    if acpi {
        for t in WMAE_TOGGLES.iter().filter(|t| t.key != KEY_FLIP && wmae_toggle_offered(t)) {
            if let Ok(v @ (0 | 1)) = wmae_get(t.id) { put(t.key, v == 1); }
        }
        for t in GZ_TOGGLES {
            if !wmaa(t.sup, 0).map_or(false, |v| v > 0) { continue; }
            if let Ok(v @ (0 | 1)) = wmaa(t.get, 0) { put(t.key, v == 1); }
        }
        if let Some(on) = usb_battery_get() { put(KEY_USB_BATTERY, on); }
    }
    match flip_backend() {
        Some(FlipBackend::Wmae) if acpi => { if let Ok(v @ (0 | 1)) = wmae_get(0x0003_0000) { put(KEY_FLIP, v == 1); } }
        Some(FlipBackend::Uefi) => { if let Some(v) = fbswif_read() { put(KEY_FLIP, v[4] != 0); } }
        _ => {}
    }
    if !acpi && m.is_empty() { return json!({"ok": false, "error": "acpi_call is not loaded"}); }
    json!({"ok": true, "toggles": m})
}

fn wmae_switch(key: &str, id: u32, on: bool) -> Result<(), String> {
    match wmae_get(id) { Ok(0 | 1) => {}, Ok(v) => return Err(format!("{key}: unsupported (getter {v})")), Err(e) => return Err(e) }
    wmae_set(id, on as i64)?;
    match wmae_get(id)? {
        v if (v == 1) == on && v <= 1 => Ok(()),
        v => Err(format!("{key}: read-back {v}")),
    }
}

pub fn wmae_toggle_set(key: &str, on: bool) -> Value {
    if unsupported_keys().iter().any(|k| k == key) {
        return json!({"ok": false, "error": format!("{key}: not supported by this firmware")});
    }
    let need_acpi = key != KEY_FLIP || matches!(flip_backend(), Some(FlipBackend::Wmae));
    if need_acpi && !acpi_available() { modprobe_acpi_call(); }
    if need_acpi && !acpi_available() { return json!({"ok": false, "error": "acpi_call is not loaded"}); }
    let r: Result<(), String> = if key == KEY_FLIP {
        match flip_backend() {
            Some(FlipBackend::Wmae) => wmae_switch(key, 0x0003_0000, on),
            Some(FlipBackend::Uefi) => fbswif_set(on),
            None => Err("flip to start is not available on this firmware".into()),
        }
    } else if key == KEY_USB_BATTERY {
        usb_battery_set(on)
    } else if let Some(t) = WMAE_TOGGLES.iter().find(|t| t.key == key) {
        if wmae_toggle_offered(t) { wmae_switch(key, t.id, on) } else { Err(format!("{key}: not in this firmware's capability list")) }
    } else if let Some(t) = GZ_TOGGLES.iter().find(|t| t.key == key) {
        (|| {
            if !wmaa(t.sup, 0).map_or(false, |v| v > 0) { return Err(format!("{key}: the firmware reports it unsupported")); }
            match wmaa(t.get, 0)? { 0 | 1 => {}, v => return Err(format!("{key}: unsupported (getter {v})")) }
            wmaa(t.set, on as u64)?;
            match wmaa(t.get, 0)? {
                v if v <= 1 && (v == 1) == on => Ok(()),
                v => Err(format!("{key}: read-back {v}")),
            }
        })()
    } else {
        Err(format!("unknown toggle '{key}'"))
    };
    match r { Ok(()) => json!({"ok": true, "key": key, "on": on}), Err(e) => json!({"ok": false, "error": e}) }
}

// ── Firmware CPU OC (PBO scalar, boost override, all-core CO) ───────────────
// LENOVO_CPU_METHOD CPU_Set_OC_Data = \_SB.GZFD.WMAC 0x0E {u32 mode, u32 TuneID,
// u32 value}; value is a plain integer, CO sign-magnitude (bit31 = negative).
// Current/min/max come from WQA6(0..2) as IEEE floats at +0x0C/+0x10/+0x14.
// Verified on the 16AFR10H: the store is the BIOS's own (PBO 5 set in setup
// reads back 5.0); values take effect at the next boot. GameZone WMAA 0x38
// GetBIOSOCMode: 0 = OC off in setup (values ignored).
const OC_TUNES: &[(&str, u32)] = &[("pbo_scalar", 0x414D_4401), ("boost_mhz", 0x414D_4402), ("curve_optimizer", 0x414D_4403)];

fn parse_buf(s: &str) -> Vec<u8> {
    s.trim_matches(|c| c == '{' || c == '}').split(',').filter_map(|t| parse_u64(t.trim()).map(|v| v as u8)).collect()
}
fn f32_at(b: &[u8], o: usize) -> Option<f32> { b.get(o..o + 4).map(|x| f32::from_le_bytes([x[0], x[1], x[2], x[3]])) }

pub fn fw_oc_status() -> Value {
    if !acpi_available() { modprobe_acpi_call(); }
    if !acpi_available() { return json!({"ok": false, "error": "acpi_call is not loaded"}); }
    let mode = wmaa(0x38, 0).ok();
    let mut tunes = serde_json::Map::new();
    let mut why = Vec::new();
    for (i, (k, id)) in OC_TUNES.iter().enumerate() {
        let raw = match acpi_raw(&format!("{}.WQA6 0x{i:x}", wmi().dev)) { Ok(r) => r, Err(e) => { why.push(format!("{k}: {e}")); continue } };
        let b = parse_buf(&raw);
        if b.len() < 0x18 || u32::from_le_bytes([b[4], b[5], b[6], b[7]]) != *id {
            why.push(format!("{k}: unexpected reply '{}'", raw.chars().take(60).collect::<String>()));
            continue;
        }
        let (cur, min, max) = (f32_at(&b, 0x0C).unwrap(), f32_at(&b, 0x10).unwrap(), f32_at(&b, 0x14).unwrap());
        let cur = if cur == 0.0 { 0.0 } else { cur }; // -0.0 = CO off
        tunes.insert((*k).into(), json!({"value": cur.round() as i64, "min": min.round() as i64, "max": max.round() as i64}));
    }
    if tunes.is_empty() { return json!({"ok": false, "error": format!("firmware has no CPU OC tunes (WQA6): {}", why.join("; "))}); }
    json!({"ok": true, "bios_oc_mode": mode, "tunes": tunes})
}

pub fn set_fw_oc(key: &str, value: i64) -> Value {
    let Some(&(_, id)) = OC_TUNES.iter().find(|(k, _)| *k == key) else { return json!({"ok": false, "error": format!("unknown tune '{key}'")}) };
    let st = fw_oc_status();
    let Some(t) = st.get("tunes").and_then(|t| t.get(key)) else { return json!({"ok": false, "error": format!("{key} not reported by firmware")}) };
    let (lo, hi) = (t["min"].as_i64().unwrap_or(0), t["max"].as_i64().unwrap_or(0));
    if value < lo || value > hi { return json!({"ok": false, "error": format!("{key}: {value} outside {lo}..{hi}")}); }
    let raw: u32 = if value < 0 { 0x8000_0000 | value.unsigned_abs() as u32 } else { value as u32 };
    let mut b = Vec::with_capacity(12);
    b.extend_from_slice(&0x11u32.to_le_bytes());
    b.extend_from_slice(&id.to_le_bytes());
    b.extend_from_slice(&raw.to_le_bytes());
    let hex: String = b.iter().map(|x| format!("{x:02X}")).collect();
    if let Err(e) = acpi_raw(&format!("{}.WMAC 0x0 0x0E b{hex}", wmi().dev)) { return json!({"ok": false, "error": e}); }
    let now = fw_oc_status();
    let got = now.get("tunes").and_then(|t| t.get(key)).and_then(|t| t["value"].as_i64());
    if got != Some(value) { return json!({"ok": false, "error": format!("{key}: read-back {got:?}")}); }
    json!({"ok": true, "key": key, "value": value, "reboot_required": true})
}

/// Logs the BIOS-side state (profile + firmware OC tunes + BIOS OC mode) so a
/// change between boot, writes and shutdown shows up in writes.log.
pub fn log_state(tag: &str) {
    if !acpi_available() { modprobe_acpi_call(); }
    let s = format!("profile={:?} fw_oc={}", platform_profile(), fw_oc_status());
    crate::wlog::log(tag, &s);
}

// ── Device report ───────────────────────────────────────────────────────────
// What the firmware says about itself, for the "Device information" dialog and
// for bug reports from other models: nothing here writes.

fn cap_name(id: u32) -> Option<&'static str> {
    // Per-mode ids (0x0101FF00 = "CPU short-term limit, Custom mode") share one name.
    let base = if matches!(id >> 24, 0x01 | 0x02) { id & 0xFFFF_0000 } else { id };
    Some(match base {
        0x0001_0000 => "iGPU mode", 0x0003_0000 => "Flip to start", 0x0004_0000 => "NVIDIA Advanced Optimus (DDS)",
        0x0005_0001 => "AMD SmartShift", 0x0005_0002 => "AMD skin temperature tracking",
        0x0007_0000 => "Supported power modes", 0x0009_0000 => "Legion Zone interface version",
        0x000A_0000 => "Auto refresh-rate switching", 0x000E_0000 => "Lenovo AI chip", 0x000F_0000 => "iGPU mode change status",
        0x0010_0000 => "Custom mode in Fn+Q", 0x001A_0000 => "Panel Over Drive",
        0x0101_0000 => "CPU short-term power limit", 0x0102_0000 => "CPU long-term power limit",
        0x0103_0000 => "CPU peak power limit", 0x0104_0000 => "CPU temperature limit",
        0x0105_0000 => "APU sPPT power limit", 0x0106_0000 => "CPU cross-loading power limit",
        0x0107_0000 => "CPU PL1 tau", 0x0108_0000 => "CPU overclocking enable",
        0x0201_0000 => "GPU power boost", 0x0202_0000 => "GPU configurable TGP", 0x0203_0000 => "GPU temperature limit",
        0x0204_0000 => "GPU total power target offset (AC)", 0x0207_0000 => "GPU status", 0x0208_0000 => "Re-evaluate power budgets",
        0x0209_0000 => "GPU device id", 0x020B_0000 => "GPU to CPU dynamic boost",
        0x0301_0001 => "Instant boot (AC)", 0x0301_0002 => "Instant boot (USB-PD)",
        0x0402_0000 => "Fan full speed", 0x0403_0001 => "CPU fan speed", 0x0403_0002 => "GPU fan speed", 0x0403_0004 => "PCH fan speed",
        0x0501_0000 => "PCH temperature", 0x0504_0000 => "CPU temperature", 0x0505_0000 => "GPU temperature",
        _ => return None,
    })
}

/// Bits of the "supported power modes" capability (0x00070000).
fn power_modes(v: u64) -> Vec<&'static str> {
    [(0, "quiet"), (1, "balanced"), (2, "performance"), (4, "extreme"), (16, "custom")].iter()
        .filter(|&&(b, _)| v >> b & 1 == 1).map(|&(_, n)| n).collect()
}

pub fn device_report() -> Value {
    if !acpi_available() { modprobe_acpi_call(); }
    let mi = crate::machine::info();
    let mut out = json!({
        "ok": true,
        "model": mi.model, "machine_type": mi.machine_type, "series": mi.series.name(), "generation": mi.generation,
        "bios": std::fs::read_to_string("/sys/class/dmi/id/bios_version").map(|s| s.trim().to_owned()).unwrap_or_default(),
        "acpi_call": acpi_available(),
    });
    let wmi_blocks: Vec<&str> = [("887B54E3-DDDC-4B2C-8B88-68A26A8835D0", "GameZone"), (OTHER_METHOD_GUID, "Other method (capabilities)"),
        ("92549549-4BDE-4F06-AC04-CE8BF898DBAA", "Fan method"), (CAPDATA00_GUID, "Capability data 00"),
        ("7A8F5407-CB67-4D6E-B547-39B3BE018154", "Capability data 01"), ("14AFD777-106F-4C9B-B334-D388DC7809BE", "CPU method"),
        ("DA7547F1-824D-405F-BE79-D9903E29CED7", "GPU method"), ("8C5B9127-ECD4-4657-980F-851019F99CA5", "Lighting method")]
        .iter().filter(|(g, _)| crate::fan_table::wmi_node(g).is_ok()).map(|&(_, n)| n).collect();
    out["wmi"] = json!(wmi_blocks);
    out["vpc2004"] = json!(vpc_path());
    if let Some(min) = crate::machine::custom_mode_needs_bios(&mi.bios) {
        out["note"] = json!(format!("Custom mode needs BIOS {}{min} or newer on this board", mi.bios.prefix));
    }
    if !acpi_available() { return out; }
    if wmi_blocks.contains(&"GameZone") {
        out["smartfan_version"] = json!(wmaa(0x2B, 0).ok());
        out["thermal_mode"] = json!(wmaa(0x37, 0).ok());
    }
    if let Some(caps) = capabilities() {
        if cap_usable_in(Some(caps), 0x0007_0000, CAP_GET) {
            if let Ok(v) = wmae_get(0x0007_0000) { out["power_modes"] = json!(power_modes(v as u64)); }
        }
        if cap_usable_in(Some(caps), 0x0009_0000, CAP_GET) { out["legion_zone_version"] = json!(wmae_get(0x0009_0000).ok()); }
        out["capabilities"] = Value::Array(caps.iter().map(|c| json!({
            "id": format!("0x{:08X}", c.id), "name": cap_name(c.id), "valid": c.flags & CAP_VALID != 0,
            "get": c.flags & CAP_GET != 0, "set": c.flags & CAP_SET != 0, "default": c.default,
        })).collect());
    }
    out
}
