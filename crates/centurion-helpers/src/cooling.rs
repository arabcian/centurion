//! Cooling health: does the laptop still cool as well as it used to?
//!
//! `centurion-cooltest run` puts a fixed CPU load on every thread for a few minutes and records
//! what the cooling does with it: steady die temperature, fan speed, average clock. The first
//! clean run under a given set of firmware settings becomes that setting's *baseline*; later runs
//! are compared with it. A heat sink clogged with dust or dried-out paste shows up as the same
//! work needing a hotter die, louder fans, or - when the firmware holds the die at its own
//! temperature target - lower clocks.
//!
//! The firmware attributes are part of the question, not just the context:
//!  * PL1 / PL2 / PL3 and the temperature target (`cpu_temp`) decide how much heat the load makes
//!    and where the CPU throttles, so runs are only ever compared with a baseline taken under the
//!    same values (`Context::key`). Change a limit and the next run starts a new baseline.
//!  * With `cpu_temp` the die sits at that target whenever the cooling cannot keep up, so the
//!    temperature cannot rise any further. A "pinned" die is judged by the clock it still gets.
//!
//! No ambient sensor exists, so a warmer room or a hot start is caught through the idle
//! temperature the run began at, and other programs using the CPU mark a run *noisy*. Both give an
//! inconclusive verdict instead of a false alarm.
//!
//! This module is the pure part (summary, comparison, store); the sampling lives in the binary.
//! Store: `$XDG_STATE_HOME/centurion/cooling.json` (default `~/.local/state/centurion`).

use serde_json::{json, Map, Value};
use std::collections::BTreeMap;

/// Bump when the load changes: runs made with another load are not comparable.
pub const LOAD_REV: u32 = 1;
/// Steady state = the last this many seconds of a run (at most half of it).
pub const STEADY_WINDOW_S: f64 = 90.0;
pub const MAX_RUNS: usize = 60;
/// Firmware attributes recorded in the context (read through sysfs).
pub const FW_KEYS: [&str; 4] = ["ppt_pl1_spl", "ppt_pl2_sppt", "ppt_pl3_fppt", "cpu_temp"];
/// Die temperature target assumed when the firmware has no `cpu_temp` attribute.
pub const DEFAULT_LIMIT_C: f64 = 95.0;

// Thresholds of `assess` (°C, percent).
const WARM_START_C: f64 = 6.0;
const WARN_TEMP_C: f64 = 8.0;
const WATCH_TEMP_C: f64 = 4.0;
const WARN_TEMP_WITH_FAN_C: f64 = 5.0;
const BETTER_TEMP_C: f64 = -4.0;
const WARN_CLOCK_PCT: f64 = 8.0;
const WATCH_CLOCK_PCT: f64 = 5.0;
const WARN_FAN_PCT: f64 = 10.0;
const WATCH_FAN_PCT: f64 = 15.0;
/// A die this close to its target is "pinned" by the firmware limit.
const PINNED_MARGIN_C: f64 = 2.0;
/// Below this a fan reading is not a speed worth comparing.
const MIN_FAN_RPM: f64 = 500.0;
/// Fans further apart than this many points (spread = (fastest - slowest) / fastest) than in the baseline: a watch item.
const WATCH_SPLIT_PTS: f64 = 15.0;
/// A fan whose peak speed in the fan test is this far (percent) under its baseline peak is a warning.
pub const FAN_PEAK_WARN_PCT: f64 = 8.0;

/// One reading during the load.
#[derive(Clone, Debug, PartialEq)]
pub struct Sample {
    pub t: f64,
    pub temp: f64,
    /// Fastest fan.
    pub fan: Option<f64>,
    /// Every fan, in `fans_rpm` order.
    pub fans: Vec<f64>,
    pub mhz: Option<f64>,
}

/// What a run boils down to.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Metrics {
    /// Die temperature when the load started (after the cool-down).
    pub start_temp: f64,
    pub steady_temp: f64,
    pub max_temp: f64,
    /// Fastest fan, mean over the steady window.
    pub fan_rpm: Option<f64>,
    /// How unevenly the fans spin (see `fan_spread`), mean over the steady window.
    pub fan_spread: Option<f64>,
    pub mhz: Option<f64>,
}

fn mean(it: impl Iterator<Item = f64>) -> Option<f64> {
    let (s, n) = it.fold((0.0, 0usize), |(s, n), v| (s + v, n + 1));
    (n > 0).then(|| s / n as f64)
}

/// Mean speed of each fan over the samples (fan order as `fans_rpm`).
pub fn fan_means<'a>(samples: impl IntoIterator<Item = &'a Sample>) -> Vec<f64> {
    let samples: Vec<&Sample> = samples.into_iter().collect();
    let n = samples.iter().map(|s| s.fans.len()).max().unwrap_or(0);
    (0..n).filter_map(|i| mean(samples.iter().filter_map(|s| s.fans.get(i).copied()))).collect()
}

/// (fastest - slowest) / fastest, in percent. None with fewer than two fans or when none reaches
/// `MIN_FAN_RPM`. A fan stopped under load shows as 100.
pub fn fan_spread(means: &[f64]) -> Option<f64> {
    let max = means.iter().copied().fold(f64::MIN, f64::max);
    if means.len() < 2 || max < MIN_FAN_RPM { return None; }
    let min = means.iter().copied().fold(f64::MAX, f64::min);
    Some((max - min) / max * 100.0)
}

/// The steady window of a run: its last STEADY_WINDOW_S seconds (at most half of it).
pub fn steady_window(samples: &[Sample]) -> Vec<&Sample> {
    let (Some(last), Some(first)) = (samples.last().map(|s| s.t), samples.first().map(|s| s.t)) else { return Vec::new() };
    let window = STEADY_WINDOW_S.min((last - first) / 2.0).max(0.0);
    samples.iter().filter(|s| s.t >= last - window).collect()
}

/// Steady-state metrics of a run. None without a sample.
pub fn summarise(samples: &[Sample], start_temp: f64) -> Option<Metrics> {
    let steady = steady_window(samples);
    if steady.is_empty() { return None; }
    Some(Metrics {
        start_temp,
        steady_temp: mean(steady.iter().map(|s| s.temp))?,
        max_temp: samples.iter().map(|s| s.temp).fold(f64::MIN, f64::max),
        fan_rpm: mean(steady.iter().filter_map(|s| s.fan)),
        fan_spread: fan_spread(&fan_means(steady.iter().copied())),
        mhz: mean(steady.iter().filter_map(|s| s.mhz)),
    })
}

/// Everything a run's meaning depends on.
#[derive(Clone, Debug, PartialEq)]
pub struct Context {
    pub profile: Option<String>,
    /// "ac" | "battery" | "?"
    pub power: String,
    pub limits: BTreeMap<String, i64>,
    pub threads: usize,
}

impl Context {
    /// Runs are comparable exactly when their keys are equal.
    pub fn key(&self) -> String {
        let l = |k: &str| self.limits.get(k).map_or("-".to_owned(), |v| v.to_string());
        format!("v{LOAD_REV}|{}|{}|pl1={}|pl2={}|pl3={}|tgt={}|thr={}",
            self.profile.as_deref().unwrap_or("?"), self.power,
            l("ppt_pl1_spl"), l("ppt_pl2_sppt"), l("ppt_pl3_fppt"), l("cpu_temp"), self.threads)
    }
    /// The die temperature the firmware lets the CPU reach.
    pub fn limit_c(&self) -> f64 { self.limits.get("cpu_temp").map_or(DEFAULT_LIMIT_C, |v| *v as f64) }
    /// A run is stopped for safety above this.
    pub fn abort_c(&self) -> f64 { (self.limit_c() + 5.0).min(100.0) }
    pub fn to_json(&self) -> Value {
        let limits: Map<String, Value> = self.limits.iter().map(|(k, v)| (k.clone(), json!(v))).collect();
        json!({"profile": self.profile, "power": self.power, "limits": limits, "threads": self.threads, "load_rev": LOAD_REV})
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Level { Better, Ok, Watch, Warn, Inconclusive }

impl Level {
    pub fn as_str(self) -> &'static str {
        match self { Level::Better => "better", Level::Ok => "ok", Level::Watch => "watch", Level::Warn => "warn", Level::Inconclusive => "inconclusive" }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Assessment {
    pub level: Level,
    /// Machine-readable reasons (the GUI words them): code + the number behind it.
    pub reasons: Vec<(&'static str, f64)>,
    pub d_temp: f64,
    pub d_clock_pct: Option<f64>,
    pub d_fan_pct: Option<f64>,
    /// Change of the fan spread in points (positive = the fans spin further apart than at the baseline).
    pub d_split_pts: Option<f64>,
    pub pinned: bool,
}

/// Compares a run with the baseline taken under the same context key.
/// `limit_c`: the firmware temperature target of that context.
pub fn assess(base: &Metrics, now: &Metrics, limit_c: f64, base_noisy: bool, now_noisy: bool) -> Assessment {
    let d_temp = now.steady_temp - base.steady_temp;
    let pct_down = |b: Option<f64>, n: Option<f64>| match (b, n) { (Some(b), Some(n)) if b > 0.0 => Some((b - n) / b * 100.0), _ => None };
    let d_clock_pct = pct_down(base.mhz, now.mhz);
    let d_fan_pct = match (base.fan_rpm, now.fan_rpm) {
        (Some(b), Some(n)) if b >= MIN_FAN_RPM && n >= MIN_FAN_RPM => pct_down(Some(b), Some(n)).map(|d| -d),
        _ => None,
    };
    let d_split_pts = match (base.fan_spread, now.fan_spread) { (Some(b), Some(n)) => Some(n - b), _ => None };
    let pinned = now.steady_temp >= limit_c - PINNED_MARGIN_C;
    let mut a = Assessment { level: Level::Ok, reasons: Vec::new(), d_temp, d_clock_pct, d_fan_pct, d_split_pts, pinned };

    if base_noisy || now_noisy {
        a.level = Level::Inconclusive;
        a.reasons.push(("noisy", 0.0));
        return a;
    }
    let d_start = now.start_temp - base.start_temp;
    if d_start >= WARM_START_C {
        a.level = Level::Inconclusive;
        a.reasons.push(("warm_start", d_start));
        return a;
    }

    let clock = d_clock_pct.unwrap_or(0.0);
    let fan = d_fan_pct.unwrap_or(0.0);
    let mut warn = false;
    let mut watch = false;
    if d_temp >= WARN_TEMP_C { warn = true; a.reasons.push(("temp_up", d_temp)); }
    else if d_temp >= WARN_TEMP_WITH_FAN_C && fan >= WARN_FAN_PCT { warn = true; a.reasons.push(("temp_up", d_temp)); a.reasons.push(("fan_up", fan)); }
    else if d_temp >= WATCH_TEMP_C { watch = true; a.reasons.push(("temp_up", d_temp)); }
    if pinned && clock >= WARN_CLOCK_PCT { warn = true; a.reasons.push(("pinned", limit_c)); a.reasons.push(("clock_down", clock)); }
    else if clock >= WATCH_CLOCK_PCT { watch = true; a.reasons.push(("clock_down", clock)); }
    if !warn && fan >= WATCH_FAN_PCT { watch = true; a.reasons.push(("fan_up", fan)); }
    // One fan slower than the others (dust in one fan, a blocked duct, a loose cable): a watch item only,
    // since a changed fan curve also moves the spread.
    if !warn && d_split_pts.unwrap_or(0.0) >= WATCH_SPLIT_PTS { watch = true; a.reasons.push(("fan_split", d_split_pts.unwrap_or(0.0))); }

    if warn { a.level = Level::Warn; }
    else if watch { a.level = Level::Watch; }
    else if d_temp <= BETTER_TEMP_C || clock <= -WATCH_CLOCK_PCT {
        a.level = Level::Better;
        a.reasons.push(("better", d_temp));
    }
    a
}

// ── store ────────────────────────────────────────────────────────────────────

/// One stored run: `{"t", "key", "ctx", "start_temp", "steady_temp", "max_temp", "fan_rpm", "fan_spread",
/// "mhz", "noisy", "secs", "baseline"}`. Runs stored before per-fan readings have no "fan_spread".
pub fn run_json(t: i64, ctx: &Context, m: &Metrics, noisy: bool, secs: u64) -> Value {
    json!({"t": t, "key": ctx.key(), "ctx": ctx.to_json(), "start_temp": m.start_temp, "steady_temp": m.steady_temp,
           "max_temp": m.max_temp, "fan_rpm": m.fan_rpm, "fan_spread": m.fan_spread, "mhz": m.mhz, "noisy": noisy, "secs": secs, "baseline": false})
}

pub fn metrics_of(run: &Value) -> Option<Metrics> {
    Some(Metrics {
        start_temp: run["start_temp"].as_f64()?,
        steady_temp: run["steady_temp"].as_f64()?,
        max_temp: run["max_temp"].as_f64()?,
        fan_rpm: run["fan_rpm"].as_f64(),
        fan_spread: run["fan_spread"].as_f64(),
        mhz: run["mhz"].as_f64(),
    })
}

fn is_baseline(r: &Value) -> bool { r["baseline"] == true }
fn usable(r: &Value) -> bool { r["noisy"] != true && metrics_of(r).is_some() }

/// The runs array of a store (empty for anything unexpected).
pub fn runs(store: &Value) -> Vec<Value> { store["runs"].as_array().cloned().unwrap_or_default() }

pub fn baseline_for<'a>(runs: &'a [Value], key: &str) -> Option<&'a Value> {
    runs.iter().find(|r| r["key"] == key && is_baseline(r))
}

/// Appends a run. A clean run under a key that has no baseline becomes it. The list is capped at
/// `MAX_RUNS`, oldest first, but a baseline is never the one dropped.
pub fn add_run(runs: &mut Vec<Value>, mut run: Value) {
    let key = run["key"].as_str().unwrap_or_default().to_owned();
    if usable(&run) && baseline_for(runs, &key).is_none() { run["baseline"] = json!(true); }
    runs.push(run);
    while runs.len() > MAX_RUNS {
        match runs.iter().position(|r| !is_baseline(r)) { Some(i) => { runs.remove(i); } None => break }
    }
}

/// Makes the newest clean run of `key` its baseline (after cleaning the fans, say). False when it has none.
pub fn promote_latest(runs: &mut [Value], key: &str) -> bool {
    let Some(i) = runs.iter().rposition(|r| r["key"] == key && usable(r)) else { return false };
    for r in runs.iter_mut().filter(|r| r["key"] == key) { r["baseline"] = json!(false); }
    runs[i]["baseline"] = json!(true);
    true
}

/// How a stored run's context (`ctx` of `run_json`) differs from `now` (`Context::to_json`):
/// `[{"field", "then", "now"}]` for profile, power, each of FW_KEYS, threads and load_rev.
/// This is why a run is not compared with the current settings.
pub fn context_diff(then: &Value, now: &Value) -> Vec<Value> {
    let mut out = Vec::new();
    let mut cmp = |field: &str, a: &Value, b: &Value| if a != b { out.push(json!({"field": field, "then": a, "now": b})); };
    for f in ["profile", "power"] { cmp(f, &then[f], &now[f]); }
    for k in FW_KEYS { cmp(k, &then["limits"][k], &now["limits"][k]); }
    for f in ["threads", "load_rev"] { cmp(f, &then[f], &now[f]); }
    out
}

/// Drops every run of `key`.
pub fn forget(runs: &mut Vec<Value>, key: &str) { runs.retain(|r| r["key"] != key); }

// ── system access (user level, read-only apart from the store) ─────────────────

use std::path::{Path, PathBuf};

fn read_trim(p: &Path) -> Option<String> { std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned()) }

/// The firmware settings and power state of this moment.
pub fn capture_context() -> Context {
    let mut limits = BTreeMap::new();
    for k in FW_KEYS { if let Some(v) = crate::calctx::fw_read(k) { limits.insert(k.to_owned(), v); } }
    Context {
        profile: crate::calctx::current_profile(),
        power: match crate::power::on_ac() { Some(true) => "ac", Some(false) => "battery", None => "?" }.to_owned(),
        limits,
        threads: std::thread::available_parallelism().map_or(1, |n| n.get()),
    }
}

/// Every fan the hwmon tree reports, in a stable order (chip name, index).
pub fn fans_rpm() -> Vec<f64> {
    let mut v: Vec<(String, u32, f64)> = Vec::new();
    for e in std::fs::read_dir("/sys/class/hwmon").into_iter().flatten().flatten() {
        let d = e.path();
        let name = read_trim(&d.join("name")).unwrap_or_default();
        for i in 1..=8u32 {
            if let Some(rpm) = read_trim(&d.join(format!("fan{i}_input"))).and_then(|s| s.parse::<f64>().ok()) { v.push((name.clone(), i, rpm)); }
        }
    }
    v.sort_by(|a, b| (&a.0, a.1).cmp(&(&b.0, b.1)));
    v.into_iter().map(|x| x.2).collect()
}

/// Mean current clock over the online CPUs, MHz.
pub fn avg_mhz() -> Option<f64> {
    let khz: Vec<f64> = crate::tune::cpus().into_iter()
        .filter_map(|(_, d)| read_trim(&d.join("cpufreq/scaling_cur_freq")).and_then(|s| s.parse::<f64>().ok())).collect();
    mean(khz.into_iter()).map(|k| k / 1000.0)
}

fn state_dir() -> PathBuf {
    let base = std::env::var_os("XDG_STATE_HOME").filter(|v| !v.is_empty()).map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").filter(|v| !v.is_empty()).map(|h| PathBuf::from(h).join(".local/state")))
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    base.join("centurion")
}

pub fn store_path() -> PathBuf { state_dir().join("cooling.json") }

/// Fan-test baselines: `{key: {"t": epoch, "peaks": [peak rpm per fan]}}`, one entry per firmware context.
pub fn fanmax_path() -> PathBuf { state_dir().join("fanmax.json") }

/// Peak speeds of the fans under `key` (None when the fan test has no baseline for it).
pub fn fan_peak_baseline(store: &Value, key: &str) -> Option<Vec<f64>> {
    Some(store[key]["peaks"].as_array()?.iter().filter_map(Value::as_f64).collect())
}

/// Compares fan peaks with their baseline, fan by fan. `drops_pct` is how far each peak sits below its
/// baseline (negative = faster). None when the fan counts differ or no baseline fan reaches `MIN_FAN_RPM`.
#[derive(Clone, Debug, PartialEq)]
pub struct PeakCheck { pub drops_pct: Vec<f64>, pub warn: bool }

pub fn check_fan_peaks(base: &[f64], now: &[f64]) -> Option<PeakCheck> {
    if base.len() != now.len() || !base.iter().any(|b| *b >= MIN_FAN_RPM) { return None; }
    let drops_pct: Vec<f64> = base.iter().zip(now).map(|(b, n)| if *b >= MIN_FAN_RPM { (b - n) / b * 100.0 } else { 0.0 }).collect();
    let warn = drops_pct.iter().any(|d| *d >= FAN_PEAK_WARN_PCT);
    Some(PeakCheck { drops_pct, warn })
}

/// The stored runs (empty when there is no readable store).
pub fn load_runs(path: &Path) -> Vec<Value> {
    match std::fs::metadata(path) { Ok(m) if m.len() < (4 << 20) => {} _ => return Vec::new() }
    std::fs::read_to_string(path).ok().and_then(|s| serde_json::from_str::<Value>(&s).ok()).map(|v| runs(&v)).unwrap_or_default()
}

/// Writes a store file in one step (temp file + rename), so a reader never sees half of it.
fn write_atomic(path: &Path, v: &Value) -> Result<(), String> {
    let dir = path.parent().ok_or("bad store path")?;
    std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let tmp = dir.join(format!(".{}.{}", path.file_name().unwrap_or_default().to_string_lossy(), std::process::id()));
    std::fs::write(&tmp, serde_json::to_vec(v).map_err(|e| e.to_string())?)
        .and_then(|_| std::fs::rename(&tmp, path))
        .map_err(|e| { let _ = std::fs::remove_file(&tmp); format!("{}: {e}", path.display()) })
}

pub fn save_runs(path: &Path, runs: &[Value]) -> Result<(), String> {
    write_atomic(path, &json!({"version": 1, "runs": runs}))
}

/// The fan-test store (empty when there is none or it is unreadable).
pub fn load_fanmax(path: &Path) -> Value {
    std::fs::read_to_string(path).ok().and_then(|s| serde_json::from_str::<Value>(&s).ok()).filter(Value::is_object).unwrap_or_else(|| json!({}))
}

pub fn save_fanmax(path: &Path, store: &Value) -> Result<(), String> { write_atomic(path, store) }

/// The newest run of `key` and its verdict against that key's baseline (None without a baseline or
/// when the newest run is the baseline itself).
pub fn latest_assessment(runs: &[Value], key: &str, limit_c: f64) -> Option<(Value, Assessment)> {
    let base = baseline_for(runs, key)?;
    let last = runs.iter().rev().find(|r| r["key"] == key)?;
    if is_baseline(last) { return None; }
    let a = assess(&metrics_of(base)?, &metrics_of(last)?, limit_c, base["noisy"] == true, last["noisy"] == true);
    Some((last.clone(), a))
}

pub fn assessment_json(a: &Assessment) -> Value {
    json!({"level": a.level.as_str(), "d_temp": a.d_temp, "d_clock_pct": a.d_clock_pct, "d_fan_pct": a.d_fan_pct, "d_split_pts": a.d_split_pts, "pinned": a.pinned,
           "reasons": a.reasons.iter().map(|(c, v)| json!({"code": c, "value": v})).collect::<Vec<_>>()})
}

#[cfg(test)]
mod tests {
    use super::*;

    fn m(start: f64, temp: f64, fan: Option<f64>, mhz: Option<f64>) -> Metrics {
        Metrics { start_temp: start, steady_temp: temp, max_temp: temp + 1.0, fan_rpm: fan, fan_spread: None, mhz }
    }
    fn ctx(limit: Option<i64>) -> Context {
        let mut limits = BTreeMap::new();
        limits.insert("ppt_pl1_spl".to_owned(), 80);
        if let Some(l) = limit { limits.insert("cpu_temp".to_owned(), l); }
        Context { profile: Some("custom".into()), power: "ac".into(), limits, threads: 32 }
    }

    #[test]
    fn steady_window_is_the_end_of_the_run() {
        // 0..240 s: 60 °C for the first 150 s, then 80 °C.
        let s: Vec<Sample> = (0..=120).map(|i| { let t = i as f64 * 2.0; Sample { t, temp: if t < 150.0 { 60.0 } else { 80.0 }, fan: Some(3000.0), fans: vec![3000.0], mhz: None } }).collect();
        let r = summarise(&s, 45.0).unwrap();
        assert_eq!(r.steady_temp, 80.0);
        assert_eq!(r.max_temp, 80.0);
        assert_eq!(r.fan_rpm, Some(3000.0));
        assert_eq!(r.mhz, None);
        assert!(summarise(&[], 45.0).is_none());
    }

    #[test]
    fn key_follows_the_firmware_settings() {
        let a = ctx(Some(95));
        let mut b = a.clone();
        assert_eq!(a.key(), b.key());
        b.limits.insert("ppt_pl1_spl".into(), 110);
        assert_ne!(a.key(), b.key());
        b = a.clone();
        b.limits.insert("cpu_temp".into(), 85);
        assert_ne!(a.key(), b.key());
        b = a.clone();
        b.power = "battery".into();
        assert_ne!(a.key(), b.key());
        assert_eq!(ctx(None).limit_c(), DEFAULT_LIMIT_C);
        assert_eq!(ctx(Some(100)).abort_c(), 100.0);
        assert_eq!(ctx(Some(85)).abort_c(), 90.0);
    }

    #[test]
    fn hotter_die_is_flagged() {
        let base = m(45.0, 78.0, Some(3200.0), Some(4200.0));
        assert_eq!(assess(&base, &m(45.0, 79.0, Some(3200.0), Some(4200.0)), 95.0, false, false).level, Level::Ok);
        assert_eq!(assess(&base, &m(45.0, 82.5, Some(3200.0), Some(4200.0)), 95.0, false, false).level, Level::Watch);
        assert_eq!(assess(&base, &m(45.0, 87.0, Some(3200.0), Some(4200.0)), 95.0, false, false).level, Level::Warn);
        // Warmer AND louder is enough for a warning earlier.
        let a = assess(&base, &m(45.0, 83.5, Some(3700.0), Some(4200.0)), 95.0, false, false);
        assert_eq!(a.level, Level::Warn);
        assert!(a.reasons.iter().any(|r| r.0 == "fan_up"));
    }

    #[test]
    fn pinned_die_is_judged_by_its_clock() {
        // cpu_temp = 85: the die sits at the target in both runs; only the clock tells.
        let base = m(45.0, 84.5, Some(4800.0), Some(4300.0));
        let now = m(45.0, 84.8, Some(5200.0), Some(3800.0));
        let a = assess(&base, &now, 85.0, false, false);
        assert!(a.pinned);
        assert_eq!(a.level, Level::Warn);
        assert!(a.reasons.iter().any(|r| r.0 == "pinned") && a.reasons.iter().any(|r| r.0 == "clock_down"));
        // The same clock loss on a die far below its target is only a watch item.
        let b = assess(&m(45.0, 70.0, None, Some(4300.0)), &m(45.0, 70.0, None, Some(3800.0)), 95.0, false, false);
        assert_eq!(b.level, Level::Watch);
    }

    #[test]
    fn warm_start_and_noise_are_not_verdicts() {
        let base = m(45.0, 78.0, Some(3200.0), Some(4200.0));
        let hot = m(52.0, 90.0, Some(4000.0), Some(3000.0));
        let a = assess(&base, &hot, 95.0, false, false);
        assert_eq!(a.level, Level::Inconclusive);
        assert_eq!(a.reasons[0].0, "warm_start");
        assert_eq!(assess(&base, &hot, 95.0, false, true).level, Level::Inconclusive);
        assert_eq!(assess(&base, &hot, 95.0, true, false).level, Level::Inconclusive);
    }

    #[test]
    fn fan_spread_measures_the_disagreement() {
        assert_eq!(fan_spread(&[3000.0, 3000.0]), Some(0.0));
        assert_eq!(fan_spread(&[3000.0, 1500.0]), Some(50.0));
        assert_eq!(fan_spread(&[3000.0, 0.0]), Some(100.0));
        assert_eq!(fan_spread(&[3000.0]), None);
        assert_eq!(fan_spread(&[400.0, 300.0]), None);
    }

    #[test]
    fn per_fan_means_come_from_the_steady_window() {
        let s = |t: f64, fans: Vec<f64>| Sample { t, temp: 80.0, fan: fans.iter().copied().reduce(f64::max), fans, mhz: None };
        let samples = vec![s(0.0, vec![3000.0, 2000.0]), s(2.0, vec![3200.0, 2200.0])];
        assert_eq!(fan_means(&samples), vec![3100.0, 2100.0]);
        // Two samples: the steady window is the last one only.
        let r = summarise(&samples, 45.0).unwrap();
        assert!((r.fan_spread.unwrap() - 31.25).abs() < 1e-9);
    }

    #[test]
    fn one_fan_slower_than_the_others_is_a_watch_item() {
        let base = Metrics { fan_spread: Some(2.0), ..m(45.0, 78.0, Some(3200.0), Some(4200.0)) };
        let even = Metrics { fan_spread: Some(4.0), ..m(45.0, 78.5, Some(3200.0), Some(4200.0)) };
        assert_eq!(assess(&base, &even, 95.0, false, false).level, Level::Ok);
        let uneven = Metrics { fan_spread: Some(30.0), ..m(45.0, 78.5, Some(3200.0), Some(4200.0)) };
        let a = assess(&base, &uneven, 95.0, false, false);
        assert_eq!(a.level, Level::Watch);
        assert_eq!(a.d_split_pts, Some(28.0));
        assert!(a.reasons.iter().any(|r| r.0 == "fan_split" && (r.1 - 28.0).abs() < 1e-9));
        // A baseline from before per-fan readings cannot be compared.
        let old = Metrics { fan_spread: None, ..m(45.0, 78.0, Some(3200.0), Some(4200.0)) };
        assert_eq!(assess(&old, &uneven, 95.0, false, false).d_split_pts, None);
        // The spread survives the store.
        let run = run_json(1, &ctx(Some(95)), &even, false, 240);
        assert_eq!(metrics_of(&run).unwrap().fan_spread, Some(4.0));
    }

    #[test]
    fn fan_peaks_eight_percent_under_the_baseline_warn() {
        let c = check_fan_peaks(&[4000.0, 4000.0], &[3600.0, 3900.0]).unwrap();
        assert!(c.warn);
        assert!((c.drops_pct[0] - 10.0).abs() < 1e-9 && (c.drops_pct[1] - 2.5).abs() < 1e-9);
        assert!(!check_fan_peaks(&[4000.0], &[3800.0]).unwrap().warn); // 5 %
        assert!(check_fan_peaks(&[4000.0], &[4000.0, 4000.0]).is_none()); // fan count changed
        assert!(check_fan_peaks(&[300.0], &[200.0]).is_none()); // nothing comparable
    }

    #[test]
    fn fan_peak_baseline_is_kept_per_key() {
        let store = json!({"ctx-a": {"t": 1, "peaks": [4000.0, 3900.0]}});
        assert_eq!(fan_peak_baseline(&store, "ctx-a"), Some(vec![4000.0, 3900.0]));
        assert_eq!(fan_peak_baseline(&store, "ctx-b"), None);
    }

    #[test]
    fn fan_test_store_round_trip() {
        let dir = std::env::temp_dir().join(format!("fanmax-test-{}", std::process::id()));
        let file = dir.join("nested/fanmax.json");
        assert!(load_fanmax(&file).as_object().unwrap().is_empty());
        let store = json!({"ctx-a": {"t": 5, "peaks": [4000.0, 3900.0]}});
        save_fanmax(&file, &store).unwrap();
        assert_eq!(fan_peak_baseline(&load_fanmax(&file), "ctx-a"), Some(vec![4000.0, 3900.0]));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn cleaning_shows_as_better() {
        let base = m(45.0, 88.0, Some(4800.0), Some(3900.0));
        let a = assess(&base, &m(45.0, 79.0, Some(3300.0), Some(4300.0)), 95.0, false, false);
        assert_eq!(a.level, Level::Better);
    }

    #[test]
    fn store_baseline_rules() {
        let c = ctx(Some(95));
        let mut runs = Vec::new();
        let good = |t: i64, temp: f64, noisy: bool| run_json(t, &c, &m(45.0, temp, Some(3000.0), Some(4000.0)), noisy, 240);
        add_run(&mut runs, good(1, 90.0, true));  // noisy: never a baseline
        assert!(baseline_for(&runs, &c.key()).is_none());
        add_run(&mut runs, good(2, 80.0, false)); // first clean run
        add_run(&mut runs, good(3, 85.0, false));
        assert_eq!(baseline_for(&runs, &c.key()).unwrap()["t"], 2);
        assert!(promote_latest(&mut runs, &c.key()));
        assert_eq!(baseline_for(&runs, &c.key()).unwrap()["t"], 3);
        assert_eq!(runs.iter().filter(|r| is_baseline(r)).count(), 1);
        // Another firmware setting has its own baseline.
        let mut other = c.clone();
        other.limits.insert("ppt_pl1_spl".into(), 120);
        add_run(&mut runs, run_json(4, &other, &m(45.0, 90.0, None, None), false, 240));
        assert_eq!(baseline_for(&runs, &other.key()).unwrap()["t"], 4);
        assert_eq!(baseline_for(&runs, &c.key()).unwrap()["t"], 3);
        forget(&mut runs, &c.key());
        assert!(runs.iter().all(|r| r["key"] != c.key().as_str()));
        assert!(!promote_latest(&mut runs, &c.key()));
    }

    #[test]
    fn context_diff_names_what_changed() {
        let (a, mut b) = (ctx(Some(95)), ctx(Some(89)));
        assert!(context_diff(&a.to_json(), &a.to_json()).is_empty());
        b.threads = 16;
        let d = context_diff(&a.to_json(), &b.to_json());
        assert_eq!(d.iter().map(|x| x["field"].as_str().unwrap()).collect::<Vec<_>>(), ["cpu_temp", "threads"]);
        assert_eq!((d[0]["then"].as_i64(), d[0]["now"].as_i64()), (Some(95), Some(89)));
    }

    #[test]
    fn store_cap_keeps_the_baseline() {
        let c = ctx(Some(95));
        let mut runs = Vec::new();
        for t in 0..(MAX_RUNS as i64 + 20) { add_run(&mut runs, run_json(t, &c, &m(45.0, 80.0, None, None), false, 240)); }
        assert_eq!(runs.len(), MAX_RUNS);
        assert_eq!(baseline_for(&runs, &c.key()).unwrap()["t"], 0);
    }

    #[test]
    fn store_round_trip_and_latest_assessment() {
        let dir = std::env::temp_dir().join(format!("cooling-test-{}", std::process::id()));
        let file = dir.join("nested/cooling.json");
        let c = ctx(Some(95));
        let mut runs = Vec::new();
        add_run(&mut runs, run_json(1, &c, &m(45.0, 78.0, Some(3200.0), Some(4200.0)), false, 240));
        assert!(latest_assessment(&runs, &c.key(), 95.0).is_none()); // only the baseline so far
        add_run(&mut runs, run_json(2, &c, &m(45.0, 88.0, Some(3900.0), Some(4000.0)), false, 240));
        save_runs(&file, &runs).unwrap();
        let back = load_runs(&file);
        assert_eq!(back, runs);
        let (last, a) = latest_assessment(&back, &c.key(), 95.0).unwrap();
        assert_eq!(last["t"], 2);
        assert_eq!(a.level, Level::Warn);
        assert!(load_runs(&dir.join("missing.json")).is_empty());
        std::fs::write(&file, b"{not json").unwrap();
        assert!(load_runs(&file).is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
