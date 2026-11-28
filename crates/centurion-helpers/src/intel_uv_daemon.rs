//! Boot config + daemon for the Intel undervolt tool.
//!
//! Replicates:
//!   * intel-undervolt `daemon`: periodic re-apply (undervolt once, power and
//!     tjoffset every interval), config reload on SIGHUP (it uses SIGUSR1 —
//!     both are accepted), hwphint EPP switching by CPU load or RAPL power.
//!   * throttled: separate AC / BATTERY profiles, power-source polling,
//!     full re-apply when the source flips, Autoreload on config mtime change.
//!   * resume: intel-undervolt/throttled rely on sleep hooks / D-Bus; here a
//!     jump of CLOCK_BOOTTIME against CLOCK_MONOTONIC (time spent suspended)
//!     triggers a full re-apply, so no hook is needed while the daemon runs.

use crate::intel_uv::{self, HwpAlgo, HwpRule, Profile};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

pub const BOOT_FILE: &str = "/etc/centurion/intel-uv-boot.json";

#[derive(Debug, Clone)]
pub struct BootConfig { pub ac: Option<Profile>, pub battery: Option<Profile>, pub interval_ms: u64, pub reapply: bool, pub alarm: Option<Alarm> }

/// Temperature alarm (ThrottleStop's failsafe alarm profile): at `temp_c` (package) PL1/PL2 are pulled
/// down to these values — never raised: the lower of them and the live limits wins — and the
/// limits from before the alarm come back once the package is ALARM_BAND_C cooler.
#[derive(Debug, Clone, PartialEq)]
pub struct Alarm { pub temp_c: u64, pub pl1_w: f64, pub pl2_w: f64 }
pub const ALARM_BAND_C: u64 = 10;

/// Some(new state) when the alarm switches on or off at this package temperature.
pub fn alarm_transition(active: bool, temp_c: u64, a: &Alarm) -> Option<bool> {
    if !active && temp_c >= a.temp_c { Some(true) }
    else if active && temp_c + ALARM_BAND_C <= a.temp_c { Some(false) }
    else { None }
}

/// The alarm's PL1/PL2 given the live ones: each the lower of the two.
pub fn alarm_limits(live: (f64, f64), a: &Alarm) -> (f64, f64) { (live.0.min(a.pl1_w), live.1.min(a.pl2_w)) }

fn parse_alarm(v: &Value) -> Result<Alarm, String> {
    let n = |k: &str| v.get(k).and_then(Value::as_f64).ok_or(format!("daemon.alarm.{k} is required"));
    let t = v.get("temp_c").and_then(Value::as_u64).ok_or("daemon.alarm.temp_c must be an integer")?;
    if !(60..=105).contains(&t) { return Err("daemon.alarm.temp_c must be 60..105 °C".into()); }
    let (a, b) = (n("pl1_w")?, n("pl2_w")?);
    if !(1.0..=intel_uv::PL_MAX_W).contains(&a) || !(1.0..=intel_uv::PL_MAX_W).contains(&b) { return Err("daemon.alarm PL must be 1..250 W".into()); }
    if a > b { return Err("daemon.alarm.pl1_w must be ≤ pl2_w".into()); }
    Ok(Alarm { temp_c: t, pl1_w: a, pl2_w: b })
}

/// {"ac": profile|null, "battery": profile|null, "daemon": {"interval_ms": 5000, "reapply": true,
///  "alarm": {"temp_c": 95, "pl1_w": 25, "pl2_w": 35} (optional)}}
/// A plain profile (no ac/battery keys) is used for both sources.
pub fn parse_boot(v: &Value) -> Result<BootConfig, String> {
    let o = v.as_object().ok_or("boot config must be an object")?;
    let split = o.contains_key("ac") || o.contains_key("battery");
    let prof = |k: &str| -> Result<Option<Profile>, String> {
        match o.get(k) { None | Some(Value::Null) => Ok(None),
            Some(p) => intel_uv::parse_profile(p).map(Some).map_err(|e| format!("{k}: {e}")) }
    };
    let (ac, battery) = if split { (prof("ac")?, prof("battery")?) } else {
        let p = intel_uv::parse_profile(v)?;
        (Some(p.clone()), Some(p))
    };
    let d = o.get("daemon").and_then(Value::as_object);
    let interval_ms = d.and_then(|d| d.get("interval_ms")).and_then(Value::as_u64).unwrap_or(5000);
    if !(500..=600_000).contains(&interval_ms) { return Err("daemon.interval_ms must be 500..600000".into()); }
    let reapply = d.and_then(|d| d.get("reapply")).and_then(Value::as_bool).unwrap_or(true);
    let alarm = match d.and_then(|d| d.get("alarm")) { None | Some(Value::Null) => None, Some(a) => Some(parse_alarm(a)?) };
    Ok(BootConfig { ac, battery, interval_ms, reapply, alarm })
}

pub fn within_gui_floor(p: &Profile) -> Result<(), String> {
    let floor = intel_uv::uv_floor_mv();
    match p.voltage.iter().find(|(_, _, mv)| *mv < floor) {
        Some((plane, _, mv)) => Err(format!("voltage.{plane}: {mv} mV is below this CPU's {floor} mV floor")),
        None => Ok(()),
    }
}

pub fn boot_within_gui_floor(c: &Value) -> Result<(), String> {
    let b = parse_boot(c)?;
    b.ac.iter().chain(&b.battery).try_for_each(within_gui_floor)
}

pub fn load_boot() -> Result<Option<BootConfig>, String> {
    if !Path::new(BOOT_FILE).exists() { return Ok(None); }
    // Root writes MSR voltages from this file at every boot/resume: only a
    // root-owned, not group/other-writable regular file is trusted.
    let s = crate::read_root_file(BOOT_FILE, 256 * 1024)
        .ok_or_else(|| format!("{BOOT_FILE}: not a root-owned, non-writable regular file (or too large) — ignored"))?;
    let v: Value = serde_json::from_str(&s).map_err(|e| format!("{BOOT_FILE}: {e}"))?;
    parse_boot(&v).map(Some).map_err(|e| format!("{BOOT_FILE}: {e}"))
}

/// Power source for the one-shot paths: the shared rule (Mains or USB-PD online, a failed read is
/// "cannot tell", not "battery"). No supplies at all (a desktop) or still unreadable → AC, as before.
pub fn on_ac() -> bool { crate::power::on_ac_settled().unwrap_or(true) }

// ── scene hold ─────────────────────────────────────────────────────────────
// A scene's Intel undervolt profile is applied by intel-uv-helper, not by the daemon. The daemon used
// to write its own boot profile's PL/TCC over it at the next interval and its voltages at the next
// power-source change. While a hold is recorded the daemon serves the *held* profile instead (periodic
// re-apply, hwphint, full apply after resume) and leaves source switching to the scene engine. The hold
// ends when the boot configuration is saved again or cleared (the helper removes the file) and at reboot
// (/run). Root-owned: only the helper writes it, after validating the profile.
pub const HOLD_FILE: &str = "/run/centurion/intel-uv-hold.json";

/// Records the profile a scene just applied (`None` = the scene reset the voltages).
pub fn write_hold(profile: Option<&Value>) -> Result<(), String> {
    crate::secure_dir("/run/centurion")?;
    crate::write_root_file(HOLD_FILE, json!({"profile": profile}).to_string().as_bytes())
}

pub fn clear_hold() { let _ = std::fs::remove_file(HOLD_FILE); }

/// None = no hold; Some(None) = held with nothing to keep alive (scene reset); Some(Some(p)) = held profile.
fn load_hold() -> Option<Option<Profile>> {
    let v: Value = serde_json::from_str(&crate::read_root_file(HOLD_FILE, 256 * 1024)?).ok()?;
    match &v["profile"] {
        Value::Null => Some(None),
        p => intel_uv::parse_profile(p).ok().map(Some),
    }
}

impl BootConfig {
    pub fn for_source(&self, ac: bool) -> Option<&Profile> { if ac { self.ac.as_ref() } else { self.battery.as_ref() } }

    /// What a full apply for `ac` must write: the source's own profile plus
    /// 0 mV for every voltage plane the *other* source offsets but this one
    /// leaves out. Without that, AC → battery kept the AC undervolt on every
    /// plane the battery profile did not name — and a source without any
    /// profile kept the whole previous undervolt. Power limits, IccMax, TCC
    /// and BD PROCHOT have no stock of their own here: see `full_apply_with`.
    pub fn full_apply(&self, ac: bool) -> Option<Profile> { self.full_apply_with(ac, &json!({})) }

    /// `full_apply`, plus the firmware's own PL1/PL2, IccMax, TCC offset and BD PROCHOT (`stock`, as
    /// `intel_uv::stock_load` recorded them before this boot's first write) for every one of those
    /// the *other* source sets and this one leaves out.
    pub fn full_apply_with(&self, ac: bool, stock: &Value) -> Option<Profile> {
        let mine = self.for_source(ac);
        let other = self.for_source(!ac);
        let mut p = mine.cloned().unwrap_or_default();
        for &(k, idx, _) in other.map(|o| o.voltage.as_slice()).unwrap_or(&[]) {
            if !p.voltage.iter().any(|v| v.0 == k) { p.voltage.push((k, idx, 0.0)); }
        }
        let mut filled = false;
        if let Some(o) = other {
            let s = intel_uv::stock_profile(stock, Some(o));
            if p.pl1.is_none() && s.pl1.is_some() { p.pl1 = s.pl1; filled = true; }
            if p.pl2.is_none() && s.pl2.is_some() { p.pl2 = s.pl2; filled = true; }
            if p.tjoffset.is_none() && s.tjoffset.is_some() { p.tjoffset = s.tjoffset; filled = true; }
            if p.disable_bdprochot.is_none() && s.disable_bdprochot.is_some() { p.disable_bdprochot = s.disable_bdprochot; filled = true; }
            for i in s.iccmax { if !p.iccmax.iter().any(|m| m.0 == i.0) { p.iccmax.push(i); filled = true; } }
        }
        (mine.is_some() || !p.voltage.is_empty() || filled).then_some(p)
    }
}

// ── boot guard ─────────────────────────────────────────────────────────────
// An undervolt that locks the machine up is stored in the boot file and would be applied again at the
// next boot, forever. The guard file is written before the first apply of a boot and removed once the
// machine has run GUARD_SETTLE after it. A boot that finds the file from an *earlier* boot for the very
// same boot configuration was cut short: that configuration is quarantined (nothing applied) until the
// file's content changes or the guard file is removed.
pub const GUARD_DIR: &str = "/var/lib/centurion";
pub const GUARD_FILE: &str = "/var/lib/centurion/intel-uv-boot-guard.json";
const GUARD_SETTLE: Duration = Duration::from_secs(60);

#[derive(Debug, PartialEq)]
pub enum Guard { Fresh, SameBoot, Quarantine }

/// `stored` = (boot id, config fingerprint) of the guard file, if any.
pub fn guard_verdict(stored: Option<(&str, &str)>, boot_id: &str, config: &str) -> Guard {
    match stored {
        None => Guard::Fresh,
        Some((b, _)) if b == boot_id => Guard::SameBoot,
        Some((_, c)) if c != config => Guard::Fresh,
        Some(_) => Guard::Quarantine,
    }
}

/// FNV-1a of the boot file's bytes (stable across Rust versions, unlike DefaultHasher).
fn fingerprint() -> Option<String> {
    let s = crate::read_root_file(BOOT_FILE, 256 * 1024)?;
    Some(format!("{:016x}", s.bytes().fold(0xcbf2_9ce4_8422_2325u64, |h, b| (h ^ b as u64).wrapping_mul(0x100_0000_01b3))))
}

fn read_guard() -> Option<(String, String)> {
    let v: Value = serde_json::from_str(&crate::read_root_file(GUARD_FILE, 4096)?).ok()?;
    Some((v["boot_id"].as_str()?.to_owned(), v["config"].as_str()?.to_owned()))
}

fn write_guard(boot_id: &str, config: &str) -> Result<(), String> {
    crate::secure_dir(GUARD_DIR)?;
    crate::write_root_file(GUARD_FILE, json!({"boot_id": boot_id, "config": config}).to_string().as_bytes())
}

/// Retry pacing for a full apply that failed (and was rolled back): a few tries, further apart each
/// time, then nothing until something changes (config, power source, resume). Every try writes the
/// hardware and puts it back, so it must not repeat every interval.
#[derive(Default)]
struct Retry { n: u32, at: Option<Instant>, exhausted: bool }
impl Retry {
    const TRIES: u32 = 3;
    fn ready(&self) -> bool { !self.exhausted && self.at.map_or(true, |t| Instant::now() >= t) }
    fn reset(&mut self) { *self = Retry::default(); }
    /// True when this was the last try.
    fn failed(&mut self, result: &Value) -> bool {
        self.n += 1;
        self.at = Some(Instant::now() + Duration::from_secs(30 * self.n as u64));
        self.exhausted = self.n >= Self::TRIES || result["state"] == "unknown";
        self.exhausted
    }
}

// ── hwphint (intel-undervolt scaling.c / stat.c / power.c) ─────────────────

#[derive(Default)]
struct CpuStat { prev: HashMap<usize, (u64, u64)>, single: f64, multi: f64 }

impl CpuStat {
    /// Per-CPU busy fraction from /proc/stat (idle = 4th field, as in
    /// stat.c). single = max over CPUs. multi = mean: intel-undervolt sums the
    /// per-CPU loads without dividing, so its multi threshold is really a
    /// "number of busy CPUs" and 0.8 triggers on almost any load — fixed here.
    fn measure(&mut self) {
        let Ok(s) = std::fs::read_to_string("/proc/stat") else { return };
        let (mut single, mut sum, mut n) = (0.0f64, 0.0f64, 0usize);
        for line in s.lines() {
            let mut it = line.split_whitespace();
            let Some(name) = it.next() else { continue };
            if !name.starts_with("cpu") || name.len() == 3 { continue; }
            let Ok(idx) = name[3..].parse::<usize>() else { continue };
            // Streamed: no per-line Vec (one line per CPU, every interval).
            let (mut total, mut idle, mut cnt) = (0u64, 0u64, 0usize);
            for x in it.filter_map(|x| x.parse::<u64>().ok()) {
                if cnt == 3 { idle = x; }
                total += x;
                cnt += 1;
            }
            if cnt < 4 { continue; }
            if let Some(&(pt, pi)) = self.prev.get(&idx) {
                if total > pt {
                    let load = (total - pt).saturating_sub(idle.saturating_sub(pi)) as f64 / (total - pt) as f64;
                    single = single.max(load);
                    sum += load;
                    n += 1;
                }
            }
            self.prev.insert(idx, (total, idle));
        }
        self.single = single;
        self.multi = if n > 0 { sum / n as f64 } else { 0.0 };
    }
}

#[derive(Default)]
struct Rapl {
    prev: HashMap<String, (u64, u64, Instant)>,
    power: HashMap<String, f64>,
    /// Zone dir name → (power key, max_energy_range_uj): both are constant, read once per zone.
    zones: HashMap<String, (String, u64)>,
}

impl Rapl {
    /// powercap energy_uj deltas, every zone and subzone (package-0, core,
    /// uncore, dram, psys …), as intel-undervolt's power.c.
    fn measure(&mut self) {
        let now = Instant::now();
        let Ok(rd) = std::fs::read_dir("/sys/class/powercap") else { return };
        for e in rd.flatten() {
            let p = e.path();
            let fname = e.file_name().to_string_lossy().into_owned();
            if !fname.starts_with("intel-rapl:") { continue; } // MSR interface only; -mmio mirrors package
            if !self.zones.contains_key(&fname) {
                let Ok(name) = std::fs::read_to_string(p.join("name")) else { continue };
                let range = std::fs::read_to_string(p.join("max_energy_range_uj")).ok().and_then(|s| s.trim().parse::<u64>().ok()).unwrap_or(u64::MAX);
                self.zones.insert(fname.clone(), (format!("{}#{fname}", name.trim()), range));
            }
            let Ok(uj) = std::fs::read_to_string(p.join("energy_uj")) else { continue };
            let Ok(uj) = uj.trim().parse::<u64>() else { continue };
            let (key, range) = &self.zones[&fname];
            let range = *range;
            if let Some(&(pu, _, pt)) = self.prev.get(key) {
                let dt = now.duration_since(pt).as_secs_f64();
                let de = if uj >= pu { uj - pu } else { uj + range.saturating_sub(pu) };
                if dt > 0.0 { self.power.insert(key.clone(), de as f64 / 1e6 / dt); }
            }
            self.prev.insert(key.clone(), (uj, range, now));
        }
    }
    /// rapl_lookup: name == domain or name starts with "domain-".
    fn get(&self, domain: &str) -> f64 {
        self.power.iter().find(|(k, _)| {
            let n = k.split('#').next().unwrap_or("");
            n == domain || n.strip_prefix(domain).map(|r| r.starts_with('-')).unwrap_or(false)
        }).map(|(_, &w)| w).unwrap_or(0.0)
    }
}

pub struct Hwp { stat: CpuStat, rapl: Rapl }

impl Hwp {
    pub fn new() -> Hwp { Hwp { stat: CpuStat::default(), rapl: Rapl::default() } }

    /// One cpu_policy_update pass. switch mode only touches policies whose
    /// current EPP is one of the rule's two hints (so a user/Optimizations
    /// choice is left alone); force writes every policy. First rule that
    /// handles a policy wins.
    pub fn update(&mut self, rules: &[HwpRule]) -> Vec<String> {
        let mut log = Vec::new();
        if rules.is_empty() { return log; }
        if rules.iter().any(|r| matches!(r.algo, HwpAlgo::Load { .. })) { self.stat.measure(); }
        if rules.iter().any(|r| matches!(r.algo, HwpAlgo::Power(_))) { self.rapl.measure(); }
        let Ok(rd) = std::fs::read_dir("/sys/devices/system/cpu/cpufreq") else { return log };
        let mut pols: Vec<_> = rd.flatten().map(|e| e.path()).filter(|p| p.file_name().map(|n| n.to_string_lossy().starts_with("policy")).unwrap_or(false)).collect();
        pols.sort();
        for pol in pols {
            let f = pol.join("energy_performance_preference");
            let Ok(cur) = std::fs::read_to_string(&f).map(|s| s.trim().to_owned()) else { continue };
            for r in rules {
                if !r.force && cur != r.load_hint && cur != r.normal_hint { continue; }
                let load = match &r.algo {
                    HwpAlgo::Load { multi, threshold } => (if *multi { self.stat.multi } else { self.stat.single }) >= *threshold,
                    // The first term's connective is ignored (see HwpAlgo::Power): folding from
                    // `false` made a first term with "and": true always false.
                    HwpAlgo::Power(terms) => terms.iter().enumerate().fold(false, |acc, (i, t)| {
                        let w = self.rapl.get(&t.domain);
                        let c = if t.greater { w > t.watts } else { w < t.watts };
                        if i == 0 { c } else if t.and { acc & c } else { acc | c }
                    }),
                };
                let hint = if load { &r.load_hint } else { &r.normal_hint };
                if r.force || cur != *hint {
                    if let Err(e) = crate::sysfs_write(&f, hint.as_bytes()) { log.push(format!("{}: {e}", f.display())); }
                }
                break;
            }
        }
        log
    }
}

impl Default for Hwp { fn default() -> Self { Self::new() } }

// ── daemon loop ────────────────────────────────────────────────────────────

/// fn item → fn pointer → integer: the two-step cast rustc's
/// function_casts_as_integer lint asks for (same value, no behaviour change).
fn handler(f: extern "C" fn(libc::c_int)) -> libc::sighandler_t { f as *const () as libc::sighandler_t }

static STOP: AtomicBool = AtomicBool::new(false);
static RELOAD: AtomicBool = AtomicBool::new(false);
/// Write end of the self-pipe the handlers poke (-1 until set up).
static WAKE_FD: std::sync::atomic::AtomicI32 = std::sync::atomic::AtomicI32::new(-1);
fn poke() {
    let fd = WAKE_FD.load(Ordering::SeqCst);
    if fd >= 0 { unsafe { libc::write(fd, b"x".as_ptr().cast(), 1); } }  // async-signal-safe; full pipe = already poked
}
extern "C" fn on_stop(_: libc::c_int) { STOP.store(true, Ordering::SeqCst); poke(); }
extern "C" fn on_reload(_: libc::c_int) { RELOAD.store(true, Ordering::SeqCst); poke(); }

/// Sleeps up to `ms`, returning early on SIGTERM/SIGINT/SIGHUP/SIGUSR1.
/// One poll() on a self-pipe: the old loop slept in 100 ms slices, i.e. ten
/// wake-ups a second for the whole life of an always-on root daemon. The
/// pipe (not a blocked-signal sigtimedwait) keeps modprobe & co. spawned from
/// here on a normal signal mask, and a signal that lands between the flag
/// check and poll() is not lost: its byte is already in the pipe.
fn wait(rd: libc::c_int, ms: u64) {
    if rd < 0 {  // no pipe (fd exhaustion): the old sliced sleep
        let end = Instant::now() + Duration::from_millis(ms);
        while Instant::now() < end && !STOP.load(Ordering::SeqCst) && !RELOAD.load(Ordering::SeqCst) {
            std::thread::sleep(Duration::from_millis(100.min(ms)));
        }
        return;
    }
    let end = Instant::now() + Duration::from_millis(ms);
    loop {
        if STOP.load(Ordering::SeqCst) || RELOAD.load(Ordering::SeqCst) { break; }
        let left = end.saturating_duration_since(Instant::now());
        if left.is_zero() { break; }
        let mut p = libc::pollfd { fd: rd, events: libc::POLLIN, revents: 0 };
        let r = unsafe { libc::poll(&mut p, 1, left.as_millis().min(i32::MAX as u128) as libc::c_int) };
        if r > 0 {
            let mut b = [0u8; 64];
            while unsafe { libc::read(rd, b.as_mut_ptr().cast(), b.len()) } > 0 {}  // drain (non-blocking)
        }
        // r < 0 (EINTR) or a drained poke: loop re-checks the flags and the deadline.
    }
}

fn clock(id: libc::clockid_t) -> f64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(id, &mut ts) };
    ts.tv_sec as f64 + ts.tv_nsec as f64 * 1e-9
}
fn suspended_s() -> f64 { clock(libc::CLOCK_BOOTTIME) - clock(libc::CLOCK_MONOTONIC) }

/// `periodic`: the every-interval re-apply. Its failures are usually
/// permanent (a limit locked by firmware) and used to be logged every few
/// seconds forever; now a failure is logged when it first appears or its text
/// changes, and once more when it clears.
/// A full profile is effective only when every requested field succeeded.
/// An unsuccessful full write stays pending so the next tick retries it.
fn fully_applied(v: &Value) -> bool {
    v["ok"] == true && v["results"].as_array().is_some_and(|rs| rs.iter().all(|r| r["ok"] == true))
}

/// Context-transition journal: the last full applies (boot, power-source switch, resume, scene) with each
/// field's read-back result, readable by the GUI (Intel Undervolt → Context transitions).
pub const TRANSITIONS_FILE: &str = "/run/centurion/intel-uv-transitions.json";
const TRANSITIONS_KEPT: usize = 20;

/// One journal entry. `intel_uv::apply` reads every written field back; a field that the firmware refused
/// (lock bit, MSR writes blocked) carries its reason, and `locked` marks it so the GUI can say so plainly.
pub fn transition_entry(t: u64, event: &str, source: &str, v: &Value) -> Value {
    let fields: Vec<Value> = v["results"].as_array().into_iter().flatten().map(|r| {
        let msg = r["message"].as_str().unwrap_or("");
        let lower = msg.to_ascii_lowercase();
        json!({"what": r["what"], "ok": r["ok"] == true, "message": msg,
               "locked": r["ok"] != true && (lower.contains("locked") || lower.contains("lockdown") || lower.contains("blocked"))})
    }).collect();
    let mut e = json!({"t": t, "event": event, "source": source, "ok": fully_applied(v), "fields": fields});
    if let Some(err) = v["error"].as_str() { e["error"] = json!(err); }
    e
}

fn record_transition(journal: &mut Vec<Value>, event: &str, source: &str, v: &Value) {
    let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    journal.push(transition_entry(t, event, source, v));
    if journal.len() > TRANSITIONS_KEPT { journal.remove(0); }
    if crate::secure_dir("/run/centurion").is_ok() {
        let _ = crate::write_root_file(TRANSITIONS_FILE, json!({"version": 1, "entries": journal}).to_string().as_bytes());
    }
}

fn report(tag: &str, v: &Value, periodic: bool) {
    use std::sync::Mutex;
    static SEEN: Mutex<Vec<(String, String)>> = Mutex::new(Vec::new());
    // One line per result: (dedup key, ok, text).
    let mut lines: Vec<(String, bool, String)> = Vec::new();
    match v["error"].as_str() {
        Some(e) => lines.push((tag.to_owned(), false, format!("centurion-intel-uv: {tag}: {e}"))),
        None => {
            lines.push((tag.to_owned(), true, String::new()));  // a whole-request error is gone
            for r in v["results"].as_array().into_iter().flatten() {
                let ok = r["ok"] == true;
                let what = r["what"].as_str().unwrap_or("");
                lines.push((format!("{tag} {what}"), ok, format!("centurion-intel-uv: {tag}: {} {} {}",
                    if ok { "OK " } else { "ERR" }, what, r["message"].as_str().unwrap_or(""))));
            }
        }
    }
    if !periodic {
        for (_, _, l) in lines.iter().filter(|x| !x.2.is_empty()) { eprintln!("{l}"); }
        return;
    }
    let mut seen = SEEN.lock().unwrap_or_else(|e| e.into_inner());
    for (key, ok, line) in lines {
        let pos = seen.iter().position(|(k, _)| *k == key);
        match (ok, pos) {
            (true, Some(i)) => { seen.remove(i); eprintln!("centurion-intel-uv: {key}: OK again"); }
            (true, None) => {}
            (false, Some(i)) if seen[i].1 == line => {}
            (false, Some(i)) => { seen[i].1 = line.clone(); eprintln!("{line}"); }
            (false, None) => { eprintln!("{line}"); seen.push((key, line)); }
        }
    }
}

fn mtime() -> Option<std::time::SystemTime> { std::fs::metadata(BOOT_FILE).and_then(|m| m.modified()).ok() }

pub fn run_daemon() -> i32 {
    let mut pipe = [-1 as libc::c_int; 2];
    let rd = if unsafe { libc::pipe2(pipe.as_mut_ptr(), libc::O_CLOEXEC | libc::O_NONBLOCK) } == 0 {
        WAKE_FD.store(pipe[1], Ordering::SeqCst);
        pipe[0]
    } else { -1 };
    unsafe {
        libc::signal(libc::SIGTERM, handler(on_stop));
        libc::signal(libc::SIGINT, handler(on_stop));
        libc::signal(libc::SIGHUP, handler(on_reload));
        libc::signal(libc::SIGUSR1, handler(on_reload));
    }
    let mut cfg = match load_boot() { Ok(c) => c, Err(e) => { eprintln!("centurion-intel-uv: {e}"); None } };
    let mut stamp = mtime();
    let mut applied: Option<bool> = None; // source of the last full apply
    let mut slept = suspended_s();
    let mut hwp = Hwp::new();
    let mut source = crate::power::Source::new(true);
    let hold_mtime = || std::fs::metadata(HOLD_FILE).and_then(|m| m.modified()).ok();
    let mut hold_stamp = hold_mtime();
    let mut hold = load_hold();
    let mut hold_live = true;  // the held profile is in the hardware (false after a resume)
    let mut alarm_saved: Option<u64> = None;  // MSR_PKG_POWER_LIMIT from before the alarm (Some = active)
    let mut journal: Vec<Value> = Vec::new();
    let mut why = "boot";  // what caused the next full apply (journal)
    let mut retry = Retry::default();       // failed full apply of the boot configuration
    let mut hold_retry = Retry::default();  // failed full apply of a scene's profile
    let mut last_ac: Option<bool> = None;
    let mut quarantined: Option<String> = None;  // fingerprint of a boot file that cut an earlier boot short
    let mut guard_settle: Option<Instant> = None;
    if cfg.is_some() {
        let bid = crate::bootguard::boot_id();
        if let (Some(fp), false) = (fingerprint(), bid.is_empty()) {
            let stored = read_guard();
            match guard_verdict(stored.as_ref().map(|(b, c)| (b.as_str(), c.as_str())), &bid, &fp) {
                Guard::Quarantine => {
                    eprintln!("centurion-intel-uv: the previous boot did not outlast applying this boot configuration — NOT applying it. \
                               Change {BOOT_FILE} or remove {GUARD_FILE} to try again");
                    quarantined = Some(fp);
                }
                Guard::Fresh => match write_guard(&bid, &fp) {
                    Ok(()) => guard_settle = Some(Instant::now() + GUARD_SETTLE),
                    Err(e) => eprintln!("centurion-intel-uv: boot guard not written ({e}); applying anyway"),
                },
                Guard::SameBoot => {}
            }
        }
    }
    eprintln!("centurion-intel-uv: daemon started ({}{})", if cfg.is_some() { "config loaded" } else { "no config yet" },
              if hold.is_some() { ", a scene holds the CPU profile" } else { "" });

    while !STOP.load(Ordering::SeqCst) {
        let m = mtime();
        if RELOAD.swap(false, Ordering::SeqCst) || m != stamp {
            stamp = m;
            match load_boot() {
                Ok(c) => {
                    cfg = c; applied = None; retry.reset(); hold_retry.reset();
                    if quarantined.is_some() && quarantined != fingerprint() {
                        quarantined = None;
                        eprintln!("centurion-intel-uv: boot configuration changed — quarantine lifted");
                    }
                    eprintln!("centurion-intel-uv: configuration reloaded");
                }
                Err(e) => eprintln!("centurion-intel-uv: reload failed, keeping the old config: {e}"),
            }
        }
        let hm = hold_mtime();
        if hm != hold_stamp {
            hold_stamp = hm;
            let was = hold.is_some();
            hold = load_hold();
            hold_retry.reset();
            hold_live = true;  // the helper has just written it to the hardware itself
            match (was, hold.is_some()) {
                (false, true) => eprintln!("centurion-intel-uv: a scene applied its own CPU profile — serving that one"),
                (true, false) => { applied = None; retry.reset(); eprintln!("centurion-intel-uv: scene hold ended — back to the boot configuration"); }
                _ => {}
            }
        }
        let s = suspended_s();
        if s - slept > 1.0 { eprintln!("centurion-intel-uv: resume detected, re-applying"); applied = None; hold_live = false; why = "resume"; retry.reset(); hold_retry.reset(); }
        slept = s;

        let interval = cfg.as_ref().map(|c| c.interval_ms).unwrap_or(5000);
        let reapply = cfg.as_ref().map(|c| c.reapply).unwrap_or(true);
        let ac = source.poll();
        if last_ac != Some(ac) { last_ac = Some(ac); retry.reset(); }
        if guard_settle.is_some_and(|t| Instant::now() >= t) { let _ = std::fs::remove_file(GUARD_FILE); guard_settle = None; }
        if let Some(held) = &hold {
            // The scene engine switches scenes with the power source; the daemon only keeps the scene's profile alive.
            if let Some(p) = held {
                if !hold_live {
                    if hold_retry.ready() {
                        let result = intel_uv::apply_atomic(p);
                        report("SCENE", &result, true);
                        record_transition(&mut journal, if why == "resume" { "resume" } else { "scene" }, if ac { "AC" } else { "BATTERY" }, &result);
                        why = "scene";
                        if fully_applied(&result) { hold_live = true; hold_retry.reset(); }
                        else if hold_retry.failed(&result) {
                            eprintln!("centurion-intel-uv: the scene's profile was not applied — not retrying until a resume or a new scene");
                        }
                    }
                } else if reapply && !hold_retry.exhausted {
                    let part = intel_uv::periodic_part(p);
                    if intel_uv::has_periodic(&part) {
                        report("SCENE", &intel_uv::apply(&part), true);
                    }
                }
                for l in hwp.update(&p.hwphint) { eprintln!("centurion-intel-uv: hwphint: {l}"); }
            }
            // A scene reset (None) needs no full profile write; a failed scene apply is retried a few
            // times (each one rolled back), then left.
            if held.is_none() { hold_live = true; }
        } else if let (Some(c), None) = (&cfg, &quarantined) {
            let src = if ac { "AC" } else { "BATTERY" };
            match c.for_source(ac) {
                Some(_) if applied != Some(ac) && retry.ready() => {
                    if applied.is_some() { eprintln!("centurion-intel-uv: power source → {src}"); if why != "resume" { why = "power source"; } }
                    if let Some(full) = c.full_apply_with(ac, &intel_uv::stock_load()) {
                        // All or nothing: a step that fails puts back the ones already written.
                        let result = intel_uv::apply_atomic(&full);
                        report(src, &result, true);
                        record_transition(&mut journal, why, src, &result);
                        why = "power source";
                        if fully_applied(&result) { applied = Some(ac); retry.reset(); }
                        else if retry.failed(&result) {
                            eprintln!("centurion-intel-uv: {src}: profile not applied — not retrying until the configuration, power source or a resume changes");
                        }
                    } else { applied = Some(ac); }
                }
                // Waiting for the next try, or given up: no periodic writes of a profile that is not in force.
                Some(_) if applied != Some(ac) || retry.exhausted => {}
                Some(p) => {
                    if c.reapply {
                        let part = intel_uv::periodic_part(p);
                        if intel_uv::has_periodic(&part) {
                            report(src, &intel_uv::apply(&part), true);
                        }
                    }
                    for l in hwp.update(&p.hwphint) { eprintln!("centurion-intel-uv: hwphint: {l}"); }
                }
                None => {
                    if applied != Some(ac) && retry.ready() {
                        match c.full_apply_with(ac, &intel_uv::stock_load()) {
                            // The other source's voltage planes back to 0 mV, its power/TCC/BD PROCHOT/IccMax to firmware's.
                            Some(reset) => {
                                eprintln!("centurion-intel-uv: no profile for {src} — back to the firmware's values");
                                let result = intel_uv::apply_atomic(&reset);
                                report(src, &result, true);
                                if fully_applied(&result) { applied = Some(ac); retry.reset(); }
                                else { retry.failed(&result); }
                            }
                            None => {
                                eprintln!("centurion-intel-uv: no profile for {src}, nothing applied");
                                applied = Some(ac);
                            }
                        }
                    }
                }
            }
        }
        // Temperature alarm, after the re-apply above so that its lower limits are the ones that stay.
        // The saved limits are written back capped by the profile in force *now* (config reload, source
        // change or scene hold while the alarm was on), so the restore never goes above either.
        let active = match &hold {
            Some(h) if hold_live && !hold_retry.exhausted => h.as_ref(),
            None if quarantined.is_none() && applied == Some(ac) && !retry.exhausted =>
                cfg.as_ref().and_then(|c| c.for_source(ac)),
            _ => None,
        };
        let caps = (active.and_then(|p| p.pl1.as_ref()).map(|t| t.watts), active.and_then(|p| p.pl2.as_ref()).map(|t| t.watts));
        match cfg.as_ref().filter(|_| quarantined.is_none()).and_then(|c| c.alarm.clone()) {
            Some(a) => {
                // An unreadable temperature changes nothing: an active alarm stays active (and enforced).
                match intel_uv::package_temp().and_then(|t| alarm_transition(alarm_saved.is_some(), t, &a).map(|on| (t, on))) {
                    Some((t, true)) => if let Some((raw, ..)) = intel_uv::live_pl() {
                        eprintln!("centurion-intel-uv: alarm: package {t} °C ≥ {} °C — lowering PL1/PL2", a.temp_c);
                        alarm_saved = Some(raw);
                    },
                    Some((t, false)) => if let Some(raw) = alarm_saved.take() {
                        eprintln!("centurion-intel-uv: alarm cleared at {t} °C — power limits restored");
                        if let Err(e) = intel_uv::restore_pl(raw, caps) { eprintln!("centurion-intel-uv: alarm restore: {e}"); }
                    },
                    None => {}
                }
                // Every tick while active: the periodic re-apply or the EC may have raised them again.
                if let (true, Some((_, l1, l2))) = (alarm_saved.is_some(), intel_uv::live_pl()) {
                    let (p1, p2) = alarm_limits((l1, l2), &a);
                    // Only terms that are set (≥ 1 W) and above the alarm value are written.
                    let req = |w: f64, live: f64| (w >= 1.0 && w < live).then_some(intel_uv::PlReq { watts: w, seconds: None, clamp: None });
                    let p = Profile { pl1: req(p1, l1), pl2: req(p2, l2), ..Default::default() };
                    if p.pl1.is_some() || p.pl2.is_some() { report("ALARM", &intel_uv::apply(&p), true); }
                }
            }
            None => {
                // Alarm removed from the config while active: give the limits back.
                if let Some(raw) = alarm_saved.take() { let _ = intel_uv::restore_pl(raw, caps); }
            }
        }
        // A power-source change waiting out its debounce: look again as soon as it can be committed.
        let nap = source.pending().map_or(interval, |d| (d.as_millis() as u64 + 50).min(interval));
        let nap = guard_settle.map_or(nap, |t| nap.min(t.saturating_duration_since(Instant::now()).as_millis() as u64 + 50));
        wait(rd, nap);
    }
    eprintln!("centurion-intel-uv: daemon stopped");
    0
}

/// One-shot boot/resume apply for the source that is active now.
pub fn apply_boot() -> Value {
    if let Some(reason) = crate::refuse_during_calibration() { return reason; }
    if let Some(reason) = crate::bootguard::should_skip() {
        return json!({"ok": true, "message": format!("boot profile skipped: {reason}")});
    }
    if let (Some((boot, config)), Some(current)) = (read_guard(), fingerprint()) {
        if guard_verdict(Some((&boot, &config)), &crate::bootguard::boot_id(), &current) == Guard::Quarantine {
            return json!({"ok": true, "message": format!("boot profile quarantined; change {BOOT_FILE} or remove {GUARD_FILE}")});
        }
    }
    match load_boot() {
        Ok(None) => json!({"ok": true, "message": "no boot profile set"}),
        Err(e) => json!({"ok": false, "error": e}),
        Ok(Some(c)) => match c.full_apply_with(on_ac(), &intel_uv::stock_load()) {
            Some(p) => intel_uv::apply_atomic(&p),
            None => json!({"ok": true, "message": format!("no profile for {}", if on_ac() { "AC" } else { "battery" })}),
        },
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn transition_entry_marks_firmware_locks() {
        let v = serde_json::json!({"ok": true, "results": [
            {"what": "core", "ok": true, "message": "-80 mV"},
            {"what": "pl1", "ok": false, "message": "MSR_PKG_POWER_LIMIT is locked by firmware (bit 63)"},
            {"what": "cache", "ok": false, "message": "readback -50 mV ≠ requested -80 mV"}]});
        let e = super::transition_entry(7, "resume", "BATTERY", &v);
        assert_eq!(e["event"], "resume");
        assert_eq!(e["ok"], false);
        assert_eq!(e["fields"][0]["locked"], false);
        assert_eq!(e["fields"][1]["locked"], true);
        assert_eq!(e["fields"][2]["locked"], false);
        let bad = super::transition_entry(1, "boot", "AC", &serde_json::json!({"ok": false, "error": "not an Intel CPU"}));
        assert_eq!(bad["error"], "not an Intel CPU");
        assert_eq!(bad["fields"].as_array().unwrap().len(), 0);
    }

    use super::*;

    #[test]
    fn full_profile_success_requires_every_step() {
        assert!(fully_applied(&json!({"ok": true, "results": [{"ok": true}]})));
        assert!(!fully_applied(&json!({"ok": true, "results": [{"ok": false}]})));
        assert!(!fully_applied(&json!({"ok": false, "results": [{"ok": true}]})));
        assert!(!fully_applied(&json!({"ok": true})));
    }

    #[test]
    fn source_switch_zeroes_foreign_planes() {
        let c = parse_boot(&json!({"ac": {"voltage": {"core": -80, "cache": -80}}, "battery": {"voltage": {"core": -50}}})).unwrap();
        let bat = c.full_apply(false).unwrap();
        let mv = |p: &Profile, k: &str| p.voltage.iter().find(|v| v.0 == k).map(|v| v.2);
        assert_eq!(mv(&bat, "core"), Some(-50.0));
        assert_eq!(mv(&bat, "cache"), Some(0.0));   // AC's cache offset is undone
        assert_eq!(mv(&bat, "gpu"), None);          // never touched by either: left alone
        let only_ac = parse_boot(&json!({"ac": {"voltage": {"core": -80}}, "battery": null})).unwrap();
        let reset = only_ac.full_apply(false).unwrap();
        assert_eq!(mv(&reset, "core"), Some(0.0));
        assert!(reset.pl1.is_none() && reset.tjoffset.is_none());
        let none = parse_boot(&json!({"ac": {"power": {"pl1": {"watts": 45}}}, "battery": null})).unwrap();
        assert!(none.full_apply(false).is_none());  // nothing to undo
    }
    #[test]
    fn source_switch_restores_firmware_power_fields() {
        let c = parse_boot(&json!({"ac": {"power": {"pl1": {"watts": 90}, "pl2": {"watts": 120}}, "tjoffset": 5, "disable_bdprochot": true,
                                        "iccmax": {"core": 150}, "voltage": {"core": -50}},
                                   "battery": {"voltage": {"core": -40}}})).unwrap();
        let stock = json!({"pl1": {"watts": 28.0, "seconds": 28.0, "clamp": true}, "pl2": {"watts": 64.0, "seconds": 0.0025, "clamp": false},
                           "tcc": 0, "bdprochot_disabled": false, "icc": {"core": 120.0}});
        let bat = c.full_apply_with(false, &stock).unwrap();
        assert_eq!(bat.pl1.as_ref().map(|t| t.watts), Some(28.0));
        assert_eq!(bat.pl2.as_ref().map(|t| t.watts), Some(64.0));
        assert_eq!((bat.tjoffset, bat.disable_bdprochot), (Some(0), Some(false)));
        assert_eq!(bat.iccmax.iter().map(|i| (i.0, i.2)).collect::<Vec<_>>(), vec![("core", 120.0)]);
        // The battery profile's own value wins; without a recorded stock nothing is invented.
        let own = parse_boot(&json!({"ac": {"power": {"pl1": {"watts": 90}}}, "battery": {"power": {"pl1": {"watts": 15}}}})).unwrap();
        assert_eq!(own.full_apply_with(false, &stock).unwrap().pl1.map(|t| t.watts), Some(15.0));
        assert!(c.full_apply_with(false, &json!({})).unwrap().pl1.is_none());
        // A locked power limit cannot be written again.
        let locked = parse_boot(&json!({"ac": {"power": {"pl1": {"watts": 90}}, "lock_power": true}, "battery": null})).unwrap();
        assert!(locked.ac.as_ref().is_some_and(|p| p.lock_power));
        assert!(locked.full_apply_with(false, &stock).is_none());
    }

    #[test]
    fn boot_guard_verdicts() {
        assert_eq!(guard_verdict(None, "b2", "c"), Guard::Fresh);
        assert_eq!(guard_verdict(Some(("b2", "c")), "b2", "c"), Guard::SameBoot);   // daemon restarted in the same boot
        assert_eq!(guard_verdict(Some(("b1", "c")), "b2", "c"), Guard::Quarantine); // earlier boot never finished with this config
        assert_eq!(guard_verdict(Some(("b1", "old")), "b2", "new"), Guard::Fresh);   // the config was changed since
    }

    #[test]
    fn retry_gives_up_after_three() {
        let mut r = Retry::default();
        assert!(r.ready());
        assert!(!r.failed(&json!({"state": "original"})) && !r.ready());
        r.at = None;
        assert!(!r.failed(&json!({"state": "original"})));
        r.at = None;
        assert!(r.failed(&json!({"state": "original"})) && r.exhausted);
        r.reset();
        assert!(r.failed(&json!({"state": "unknown"})) && !r.ready());
        r.reset();
        assert!(r.ready() && !r.exhausted);
    }

    #[test]
    fn alarm_parse_and_hysteresis() {
        let c = parse_boot(&json!({"voltage": {"core": -50}, "daemon": {"alarm": {"temp_c": 92, "pl1_w": 25, "pl2_w": 35}}})).unwrap();
        let a = c.alarm.unwrap();
        assert_eq!((a.temp_c, a.pl1_w, a.pl2_w), (92, 25.0, 35.0));
        assert_eq!(alarm_transition(false, 91, &a), None);
        assert_eq!(alarm_transition(false, 92, &a), Some(true));
        assert_eq!(alarm_transition(true, 83, &a), None);          // still within the 10 °C band
        assert_eq!(alarm_transition(true, 82, &a), Some(false));
        assert!(parse_boot(&json!({"voltage": {"core": -50}})).unwrap().alarm.is_none());
        for bad in [json!({"temp_c": 59, "pl1_w": 25, "pl2_w": 35}), json!({"temp_c": 106, "pl1_w": 25, "pl2_w": 35}),
                    json!({"temp_c": 90, "pl1_w": 0, "pl2_w": 35}), json!({"temp_c": 90, "pl1_w": 40, "pl2_w": 35}),
                    json!({"temp_c": 90, "pl1_w": 25})] {
            assert!(parse_boot(&json!({"voltage": {"core": -50}, "daemon": {"alarm": bad}})).is_err(), "{bad}");
        }
    }
    #[test]
    fn alarm_never_raises() {
        let a = Alarm { temp_c: 90, pl1_w: 25.0, pl2_w: 35.0 };
        assert_eq!(alarm_limits((45.0, 90.0), &a), (25.0, 35.0));
        assert_eq!(alarm_limits((20.0, 30.0), &a), (20.0, 30.0));  // already lower: kept
        assert_eq!(alarm_limits((20.0, 90.0), &a), (20.0, 35.0));
    }
    #[test]
    fn boot_formats() {
        let legacy = parse_boot(&json!({"voltage": {"core": -50, "cache": -50}})).unwrap();
        assert!(legacy.ac.is_some() && legacy.battery.is_some() && legacy.reapply && legacy.interval_ms == 5000);
        let split = parse_boot(&json!({"ac": {"voltage": {"core": -50}}, "battery": null, "daemon": {"interval_ms": 2000, "reapply": false}})).unwrap();
        assert!(split.ac.is_some() && split.battery.is_none() && !split.reapply && split.interval_ms == 2000);
        assert!(parse_boot(&json!({"ac": {"voltage": {"core": 5}}})).is_err());
        assert!(parse_boot(&json!({"ac": null, "daemon": {"interval_ms": 10}})).is_err());
    }
}
