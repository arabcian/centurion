//! centurion-cooltest - cooling health test. Runs as the user (never through pkexec).
//!
//!   centurion-cooltest status               {"ok", "context", "key", "limit_c", "baseline", "runs", "latest", "assessment"}
//!   centurion-cooltest run [--seconds N]    JSON lines: {"phase", ...} progress, then {"ok", "run", "assessment"}
//!   centurion-cooltest baseline             make the newest clean run of the current settings the baseline
//!   centurion-cooltest forget               drop every stored run and fan-test result of the current settings
//!   centurion-cooltest fans                 {"ok", "fans": [rpm per fan]} - a read of the fans now
//!   centurion-cooltest fanmax --peaks A,B   the fan test's peak rpm per fan: the first one under the current
//!                                           settings is the baseline, later ones are compared with it
//!
//! `run` loads every CPU thread with a fixed, light-weight floating-point loop (about four minutes by
//! default), samples die temperature, fans and clock every 2 s, and stores the steady-state result.
//! Nothing is written to the hardware; the load stops when the process does.
//! The fan test itself (fans to maximum, then back) is done by the GUI through the Home page, which
//! has the root rights for it; this tool only reads the fans and judges the peaks it is given.
//! How runs are compared (firmware settings, pinned die, noise): see the `cooling` module.

use centurion_helpers::cooling::{self as cool, Context, Sample};
use serde_json::{json, Value};
use std::io::Write;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const DEFAULT_SECONDS: u64 = 240;
const MIN_SECONDS: u64 = 120;
const MAX_SECONDS: u64 = 900;
const SAMPLE_EVERY: Duration = Duration::from_secs(2);
/// Cool-down before the load: down to this die temperature, at most COOLDOWN_MAX.
const COOLDOWN_TARGET_C: f64 = 50.0;
const COOLDOWN_MAX: Duration = Duration::from_secs(120);
/// Other programs keeping this many CPUs busy over the run make it noisy.
const NOISY_BUSY_CPUS: f64 = 1.0;

fn emit(v: &Value) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{v}");
    let _ = out.flush();
}

fn now_epoch() -> i64 { SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs() as i64) }

/// The same fixed work on every run; changing it means bumping `cooling::LOAD_REV`.
fn burn(stop: &AtomicBool) {
    let mut a = [1.0_f64; 8];
    while !stop.load(Ordering::Relaxed) {
        for _ in 0..20_000 {
            for (k, x) in a.iter_mut().enumerate() { *x = *x * 1.000_000_1 + 1e-7 * (k as f64 + 1.0); }
            if a[0] > 1e6 { a = [1.0; 8]; }
        }
        std::hint::black_box(&a);
    }
}

fn status() -> Value {
    let ctx = cool::capture_context();
    let key = ctx.key();
    let runs = cool::load_runs(&cool::store_path());
    let mine: Vec<&Value> = runs.iter().filter(|r| r["key"] == key.as_str()).collect();
    let mut v = json!({
        "ok": true, "context": ctx.to_json(), "key": key, "limit_c": ctx.limit_c(),
        "has_sensor": centurion_helpers::bench::cpu_temp_c().is_some(),
        "fans": cool::fans_rpm().len(),
        "baseline": cool::baseline_for(&runs, &key), "runs": mine, "total_runs": runs.len(),
    });
    if let Some((last, a)) = cool::latest_assessment(&runs, &key, ctx.limit_c()) {
        v["latest"] = last;
        v["assessment"] = cool::assessment_json(&a);
    }
    v
}

fn run(seconds: u64) -> Value {
    if std::path::Path::new(centurion_helpers::stability::INFLIGHT).exists() {
        return json!({"ok": false, "error": "a calibration run is in progress; try again when it is finished"});
    }
    if centurion_helpers::bench::cpu_temp_c().is_none() {
        return json!({"ok": false, "error": "no CPU temperature sensor found (k10temp / coretemp)"});
    }
    let ctx: Context = cool::capture_context();
    let limit = ctx.limit_c();
    let abort_at = ctx.abort_c();

    // Low priority: the desktop stays usable while every thread is busy.
    unsafe { libc::setpriority(libc::PRIO_PROCESS, 0, 10); }
    let foreign = centurion_helpers::bench::Foreign::snapshot(&[]);
    let t_all = Instant::now();

    emit(&json!({"phase": "cooldown", "total": COOLDOWN_MAX.as_secs()}));
    let (start, _) = centurion_helpers::bench::thermal_gate(COOLDOWN_TARGET_C, COOLDOWN_MAX);
    let start_temp = start.or_else(centurion_helpers::bench::cpu_temp_c).unwrap_or(0.0);

    let stop = AtomicBool::new(false);
    let mut samples: Vec<Sample> = Vec::new();
    let mut aborted: Option<f64> = None;
    std::thread::scope(|s| {
        for _ in 0..ctx.threads { s.spawn(|| burn(&stop)); }
        let t0 = Instant::now();
        let total = Duration::from_secs(seconds);
        while t0.elapsed() < total {
            std::thread::sleep(SAMPLE_EVERY);
            let t = t0.elapsed().as_secs_f64();
            let Some(temp) = centurion_helpers::bench::cpu_temp_c() else { continue };
            let fans = cool::fans_rpm();
            let fan = fans.iter().copied().reduce(f64::max);
            let mhz = cool::avg_mhz();
            samples.push(Sample { t, temp, fan, fans, mhz });
            emit(&json!({"phase": "load", "elapsed": t, "total": seconds, "temp": temp, "fan": fan, "mhz": mhz, "start_temp": start_temp}));
            if temp >= abort_at { aborted = Some(temp); break; }
        }
        stop.store(true, Ordering::Relaxed);
    });

    if let Some(t) = aborted {
        return json!({"ok": false, "error": format!("stopped for safety: the CPU reached {t:.0} °C (the limit for this test is {abort_at:.0} °C, firmware target {limit:.0} °C)")});
    }
    let Some(m) = cool::summarise(&samples, start_temp) else {
        return json!({"ok": false, "error": "no temperature samples were collected"});
    };
    let busy = foreign.busy_cpus(&[], t_all.elapsed().as_secs_f64());
    let noisy = busy >= NOISY_BUSY_CPUS;

    let path = cool::store_path();
    let mut runs = cool::load_runs(&path);
    let mut r = cool::run_json(now_epoch(), &ctx, &m, noisy, seconds);
    r["other_busy_cpus"] = json!((busy * 10.0).round() / 10.0);
    r["fans"] = json!(cool::fan_means(cool::steady_window(&samples)));
    cool::add_run(&mut runs, r.clone());
    if let Err(e) = cool::save_runs(&path, &runs) {
        return json!({"ok": false, "error": format!("the test finished but its result could not be saved: {e}")});
    }
    let stored = runs.last().cloned().unwrap_or(r);
    let mut out = json!({"ok": true, "run": stored, "limit_c": limit});
    if let Some((_, a)) = cool::latest_assessment(&runs, &ctx.key(), limit) { out["assessment"] = cool::assessment_json(&a); }
    out
}

fn edit_store(f: impl FnOnce(&mut Vec<Value>, &str) -> Result<(), String>) -> Value {
    let key = cool::capture_context().key();
    let path = cool::store_path();
    let mut runs = cool::load_runs(&path);
    match f(&mut runs, &key).and_then(|_| cool::save_runs(&path, &runs)) {
        Ok(()) => json!({"ok": true}),
        Err(e) => json!({"ok": false, "error": e}),
    }
}

fn forget() -> Value {
    let key = cool::capture_context().key();
    let (path, fan_path) = (cool::store_path(), cool::fanmax_path());
    let mut runs = cool::load_runs(&path);
    cool::forget(&mut runs, &key);
    let mut peaks = cool::load_fanmax(&fan_path);
    if let Some(o) = peaks.as_object_mut() { o.remove(&key); }
    match cool::save_runs(&path, &runs).and_then(|_| cool::save_fanmax(&fan_path, &peaks)) {
        Ok(()) => json!({"ok": true}),
        Err(e) => json!({"ok": false, "error": e}),
    }
}

fn fans() -> Value { json!({"ok": true, "fans": cool::fans_rpm()}) }

/// The fan test's peaks against the baseline of the current settings (the first test under them becomes it).
fn fanmax(args: &[String]) -> Value {
    let peaks: Result<Vec<f64>, _> = args.iter().position(|a| a == "--peaks")
        .and_then(|i| args.get(i + 1))
        .map_or(Err(()), |s| s.split(',').map(|p| p.trim().parse::<f64>().map_err(|_| ())).collect());
    let Ok(peaks) = peaks else { return json!({"ok": false, "error": "usage: fanmax --peaks RPM[,RPM...]"}) };
    if peaks.is_empty() { return json!({"ok": false, "error": "no fan speeds were measured"}); }
    let key = cool::capture_context().key();
    let path = cool::fanmax_path();
    let mut store = cool::load_fanmax(&path);
    let Some(base) = cool::fan_peak_baseline(&store, &key) else {
        store[&key] = json!({"t": now_epoch(), "peaks": peaks});
        return match cool::save_fanmax(&path, &store) {
            Ok(()) => json!({"ok": true, "baseline": true, "peaks": peaks}),
            Err(e) => json!({"ok": false, "error": e}),
        };
    };
    match cool::check_fan_peaks(&base, &peaks) {
        Some(c) => json!({"ok": true, "baseline": false, "peaks": peaks, "base_peaks": base, "drops_pct": c.drops_pct, "warn": c.warn,
                          "warn_pct": cool::FAN_PEAK_WARN_PCT}),
        None => json!({"ok": false, "error": "the number of fans differs from the baseline; press \"Forget results for these settings\" to start over"}),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let v = match args.first().map(String::as_str) {
        Some("status") => status(),
        Some("run") => {
            let secs = match args.iter().position(|a| a == "--seconds") {
                Some(i) => match args.get(i + 1).and_then(|s| s.parse::<u64>().ok()).filter(|n| (MIN_SECONDS..=MAX_SECONDS).contains(n)) {
                    Some(n) => n,
                    None => { emit(&json!({"ok": false, "error": format!("--seconds must be {MIN_SECONDS}..{MAX_SECONDS}")})); std::process::exit(1); }
                },
                None => DEFAULT_SECONDS,
            };
            run(secs)
        }
        Some("baseline") => edit_store(|runs, key| if cool::promote_latest(runs, key) { Ok(()) } else { Err("no clean run for the current settings yet".into()) }),
        Some("forget") => forget(),
        Some("fans") => fans(),
        Some("fanmax") => fanmax(&args),
        _ => {
            eprintln!("usage: centurion-cooltest status | run [--seconds N] | baseline | forget | fans | fanmax --peaks RPM[,RPM...]");
            std::process::exit(2);
        }
    };
    emit(&v);
    std::process::exit(if v["ok"] == true { 0 } else { 1 });
}
