//! centurion-calibrate — build this machine's signature: what every CPU, scheduler,
//! memory and storage knob does here, idle, under load and on the disk,
//! accumulated across runs.
//!
//!   sudo centurion-calibrate [--budget MIN] [--depth lean|deep|max] [--all] [--phase idle|load|io|both]
//!                      [--only KEY|@thp|@mem|@cpu|@sched|@io,..] [--sessions N] [--thorough] [--dir PATH] [--seed N] [--no-confirm]
//!   sudo centurion-calibrate --oat [...]        legacy: one knob at a time (ref, every candidate, ref)
//!   centurion-calibrate --list                  the plan: keys, values, depth, estimated time, coverage
//!   centurion-calibrate --show                  the signature (weighted estimates per value, interactions)
//!   sudo centurion-calibrate --restore          put back what an interrupted run changed
//!   sudo centurion-calibrate --restore --drop   same, and give up originals that cannot be written back
//!   sudo centurion-calibrate --scene NAME [...] measure in a scene's power context (profile, limits, fan)
//!   sudo centurion-calibrate --verify NAME [--rounds N] [--io] [--goal G]   whole preset vs the boot state (ABBA)
//!   sudo centurion-calibrate --gpu-coupling [--secs S]   CPU-GPU power coupling while a GPU-bound load runs
//!   sudo centurion-calibrate --field LOG... [--game NAME] import game sessions' frame-time logs (FLM / MangoHud)
//!   sudo centurion-calibrate --schedule-boot [--sessions N] [--budget MIN] [--scene NAME] | --cancel-boot
//!   centurion-calibrate --status                boot calibration status
//!   centurion-calibrate --boot-run              (centurion-calibrate-boot service) run a scheduled boot calibration
//!
//! Measurement conditions (docs/AUTOTUNE.md): every row carries its power source (RAPL on AC,
//! battery), the session's context (profile, firmware limits, fan, curve) and its start temperature;
//! the session holds the power context (calibration hold), the fan at full speed on AC
//! (--no-fan-lock), waits for the die between runs, reads the battery at its own cadence, and from
//! the "crowd" stage on varies heat as a design factor (--heat / --no-heat). On battery the DEV
//! phase measures device power states (whole-machine idle power).
//!
//! Default mode = sequential experimental design. Every run changes a balanced random set
//! of knobs together; a Bayesian model (src/model.rs) learns each knob's effect (dose-response
//! for numeric ladders) and the interactions of ALL knob pairs and, with enough data, triples,
//! from ALL runs. Depth grows with the budget (lean <= 20 min, deep <= 50, max beyond): more
//! runs, crowded runs that change many knobs at once, interaction doubts chased on purpose,
//! dose refinement between measured levels. Short sessions without --depth are progressive:
//! each spends its time on what the log still lacks (see Stage), so repeated 15-minute
//! sessions (or --sessions N, back to back) add up to a deep calibration. After the space-filling start each next batch is
//! placed where it most reduces the chance that a per-goal decision (change / leave a knob)
//! or an interaction among the chosen knobs is misjudged; it stops early when both are
//! settled. The predicted best configuration per goal is then measured (confirmation) and
//! its result feeds back. Latency, throughput, power and memory are measured on every run
//! (log-ratio to the session's reference runs, robust to outliers and drift), so any goal
//! weights can be decided from the same data. THP is measured as a family (enabled, mTHP,
//! defrag, shmem) with TLB-reach and huge-fault probes. A run that OOM-kills is bisected to
//! the smallest culprit set, which is stored as unsafe; a run disturbed by other programs
//! counts less.
//!
//! The plan comes from the tunable table itself (CPU, Scheduler, Memory, Storage),
//! minus keys that are structural or unsafe to flip (calib::EXCLUDED). The
//! budget (--budget, default 15 min) is kept by wall clock; repeated sessions
//! fill the gaps of earlier ones and refine the estimates.
//! Results accumulate in /var/lib/centurion/signature.json
//! (recency- and kernel-weighted); autotune doses every knob from it.
//!
//! Phase IO: the storage suite (bench::io) on the disk holding --dir (default: /var/tmp or
//! the first of /var/cache, /home, / that is on a local disk) for the Storage rows and the
//! dirty window - their effects are measured and weighed there only (storage weight).
//! Phase 1 (idle): quiet machine. Phase 2 (load): a ballast child holds memory
//! down to max(1 GiB, 5 % RAM) free, re-faults 64 MiB blocks and keeps half the
//! CPUs busy; part of its memory is perforated (free, but no free 2 MiB block). Brakes: the ballast dies at once if MemAvailable < 256 MiB or PSI
//! memory full > 40 %, is the OOM killer's first target and dies with this
//! process; a candidate that caused an OOM kill is stored as unsafe.
//! Every original is journaled before it is written and restored after each
//! knob, on Ctrl-C, or with --restore after a crash. Needs a clean state:
//! Optimizations → Restore originals first.

use centurion_helpers::autotune::{Goal, Weights, MARGIN};
use centurion_helpers::bench;
use centurion_helpers::calib::{self, Benches, Calibration, Metric, Objective, Phase, Row, Sample};
use centurion_helpers::model::{self, Cfg, Factor, Joint, Mix, Model, Problem, Rng};
use centurion_helpers::tune;
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

const JOURNAL: &str = "/run/centurion/calibrate.json";
const TUNE_LOCK: &str = "/run/centurion/tune/lock";
const TUNE_STATE: &str = "/run/centurion/tune/state.json";
static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn on_signal(_: libc::c_int) { STOP.store(true, Ordering::SeqCst); }

fn die(msg: &str) -> ! { eprintln!("centurion-calibrate: {msg}"); std::process::exit(1) }
fn now() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) }

/// Originals of every file written, persisted before each write.
struct Journal { entries: Vec<(String, PathBuf, String)> }

impl Journal {
    fn load() -> Journal {
        let v: Value = std::fs::read_to_string(JOURNAL).ok().and_then(|s| serde_json::from_str(&s).ok()).unwrap_or(json!([]));
        let entries = v.as_array().into_iter().flatten().filter_map(|e| {
            Some((e[0].as_str()?.to_owned(), PathBuf::from(e[1].as_str()?), e[2].as_str()?.to_owned()))
        }).collect();
        Journal { entries }
    }
    fn save(&self) -> Result<(), String> {
        let v: Vec<Value> = self.entries.iter().map(|(k, p, o)| json!([k, p, o])).collect();
        centurion_helpers::secure_dir("/run/centurion")?;
        centurion_helpers::write_root_file(JOURNAL, &serde_json::to_vec(&v).unwrap())
    }
    /// Sets a tune key, journaling each file's original first.
    fn set(&mut self, key: &str, value: &str) -> Result<(), String> {
        let t = tune::find(key).ok_or_else(|| format!("{key}: unknown"))?;
        let v = tune::validate(t, &Value::String(value.to_owned())).or_else(|_| {
            value.parse::<i64>().map_err(|e| e.to_string()).and_then(|n| tune::validate(t, &json!(n)))
        })?;
        for (f, data) in tune::plan(t, &v)? {
            let Some(orig) = tune::baseline_value(t, &f) else { continue };
            if !self.entries.iter().any(|(_, p, _)| *p == f) {
                self.entries.push((key.to_owned(), f.clone(), orig));
                self.save()?;
            }
            tune::write_value(t, &f, &data)?;
        }
        Ok(())
    }
    /// Puts every original back. The I/O scheduler goes first: switching the elevator resets
    /// the queue's nr_requests and (bfq) writeback throttling, so a journaled original of those
    /// must be written after it, not before.
    fn restore(&mut self) -> Vec<String> {
        let mut errs = Vec::new();
        let mut failed: Vec<(String, PathBuf, String)> = Vec::new();
        let sched = |k: &str| k == "blk.scheduler";
        let order = self.entries.iter().filter(|e| sched(&e.0)).chain(self.entries.iter().rev().filter(|e| !sched(&e.0)));
        for (k, f, o) in order {
            if let Some(t) = tune::find(k) {
                if let Err(e) = tune::write_value(t, f, o) {
                    if f.exists() {
                        errs.push(format!("{k} {}: {e}", f.display()));
                        failed.push((k.clone(), f.clone(), o.clone()));
                    }
                }
            }
        }
        // An original that could not be written back stays journaled (the journal used to be deleted
        // regardless, and with it the only record of the value): the next restore tries it again, and
        // a later set() of the same file keeps this original instead of recording the changed value.
        failed.reverse();  // back into first-write order
        self.entries = failed;
        if self.entries.is_empty() { let _ = std::fs::remove_file(JOURNAL); } else if let Err(e) = self.save() { errs.push(format!("journal: {e}")); }
        errs
    }
}

/// Knob group -> writes for one value.
fn apply(j: &mut Journal, key: &str, value: &str, rate: u64, ram: u64) -> Result<(), String> {
    match key {
        "vm.dirty" => {
            let w: f64 = value.parse().map_err(|_| "bad window")?;
            let (bg, d) = centurion_helpers::autotune::dirty_pair(rate, ram, w);
            // Background first when shrinking, limit first when growing: bg < dirty at every step.
            let cur: u64 = tune::find("vm.dirty_bytes").and_then(tune::current).and_then(|s| s.parse().ok()).unwrap_or(0);
            if d >= cur { j.set("vm.dirty_bytes", &d.to_string())?; j.set("vm.dirty_background_bytes", &bg.to_string()) }
            else { j.set("vm.dirty_background_bytes", &bg.to_string())?; j.set("vm.dirty_bytes", &d.to_string()) }
        }
        "thp" => {
            let (mode, mthp) = value.split_once('+').map_or((value, false), |(m, _)| (m, true));
            j.set("thp.enabled", mode)?;
            for k in ["thp.mthp_16k", "thp.mthp_32k", "thp.mthp_64k"] {
                if tune::find(k).map_or(false, |t| !tune::files(t).is_empty()) { j.set(k, if mthp { "inherit" } else { "never" })?; }
            }
            Ok(())
        }
        _ => j.set(key, value),
    }
}

/// Live value of a group, as one of its labels. A Storage row whose disks disagree ("mixed",
/// e.g. a USB stick next to the NVMe drives) takes the value of the disk the suite measures.
fn live_label(key: &str, disk: Option<&str>) -> Option<String> {
    let cur = |k: &str| tune::find(k).and_then(tune::current);
    if let (Some(attr), Some(d)) = (key.strip_prefix("blk."), disk) {
        let v = cur(key)?;
        if v != "mixed" { return Some(v); }
        let raw = std::fs::read_to_string(format!("/sys/block/{d}/queue/{attr}")).ok()?;
        let raw = raw.trim();
        return Some(match (raw.find('['), raw.find(']')) { (Some(a), Some(b)) if b > a => raw[a + 1..b].to_owned(), _ => raw.to_owned() });
    }
    match key {
        "vm.dirty" => Some("1".into()),
        "thp" => {
            let m = cur("thp.enabled")?;
            if m != "always" && m != "madvise" && m != "never" { return None; }
            let mthp = ["thp.mthp_16k", "thp.mthp_32k", "thp.mthp_64k"].iter().any(|k| cur(k).map_or(false, |v| v != "never"));
            if m == "never" && mthp { return None; }
            Some(if mthp { format!("{m}+mthp") } else { m })
        }
        _ => cur(key),
    }
}

/// The ballast child plus a brake thread that kills it when memory gets tight.
struct Ballast { child: Arc<std::sync::Mutex<std::process::Child>>, pid: u32, braked: Arc<AtomicBool>, stop: Arc<AtomicBool>, held_mib: usize }

impl Ballast {
    fn start(exe: &std::path::Path) -> Result<Ballast, String> {
        use std::io::BufRead;
        let ram_mib = bench::meminfo_kb("MemTotal:").unwrap_or(0) / 1024;
        let avail = bench::meminfo_kb("MemAvailable:").unwrap_or(0) / 1024;
        let headroom = (ram_mib * 5 / 100).max(1024);
        // Leave the headroom plus room for the probe (sparse + dense heap).
        let target = avail.saturating_sub(headroom + 768).min(ram_mib * 85 / 100);
        if target < 1024 { return Err(format!("only {avail} MiB available: not enough to build pressure safely")); }
        let threads = (std::thread::available_parallelism().map_or(2, |n| n.get()) / 2).max(1);
        let mut child = std::process::Command::new(exe).arg("__ballast").arg(target.to_string()).arg(threads.to_string())
            .arg(headroom.to_string()).arg("1").stdout(std::process::Stdio::piped()).spawn().map_err(|e| format!("ballast: {e}"))?;
        let mut line = String::new();
        let out = child.stdout.take().ok_or("ballast: no stdout")?;
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || { let mut r = std::io::BufReader::new(out); let mut l = String::new(); let _ = r.read_line(&mut l); let _ = tx.send(l); });
        match rx.recv_timeout(Duration::from_secs(120)) { Ok(l) => line = l, Err(_) => {} }
        let held_mib = line.trim().strip_prefix("ready ").and_then(|n| n.parse().ok()).unwrap_or(0);
        if held_mib == 0 { let _ = child.kill(); return Err("ballast did not come up".into()); }
        let (braked, stop) = (Arc::new(AtomicBool::new(false)), Arc::new(AtomicBool::new(false)));
        // The watcher kills through the Child handle, under the lock shared with alive()/stop(): it used
        // to kill a bare pid, which could already have been reaped and handed to another process.
        let pid = child.id();
        let child = Arc::new(std::sync::Mutex::new(child));
        let (b2, s2, c2) = (braked.clone(), stop.clone(), child.clone());
        std::thread::spawn(move || while !s2.load(Ordering::Relaxed) {
            let avail = bench::meminfo_kb("MemAvailable:").unwrap_or(u64::MAX) / 1024;
            let full = centurion_helpers::autotune::psi_avg10(&std::fs::read_to_string("/proc/pressure/memory").unwrap_or_default(), "full").unwrap_or(0.0);
            if avail < 256 || full > 40.0 {
                let mut c = c2.lock().unwrap_or_else(|e| e.into_inner());
                if matches!(c.try_wait(), Ok(None)) { let _ = c.kill(); }  // std never signals a reaped child
                b2.store(true, Ordering::SeqCst);
                break;
            }
            std::thread::sleep(Duration::from_millis(250));
        });
        Ok(Ballast { child, pid, braked, stop, held_mib })
    }
    fn alive(&mut self) -> bool {
        !self.braked.load(Ordering::SeqCst) && matches!(self.child.lock().unwrap_or_else(|e| e.into_inner()).try_wait(), Ok(None))
    }
    fn stop(self) {
        self.stop.store(true, Ordering::Relaxed);
        let mut c = self.child.lock().unwrap_or_else(|e| e.into_inner());
        let _ = c.kill();
        let _ = c.wait();
    }
}


/// `scale` stretches every measurement window (1.0 default, 1.6 with --thorough).
/// `battery`: power source chosen at session start (whole machine) instead of RAPL.
/// `disk`: whole disk holding `dir` (None: not a local disk - no IO phase).
/// `bat_period`: measured update period of the battery's discharge reading (s); `heat_secs`: heat
/// soak before a "hot" run; `cx`: id of the session's measurement context; `heat`: the design
/// varies the heat state (None = by depth, see Depth::heat).
struct Ctx { exe: PathBuf, dir: String, disk: Option<String>, io_size: usize, scale: f64, battery: bool, bat_period: Option<f64>,
             heat_secs: f64, cx: String, heat: Option<bool> }

fn ms(c: &Ctx, base: u64) -> Duration { Duration::from_millis((base as f64 * c.scale) as u64) }

/// Latency side of the CPU benchmarks, the same on a quiet and on a loaded machine: thread
/// ping-pong, the light frame loop (tail and median), the game loop (frame time, tail, pacing)
/// and the cache-bound game loop (frame time and tail over a working set of half the largest L3).
/// `quiet`: the game loop's package power is the loop's own (under load the ballast's spinners
/// draw most of it, and the whole run's package power is recorded instead).
fn cpu_suite(c: &Ctx, s: &mut Sample, quiet: bool) {
    if let Some(p) = bench::pingpong_p99_us(2000) { s.insert(Metric::PingPongP99Us, p); }
    if let Some((t, m)) = bench::frame_us(ms(c, 600)) { s.insert(Metric::FrameP99Us, t); s.insert(Metric::FrameMedUs, m); }
    if let Some(g) = bench::game_loop(ms(c, 720)) {
        s.insert(Metric::GameFrameMs, g.med_ms); s.insert(Metric::GameTailMs, g.tail_ms); s.insert(Metric::GameJitterUs, g.jitter_us);
        if let (true, Some(w)) = (quiet, g.watts) { s.insert(Metric::GameW, w); }
    }
    if let Some((m, t)) = bench::cache_loop(ms(c, 600)) { s.insert(Metric::CacheFrameMs, m); s.insert(Metric::CacheTailMs, t); }
}

fn run_idle(b: Benches, c: &Ctx) -> Result<Sample, String> {
    // Page cache and fragmentation only matter to the heap and I/O probes.
    if b.io || b.cpu_mem { bench::settle(); } else { std::thread::sleep(Duration::from_millis(300)); }
    let mut s = Sample::new();
    if b.idle {
        std::thread::sleep(ms(c, 800));
        if let Some(w) = bench::idle_w_with(ms(c, 2000), c.battery, c.bat_period) { s.insert(Metric::IdleW, w); }
    }
    if b.cpu || b.cpu_mem {
        if let Some(w) = bench::wake_p99_us(ms(c, 600)) { s.insert(Metric::WakeP99Us, w); }
    }
    if b.cpu {
        cpu_suite(c, &mut s, true);
        s.insert(Metric::CpuSingle, bench::cpu_single(ms(c, 400)));
        let k = bench::contended(ms(c, 600));
        s.insert(Metric::CpuMulti, k.rate);
        if let Some(e) = k.eff { s.insert(Metric::CpuEff, e); }
        if let Some(f) = k.frame_tail { s.insert(Metric::BusyFrameP99Us, f); }
    }
    if b.cpu || b.cpu_mem { if let Some(j) = bench::jobs_per_sec(&c.exe, ms(c, 450)) { s.insert(Metric::JobsPerSec, j); } }
    if b.cpu_mem { bench::probe_child(&c.exe, 512, &mut s)?; }
    if b.io { bench::io(&c.dir, c.io_size, &mut s)?; }
    Ok(s)
}

/// One loaded run. Returns the sample and whether an OOM kill happened.
/// One loaded run (memory is compacted once per knob, not per run).
fn run_load(b: Benches, c: &Ctx) -> Result<(Sample, bool), String> {
    std::thread::sleep(ms(c, 800));  // kswapd settles on the new values
    let (stall0, oom0, t0) = (bench::vmstat("allocstall"), bench::vmstat("oom_kill"), Instant::now());
    let (res, w) = bench::with_pkg_power(|| -> Result<Sample, String> {
        let mut s = Sample::new();
        if let Some(w) = bench::wake_p99_us(ms(c, 600)) { s.insert(Metric::WakeP99Us, w); }
        if b.cpu {
            cpu_suite(c, &mut s, false);
            s.insert(Metric::CpuSingle, bench::cpu_single(ms(c, 400)));
            // The ballast's spinners hold half the CPUs already: 1.5 threads per CPU on top of them.
            let k = bench::contended(ms(c, 450));
            s.insert(Metric::CpuMulti, k.rate);
            if let Some(f) = k.frame_tail { s.insert(Metric::BusyFrameP99Us, f); }
        }
        // Short jobs under memory pressure: every fresh heap is paid for by reclaim.
        if let Some(j) = bench::jobs_per_sec(&c.exe, ms(c, 450)) { s.insert(Metric::JobsPerSec, j); }
        if b.cpu_mem { bench::probe_child(&c.exe, 256, &mut s)?; }
        if b.io { bench::io(&c.dir, c.io_size / 4, &mut s)?; }
        Ok(s)
    });
    let mut s = res?;
    s.insert(Metric::StallsPerSec, (bench::vmstat("allocstall") - stall0) as f64 / t0.elapsed().as_secs_f64());
    if let Some(w) = w { s.insert(Metric::PkgW, w); }
    Ok((s, bench::vmstat("oom_kill") > oom0))
}

/// One run of the IO phase: page cache dropped (the previous run's file pages must not serve
/// this one's reads), then the storage suite on the measured disk.
fn run_io(c: &Ctx) -> Result<Sample, String> {
    unsafe { libc::sync(); }
    let _ = std::fs::write("/proc/sys/vm/drop_caches", "3");
    std::thread::sleep(Duration::from_millis(400));
    let mut s = Sample::new();
    bench::io(&c.dir, c.io_size, &mut s)?;
    Ok(s)
}

/// One run of the DEV phase (battery): the devices settle into their idle power states, then the
/// whole machine's idle power over at least ten seconds and four fresh battery readings.
fn run_dev(c: &Ctx) -> Result<Sample, String> {
    std::thread::sleep(ms(c, 3000));
    let mut s = Sample::new();
    let w = bench::idle_w_with(ms(c, 10_000), true, c.bat_period).ok_or("no battery power reading")?;
    s.insert(Metric::IdleW, w);
    Ok(s)
}

/// Seconds per run (default windows; --thorough scales them).
fn est_secs(ph: Phase, b: Benches, scale: f64) -> f64 {
    let s = match ph {
        Phase::Dev => 13.5,
        Phase::Idle => 0.3 + if b.io || b.cpu_mem { 0.8 } else { 0.0 } + if b.idle { 2.8 } else { 0.0 } + if b.cpu || b.cpu_mem { 1.1 } else { 0.0 }
            + if b.cpu { 3.2 } else { 0.0 } + if b.cpu_mem { 1.8 } else { 0.0 } + if b.io { 4.0 } else { 0.0 },
        Phase::Load => 1.9 + if b.cpu { 3.0 } else { 0.0 } + if b.cpu_mem { 1.0 } else { 0.0 } + if b.io { 2.0 } else { 0.0 },
        Phase::Io => 0.6 + 4.0,
    };
    s * scale
}

/// Runs for one knob: the reference opens and closes each round (drift shows
/// up as reference noise), every candidate once per round.
fn runs_for(values: usize, rounds: usize) -> usize { rounds * (values + 1) + 1 }

/// Run order: [ref, c1..cn, ref] per round, candidates rotated between rounds.
fn order(reference: &str, cands: &[String], rounds: usize) -> Vec<String> {
    let mut out = Vec::new();
    for r in 0..rounds {
        out.push(reference.to_owned());
        let mut c = cands.to_vec();
        let n = c.len();
        if n > 0 { c.rotate_left(r % n); }
        out.extend(c);
    }
    out.push(reference.to_owned());
    out
}

/// One planned measurement: a key in one phase.
struct Item { key: String, phase: Phase, benches: Benches, reference: String, values: Vec<String>, secs: f64 }

/// `--only` entry: a key, or a group alias (@thp, @mem, @cpu, @sched, @io = Storage rows + dirty window).
fn selects(x: &str, key: &str, group: &str) -> bool {
    match x {
        "@io" | "@storage" => calib::io_key(key),
        "@thp" => key == "thp" || key.starts_with("thp."),
        "@mem" => group == "Memory",
        "@cpu" => group == "CPU",
        "@sched" => group == "Scheduler",
        "@dev" | "@devices" => calib::dev_key(key),
        _ => x == key,
    }
}

fn plan(rounds: usize, scale: f64, ram: u64, only: &Option<Vec<String>>, phase: &str, cal: &Calibration, disk: &Result<String, String>, battery: bool) -> (Vec<Item>, Vec<(String, String)>) {
    let swap = bench::meminfo_kb("SwapTotal:").unwrap_or(0) > 0;
    let zram = std::fs::read_to_string("/proc/swaps").unwrap_or_default().contains("/dev/zram");
    let numa = std::fs::read_dir("/sys/devices/system/node").map(|d| d.flatten().filter(|e| e.file_name().to_string_lossy().starts_with("node")).count()).unwrap_or(1);
    let mut items = Vec::new();
    let mut skipped = Vec::new();
    let mut keys: Vec<(String, &'static str)> = vec![("vm.dirty".into(), "Memory"), ("thp".into(), "Memory")];
    for t in tune::TUNABLES {
        if calib::dev_key(t.key) {
            if !battery { skipped.push((t.key.to_owned(), "device power state: measured on battery only (RAPL cannot see it)".into())); continue; }
            if !t.debugfs && tune::files(t).is_empty() { skipped.push((t.key.to_owned(), "not on this machine/kernel".into())); continue; }
            keys.push((t.key.to_owned(), "Devices"));
            continue;
        }
        if !["CPU", "Scheduler", "Memory", "Storage"].contains(&t.group) { continue; }
        if let Some((_, why)) = calib::EXCLUDED.iter().find(|(k, _)| *k == t.key) { skipped.push((t.key.to_owned(), why.to_string())); continue; }
        let why = if !tune::vendor_ok(t) { Some("other CPU vendor") }
            else if !t.debugfs && tune::files(t).is_empty() { Some("not on this machine/kernel") }
            else if t.key.starts_with("zswap.") && (zram || !swap) { Some("zram / no swap: zswap unused") }
            else if (t.key == "vm.swappiness" || t.key == "vm.page_cluster") && !swap { Some("no swap") }
            else if (t.key == "kernel.numa_balancing" || t.key == "vm.zone_reclaim_mode") && numa <= 1 { Some("single NUMA node") }
            else { None };
        if let Some(w) = why { skipped.push((t.key.to_owned(), w.to_owned())); continue; }
        keys.push((t.key.to_owned(), t.group));
    }
    for (key, group) in keys {
        if only.as_ref().map_or(false, |o| !o.iter().any(|x| selects(x, &key, group))) { continue; }
        if calib::io_key(&key) { if let Err(e) = disk { skipped.push((key, format!("no storage suite: {e} (--dir PATH on a local disk)"))); continue; } }
        let Some(reference) = live_label(&key, disk.as_ref().ok().map(String::as_str)) else { skipped.push((key, "live value unreadable".into())); continue };
        let t = tune::find(&key);
        let opts: Vec<String> = t.map(|t| tune::options(t).into_iter().map(|(v, _)| v).collect()).unwrap_or_default();
        let raw = match key.as_str() {
            "vm.dirty" => vec!["0.25".into(), "0.5".into(), "2".into()],
            "thp" => vec!["never".into(), "madvise".into(), "always".into(), "madvise+mthp".into(), "always+mthp".into()],
            _ => calib::override_values(&key, &reference, ram / 1024).or_else(|| t.map(|t| calib::generic_values(&t.kind, &reference, &opts))).unwrap_or_default(),
        };
        let mut values = Vec::new();
        for v in raw {
            let vs = if v == "@wsf" {
                [128u64, 256, 512].iter().map(|h| h << 20).filter(|h| *h <= (ram / 50).min(1 << 30))
                    .map(|h| centurion_helpers::autotune::wsf_for(h, ram).to_string()).collect()
            } else { vec![v] };
            for x in vs {
                if t.map_or(false, |t| t.kind == tune::Kind::Choice) && !opts.is_empty() && !opts.contains(&x) { continue; }
                if x != reference && !values.contains(&x) { values.push(x); }
            }
        }
        if key == "vm.dirty" {
            // Windows are clamped to [32 MiB, min(RAM/50, 1 GiB)]: on a fast disk several become the same limits,
            // which would be measured as different doses (or as the reference itself).
            let rate = centurion_helpers::iorate::gather().bps;
            let bytes = |w: &str| w.parse::<f64>().ok().map(|w| centurion_helpers::autotune::dirty_pair(rate, ram, w));
            let mut seen = vec![bytes(&reference)];
            values.retain(|v| { let b = bytes(v); if seen.contains(&b) { false } else { seen.push(b); true } });
        }
        if values.is_empty() { skipped.push((key, "no alternative values".into())); continue; }
        for (ph, b) in calib::phases_for(&key, group) {
            if phase != "both" && phase != ph.name() { continue; }
            let secs = est_secs(ph, b, scale) * runs_for(values.len(), rounds) as f64;
            items.push(Item { key: key.clone(), phase: ph, benches: b, reference: reference.clone(), values: values.clone(), secs });
        }
    }
    // Least measured first; phases stay together (one ballast for the whole load phase).
    items.sort_by_key(|i| (i.phase.idx(), cal.coverage(&i.key, i.phase)));
    (items, skipped)
}

// ── sequential design ────────────────────────────────────────────────────────

/// How deep a session digs, from its time budget: the shape of the space-filling runs (how
/// many knobs change together), the run cap, the share spent space-filling, the batch size,
/// confirmation repeats, how many doubtful interactions are chased on purpose, the largest
/// random experiment, and how many dose refinements (midpoints of numeric ladders) are tried.
#[derive(Clone, Copy, Debug, PartialEq)]
struct Depth { name: &'static str, mix: Mix, cap: usize, init: f64, batch: usize, reps: usize, pairs: usize, crowd: usize, ladder: usize, explore: bool }

impl Depth {
    /// Whether the design varies the heat state: once every knob has its basic coverage (a hot run
    /// costs a heat soak), i.e. deep/max sessions and progressive stages from "crowd" on.
    fn heat(&self) -> bool { !matches!(self.name, "lean" | "lean/base" | "lean/pairs") }
}

impl Depth {
    /// `minutes`: the whole session (infinite = --all); `nf`: most knobs in one phase.
    fn pick(minutes: f64, nf: usize, name: Option<&str>) -> Depth {
        let n = name.unwrap_or(if minutes <= 20.0 { "lean" } else if minutes <= 50.0 { "deep" } else { "max" });
        let f = nf.max(4);
        match n {
            "deep" => Depth { name: "deep", mix: Mix { cluster: 0.35, cluster_k: (2, 5), spread_k: (4, 9), crowd: 0.2, crowd_k: (8, 14.min(f)) },
                              cap: 520, init: 0.5, batch: 6, reps: 3, pairs: 8, crowd: 10, ladder: 4, explore: false },
            "max" => Depth { name: "max", mix: Mix { cluster: 0.25, cluster_k: (2, 6), spread_k: (4, 10), crowd: 0.35, crowd_k: (10.min(f), (f / 2).max(12).min(f)) },
                             cap: 1000, init: 0.45, batch: 8, reps: 4, pairs: 16, crowd: 14, ladder: 8, explore: false },
            _ => Depth { name: "lean", mix: Mix::lean(), cap: 240, init: 0.55, batch: 5, reps: 2, pairs: 0, crowd: 6, ladder: 0, explore: false },
        }
    }
}

/// Progressive lean sessions: a short session builds on what the log already holds and
/// spends its time on the next thing the data lacks, so sessions left running while the
/// machine is free add up to a deep calibration. Stages per phase, derived from the log
/// itself (no counters to go stale; runs of deep/max sessions count too):
///   base   - until every value of every knob was in >= 6 runs
///   pairs  - until 90 % of knob pairs were changed together in >= 2 runs (gap-filling)
///   crowd  - until max(24, knobs) runs changed >= 8 knobs at once
///   refine - one session of dose refinement (or a deep/max session, or refined doses in the log)
///   polish - then rotating: doubt (interactions, triples), crowd, refine - the one done least
/// Every session's strategy is noted in the signature, so the next one knows what ran.
#[derive(Clone, Copy, Debug, PartialEq, PartialOrd)]
enum Stage { Base, Pairs, Crowd, Refine, Polish }

impl Stage {
    fn what(self) -> &'static str {
        match self {
            Stage::Base => "every value of every knob measured (>= 6 runs each)",
            Stage::Pairs => "every knob pair seen together (>= 2 runs), gaps first",
            Stage::Crowd => "crowded runs (many knobs at once: saturation, higher-order effects)",
            Stage::Refine => "dose refinement between measured levels",
            Stage::Polish => "rotating per session: interaction doubt, crowd, dose refinement",
        }
    }
}

/// What the log holds for one phase's knobs.
struct Progress { runs: usize, sessions: usize, main_min: usize, pairs2: f64, crowded: usize, crowd_need: usize, refined: usize, numeric: usize,
                  refine_done: bool, polish: [usize; 3] }

fn is_numeric(f: &Factor) -> bool { f.reference.parse::<f64>().is_ok() && !f.values.is_empty() && f.values.iter().all(|v| v.parse::<f64>().is_ok()) }

/// A numeric value strictly between the knob's planned levels that is not one of them.
fn is_mid(f: &Factor, v: &str) -> bool {
    let Ok(x) = v.parse::<f64>() else { return false };
    let lv: Vec<f64> = std::iter::once(&f.reference).chain(&f.values).filter_map(|s| s.parse().ok()).collect();
    let (lo, hi) = lv.iter().fold((f64::MAX, f64::MIN), |(a, b), l| (a.min(*l), b.max(*l)));
    lv.len() >= 2 && x > lo && x < hi && !lv.iter().any(|l| (l - x).abs() < 1e-9)
}

fn progress(cal: &Calibration, phase: Phase, factors: &[Factor]) -> Progress {
    let nf = factors.len();
    let idx: BTreeMap<&str, usize> = factors.iter().enumerate().map(|(i, f)| (f.key.as_str(), i)).collect();
    let mut vc: Vec<Vec<usize>> = factors.iter().map(|f| vec![0; f.values.len()]).collect();
    let (mut co, mut refined, mut sess) = (vec![0usize; nf * nf], vec![false; nf], std::collections::BTreeSet::new());
    let (mut runs, mut crowded) = (0, 0);
    let big = 8.min(nf.saturating_sub(1)).max(2);
    for r in cal.rows.iter().filter(|r| r.phase == phase) {
        runs += 1;
        sess.insert(r.sess);
        let ids: Vec<usize> = r.cfg.iter().filter_map(|(k, v)| {
            let i = *idx.get(k.as_str())?;
            match factors[i].values.iter().position(|x| x == v) { Some(j) => vc[i][j] += 1, None => if is_mid(&factors[i], v) { refined[i] = true } }
            Some(i)
        }).collect();
        if ids.len() >= big { crowded += 1; }
        for &a in &ids { for &b in &ids { if a < b { co[a * nf + b] += 1; } } }
    }
    let total = nf * nf.saturating_sub(1) / 2;
    let seen2 = (0..nf).flat_map(|a| (a + 1..nf).map(move |b| (a, b))).filter(|(a, b)| co[a * nf + b] >= 2).count();
    Progress {
        runs, sessions: sess.len(), main_min: vc.iter().flatten().min().copied().unwrap_or(0),
        pairs2: if total == 0 { 1.0 } else { seen2 as f64 / total as f64 }, crowded,
        crowd_need: if nf < 10 { 0 } else { 24.max(nf) }, refined: refined.iter().filter(|x| **x).count(),
        numeric: factors.iter().filter(|f| is_numeric(f)).count(),
        refine_done: cal.strategies_of(phase).iter().any(|s| matches!(s.as_str(), "lean/refine" | "lean/polish-refine" | "deep" | "max")),
        polish: ["lean/polish-doubt", "lean/polish-crowd", "lean/polish-refine"].map(|n| cal.strategies_of(phase).iter().filter(|s| *s == n).count()),
    }
}

impl Progress {
    fn stage(&self) -> Stage {
        if self.main_min < 6 { Stage::Base }
        else if self.pairs2 < 0.9 { Stage::Pairs }
        else if self.crowded < self.crowd_need { Stage::Crowd }
        else if !self.refine_done && self.refined == 0 && self.numeric > 0 { Stage::Refine }
        else { Stage::Polish }
    }
    fn line(&self) -> String {
        format!("{} run(s) in {} session(s); least-measured value {}/6 runs, knob pairs seen twice {:.0} %/90 %, crowded runs {}/{}, refined doses {}",
                self.runs, self.sessions, self.main_min, self.pairs2 * 100.0, self.crowded, self.crowd_need, self.refined)
    }
}

impl Depth {
    /// A lean session's shape for its stage (time decides the run count). `explore`: once the
    /// decisions are settled the rest of the time keeps gathering coverage instead of stopping.
    /// `polish`: how often each polish strategy ran (the least-run one goes next).
    fn progressive(stage: Stage, nf: usize, polish: [usize; 3]) -> Depth {
        let f = nf.max(4);
        let deep = Mix { cluster: 0.35, cluster_k: (2, 5), spread_k: (4, 9), crowd: 0.2, crowd_k: (8.min(f), 14.min(f)) };
        let crowd = Mix { cluster: 0.2, cluster_k: (2, 5), spread_k: (4, 9), crowd: 0.5, crowd_k: (8.min(f), 14.min(f)) };
        let d = |name, mix, init, batch, reps, pairs, crowd, ladder| Depth { name, mix, cap: 240, init, batch, reps, pairs, crowd, ladder, explore: true };
        match stage {
            Stage::Base => d("lean/base", Mix::lean(), 0.55, 5, 2, 0, 6, 0),
            Stage::Pairs => d("lean/pairs", Mix { cluster: 0.3, cluster_k: (2, 5), spread_k: (4, 9), crowd: 0.0, crowd_k: (8, 12) }, 0.6, 5, 2, 6, 8, 0),
            Stage::Crowd => d("lean/crowd", crowd, 0.5, 6, 2, 8, 12, 0),
            Stage::Refine => d("lean/refine", deep, 0.2, 6, 3, 8, 10, 4),
            Stage::Polish => match (0..3).min_by_key(|&i| (polish[i], i)).unwrap_or(0) {
                0 => d("lean/polish-doubt", deep, 0.15, 6, 3, 16, 14, 2),
                1 => d("lean/polish-crowd", crowd, 0.4, 6, 2, 12, 14, 0),
                _ => d("lean/polish-refine", deep, 0.15, 6, 4, 8, 10, 6),
            },
        }
    }
}

/// One executed design point (empty cfg = reference run): wall-clock time, evidence weight, die
/// temperature at its start and whether it ran heat-soaked (cfg holds ("ctx.heat", "hot")).
struct Run { cfg: Cfg, s: Sample, order: usize, t: u64, w: f64, temp: Option<f64> }

fn is_hot(cfg: &[(String, String)]) -> bool { cfg.iter().any(|(k, v)| k == "ctx.heat" && v == "hot") }

/// What every row of a session shares: the power source and the measurement context.
#[derive(Clone)]
struct Meta { src: calib::PowerSrc, cx: String }

fn obj_index(m: Metric) -> usize { match m.objective() { Objective::Lat => 0, Objective::Thr => 1, Objective::Pwr => 2, Objective::Mem => 3 } }

/// Weight of every metric inside its objective, from the scatter of the session's reference
/// runs (robust sd of the log values): w = 1 / (sd^2 + mean sd^2 of the objective), i.e. halfway
/// between equal weights and inverse variance, then kept within 1/3..3x of the equal share. A
/// tail that jumps 30 % between identical runs no longer drowns a bandwidth that moves 1 %, and
/// no single metric can take an objective over. Fewer than 4 reference runs: equal weights.
fn metric_weights(refs: &[&Run], ms: &[Metric]) -> BTreeMap<Metric, f64> {
    let sd = |m: Metric| -> Option<f64> {
        let v: Vec<f64> = refs.iter().filter_map(|r| r.s.get(&m)).filter(|x| **x + m.eps() > 0.0).map(|x| (x + m.eps()).ln()).collect();
        if v.len() < 4 { return None; }
        let mut a = v.clone();
        let med = calib::median(&mut a)?;
        let mut d: Vec<f64> = v.iter().map(|x| (x - med).abs()).collect();
        Some(1.4826 * calib::median(&mut d)?)
    };
    let mut out = BTreeMap::new();
    for o in 0..4 {
        let mo: Vec<Metric> = ms.iter().copied().filter(|m| obj_index(*m) == o).collect();
        if mo.is_empty() { continue; }
        let sds: Vec<Option<f64>> = mo.iter().map(|m| sd(*m)).collect();
        if sds.iter().any(Option::is_none) { for m in &mo { out.insert(*m, 1.0); } continue; }
        let var: Vec<f64> = sds.iter().map(|x| x.unwrap().powi(2)).collect();
        let mean = (var.iter().sum::<f64>() / var.len() as f64).max(1e-6);
        let raw: Vec<f64> = var.iter().map(|v| 1.0 / (v + mean)).collect();
        let avg = raw.iter().sum::<f64>() / raw.len() as f64;
        for (m, w) in mo.iter().zip(&raw) { out.insert(*m, (w / avg).clamp(1.0 / 3.0, 3.0)); }
    }
    out
}

/// Runs -> rows: per metric the log-ratio to the session's reference runs (all runs when there
/// are fewer than 3), sign so that + is better, each clipped to +-0.5 (a stray outlier cannot
/// dominate), averaged per objective with the metric's noise weight (`metric_weights`) and brought
/// to the objective's scale (`calib::objective_gain`: the gain summed over the objective's metrics,
/// not diluted by the ones a knob does not touch). A metric
/// counts only if at least 80 % of the runs have it, so every run is judged on the same set.
/// Each row keeps its run's time (drift model) and weight.
fn compute_rows(runs: &[Run], phase: Phase, sess: u64, kernel: &str, n_total: usize, meta: &Meta) -> Vec<Row> {
    if runs.len() < 4 { return Vec::new(); }
    // Start temperature relative to the session's typical start in the same heat state.
    let typical = |hot: bool| { let mut v: Vec<f64> = runs.iter().filter(|r| is_hot(&r.cfg) == hot).filter_map(|r| r.temp).collect(); calib::median(&mut v) };
    let (t_cool, t_hot) = (typical(false), typical(true));
    let refs: Vec<&Run> = runs.iter().filter(|r| r.cfg.is_empty()).collect();
    let base_runs: Vec<&Run> = if refs.len() >= 3 { refs.clone() } else { runs.iter().collect() };
    let mut base: BTreeMap<Metric, f64> = BTreeMap::new();
    for m in Metric::ALL {
        if runs.iter().filter(|r| r.s.contains_key(&m)).count() * 5 < runs.len() * 4 { continue; }
        let mut v: Vec<f64> = base_runs.iter().filter_map(|r| r.s.get(&m).copied()).collect();
        if let Some(b) = calib::median(&mut v) { base.insert(m, b); }
    }
    // Weights over the metrics this phase actually measured (the others are not in any row).
    let wts = metric_weights(&refs, &base.keys().copied().collect::<Vec<_>>());
    let mut gain = [0usize; 4];
    for m in base.keys() { gain[obj_index(*m)] += 1; }
    let gain = gain.map(calib::objective_gain);
    runs.iter().map(|r| {
        let mut acc = [(0.0f64, 0.0f64); 4];
        for (m, b) in &base {
            let Some(&v) = r.s.get(m) else { continue };
            let e = m.eps();
            if v + e <= 0.0 || b + e <= 0.0 { continue; }
            let mut d = ((v + e) / (b + e)).ln();
            if !m.higher_better() { d = -d; }
            let (i, w) = (obj_index(*m), wts.get(m).copied().unwrap_or(1.0));
            acc[i].0 += w * d.clamp(-0.5, 0.5); acc[i].1 += w;
        }
        let mut y = [f64::NAN; 4];
        for i in 0..4 { if acc[i].1 > 0.0 { y[i] = acc[i].0 / acc[i].1 * gain[i]; } }
        let tc = match (r.temp, if is_hot(&r.cfg) { t_hot } else { t_cool }) { (Some(t), Some(m)) => t - m, _ => f64::NAN };
        Row { phase, sess, pos: (r.order as f64 / n_total.max(runs.len()) as f64).min(1.0), t: r.t, kernel: kernel.into(), cfg: r.cfg.clone(), y, w: r.w,
              bv: calib::BENCH_VERSION, src: if phase == Phase::Dev { calib::PowerSrc::Battery } else { meta.src }, cx: meta.cx.clone(), tc, field: false }
    }).collect()
}

/// Reference levels of a session's reference runs (watts): what the relative effects are relative to.
fn ref_levels(runs: &[Run], phase: Phase, src: calib::PowerSrc) -> Vec<(String, f64)> {
    let med = |m: Metric| { let mut v: Vec<f64> = runs.iter().filter(|r| r.cfg.is_empty()).filter_map(|r| r.s.get(&m).copied()).collect(); calib::median(&mut v) };
    let mut out = Vec::new();
    match phase {
        Phase::Idle => {
            if let Some(w) = med(Metric::IdleW) { out.push((format!("idle_w_{}", src.name()), w)); }
            if let (Some(w), calib::PowerSrc::Rapl) = (med(Metric::GameW), src) { out.push(("game_w".into(), w)); }
        }
        Phase::Load => if let (Some(w), calib::PowerSrc::Rapl) = (med(Metric::PkgW), src) { out.push(("pkg_w".into(), w)); },
        Phase::Dev => if let Some(w) = med(Metric::IdleW) { out.push(("idle_w_battery".into(), w)); },
        Phase::Io => {}
    }
    out
}

/// A finished run: sample, OOM kill, evidence weight (a busy machine counts less), time, start temperature.
struct Done { s: Sample, oom: bool, w: f64, t: u64, temp: Option<f64> }

struct Runner<'a> { j: Journal, c: &'a Ctx, rate: u64, ram: u64, ballast: Option<Ballast>, restarts: u32, runs: usize, t0: Instant, busy: usize,
                    /// Settings every run starts from (a run that changes the same key overrides them).
                    base: Cfg,
                    /// Per phase: the die temperature a cool run starts from (set by the phase's first cool run).
                    cool: [Option<f64>; 4],
                    /// When the last heat soak ended (consecutive hot runs only top it up).
                    last_hot: Option<Instant>,
                    /// Display state of the phase's first run, and how many runs differed from it.
                    disp0: Option<String>, disp_off: usize,
                    /// Seconds spent waiting for the die to cool.
                    gate_s: f64 }

impl Runner<'_> {
    fn need_ballast(&mut self) -> Result<(), String> {
        let dead = match self.ballast.as_mut() { None => true, Some(b) => !b.alive() };
        if !dead { return Ok(()); }
        if self.ballast.is_some() {
            self.restarts += 1;
            let _ = self.j.restore();
            if self.restarts > 2 { return Err("ballast stopped by the brakes repeatedly: load phase ends here".into()); }
        } else { eprintln!("\nbuilding memory pressure (with fragmented free memory) ..."); }
        let b = Ballast::start(&self.c.exe)?;
        eprintln!("ballast holds {} MiB; {} MiB left available", b.held_mib, bench::meminfo_kb("MemAvailable:").unwrap_or(0) / 1024);
        self.ballast = Some(b);
        Ok(())
    }
    /// One run of `cfg` (all other knobs at their reference). None = a value could not be written.
    fn run(&mut self, phase: Phase, b: Benches, cfg: &Cfg, note: &str) -> Result<Option<Done>, String> {
        if STOP.load(Ordering::SeqCst) { let _ = self.j.restore(); return Err("interrupted".into()); }
        if phase == Phase::Load { self.need_ballast()?; }
        // No memory pressure behind the storage suite: the ballast would turn it into a swap test.
        if phase != Phase::Load { if let Some(b) = self.ballast.take() { b.stop(); } }
        self.runs += 1;
        let names: Vec<String> = cfg.iter().take(3).map(|(k, v)| format!("{k}={v}")).collect();
        eprint!("\r[{:>3} runs {:>4.1} min] {:<4} {:<64}", self.runs, self.t0.elapsed().as_secs_f64() / 60.0, phase.name(),
                if cfg.is_empty() { format!("reference {note}") } else { format!("{} change(s): {}{}", cfg.len(), names.join(" "), if cfg.len() > 3 { " ..." } else { "" }) });
        let _ = std::io::stderr().flush();
        let mut todo: Vec<(String, String)> = self.base.iter().filter(|(k, _)| !cfg.iter().any(|(c, _)| c == k)).cloned().collect();
        // Context pseudo-knobs (the heat state) are conditions of the run, never written.
        todo.extend(cfg.iter().filter(|(k, _)| !model::is_ctx(k)).cloned());
        // The elevator first: switching it resets nr_requests and (bfq) writeback throttling.
        todo.sort_by_key(|(k, _)| k != "blk.scheduler");
        for (k, v) in &todo {
            if let Err(e) = apply(&mut self.j, k, v, self.rate, self.ram) {
                eprintln!("\n{k} = {v}: {e} — run skipped");
                let _ = self.j.restore();
                return Ok(None);
            }
        }
        // The run in flight survives a hard hang: centurion-boot-guard turns it into an unsafe set on the next boot.
        centurion_helpers::stability::inflight_set(phase.name(), &todo);
        let temp = self.conditions(phase, is_hot(cfg));
        let disp = bench::display_state();
        let skip: Vec<u32> = self.ballast.as_ref().map(|b| b.pid).into_iter().collect();
        let (fg, t1) = (bench::Foreign::snapshot(&skip), Instant::now());
        let r = match phase { Phase::Idle => run_idle(b, self.c).map(|s| (s, false)), Phase::Load => run_load(b, self.c),
                              Phase::Io => run_io(self.c).map(|s| (s, false)), Phase::Dev => run_dev(self.c).map(|s| (s, false)) };
        let busy = fg.busy_cpus(&skip, t1.elapsed().as_secs_f64());
        for e in self.j.restore() { eprintln!("\nrestore: {e}"); }
        // Other programs keeping more than 0.6 CPU busy disturb the figures: such a run counts less.
        let mut w = if busy > 0.6 { (0.6 / busy).clamp(0.1, 1.0) } else { 1.0 };
        if w < 1.0 { self.busy += 1; if self.busy <= 3 || self.busy % 10 == 0 { eprintln!("\nother programs kept {busy:.1} CPU(s) busy: run counted at {:.0} % ({} such run(s))", w * 100.0, self.busy); } }
        // The display changed under the run (dimmed, blanked, brightness keys): its power is not comparable.
        match &self.disp0 {
            None => self.disp0 = Some(disp),
            Some(d0) if *d0 != disp => {
                w *= 0.2;
                self.disp_off += 1;
                if self.disp_off <= 2 { eprintln!("\nthe display changed during the session (brightness / blanking: {disp}): such runs count 20 % - turn off screen dimming and blanking for a calibration"); }
            }
            _ => {}
        }
        match r { Ok((s, oom)) => Ok(Some(Done { s, oom, w, t: now(), temp })), Err(e) => { eprintln!("\n{e}"); Ok(None) } }
    }
    /// Thermal conditions before a run. Hot: heat soak (every CPU busy; a top-up when the previous run
    /// was hot as well). Cool: wait until the die is back near the phase's cool start or has stopped
    /// cooling; the phase's first cool run sets that start. Returns the die temperature the run starts at.
    fn conditions(&mut self, phase: Phase, hot: bool) -> Option<f64> {
        if phase == Phase::Io { return bench::cpu_temp_c(); }
        if hot {
            let top_up = self.last_hot.map_or(false, |t| t.elapsed() < Duration::from_secs(8));
            bench::preheat(if top_up { ms(self.c, 4000) } else { Duration::from_secs_f64(self.c.heat_secs) });
            self.last_hot = Some(Instant::now());
            return bench::cpu_temp_c();
        }
        let pi = phase.idx();
        match self.cool[pi] {
            None => {
                let (t, w) = bench::thermal_gate(f64::NEG_INFINITY, Duration::from_secs(20));
                self.gate_s += w;
                self.cool[pi] = t;
                t
            }
            Some(base) => {
                let (margin, max) = if phase == Phase::Dev { (2.0, 15.0) } else { (4.0, 8.0) };
                let (t, w) = bench::thermal_gate(base + margin, Duration::from_secs_f64(max * self.c.scale));
                self.gate_s += w;
                t
            }
        }
    }
}

/// A run that caused an OOM kill: halve its changes until the smallest set that still does is found.
fn oom_cull(cal: &mut Calibration, rn: &mut Runner, phase: Phase, b: Benches, cfg: &Cfg, left: &mut usize, refs: &BTreeMap<String, String>, kernel: &str) -> Result<(), String> {
    if cfg.len() == 1 {
        let (k, v) = &cfg[0];
        cal.add(k, refs.get(k).map_or("", String::as_str), v, phase, None, true, now(), kernel);
        cal.add_unsafe_set(cfg.clone());
        eprintln!("\n{k} = {v} caused an OOM kill: never picked");
        return Ok(());
    }
    let mid = cfg.len() / 2;
    let mut found = false;
    for half in [cfg[..mid].to_vec(), cfg[mid..].to_vec()] {
        if *left == 0 { break; }
        *left -= 1;
        if let Some(Done { oom: true, .. }) = rn.run(phase, b, &half, "(OOM search)")? { found = true; oom_cull(cal, rn, phase, b, &half, left, refs, kernel)?; }
    }
    if !found { cal.add_unsafe_set(cfg.clone()); eprintln!("\n{} changes together caused an OOM kill: that combination is never picked", cfg.len()); }
    Ok(())
}

struct Analysis { risk: f64, doubt_pairs: usize, optima: Vec<(Goal, Cfg, f64, f64)>, picks: Vec<Cfg>, r2: f64, cover: f64 }

/// A goal's objective weights in one phase: the IO phase's objectives count with the storage
/// weight, exactly as autotune weighs them, so the margin means the same in both.
fn goal_wts(g: Goal, phase: Phase) -> [f64; 4] {
    let w = Weights::for_goal(g);
    if phase == Phase::Dev { return [0.0, 0.0, w.power, 0.0]; }
    let k = if phase == Phase::Io { w.storage } else { 1.0 };
    [w.latency * k, w.throughput * k, w.power * k, w.footprint * k]
}

/// Every goal's utility over one phase's per-objective fits (no refit per goal).
fn goal_models(space: &Arc<model::Space>, fits: &[Option<Arc<model::Fit>>; 4], phase: Phase) -> Vec<(Goal, Model)> {
    Goal::ALL.iter().filter_map(|&g| Model::new(space.clone(), fits.clone(), goal_wts(g, phase)).map(|m| (g, m))).collect()
}

/// Per goal: the best combination, how much doubt is left in its decisions (and, with
/// `pair_k`, in the interactions among the knobs that matter), and which experiments would
/// remove the most of it. `crowd`: largest random experiment in the candidate pool.
fn analyse(models: Vec<(Goal, Model)>, phase: Phase, factors: &[Factor], bad: &[Cfg], want: usize, pair_k: usize, crowd: usize, rng: &mut Rng, base: &Cfg) -> Option<Analysis> {
    let (r2, cover) = (models.first()?.1.r2(), models.first()?.1.cover90());
    let heat_cfg: Cfg = base.iter().cloned().chain(std::iter::once(("ctx.heat".to_owned(), "hot".to_owned()))).collect();
    let single = |m: Model, ctxs: Vec<(f64, Cfg)>| {
        let (mut idle, mut load, mut io, mut dev) = (None, None, None, None);
        match phase { Phase::Idle => idle = Some(m), Phase::Load => load = Some(m), Phase::Io => io = Some(m), Phase::Dev => dev = Some(m) }
        Joint { idle, load, io, dev, share: 0.5, ctxs }
    };
    // Decisions are judged in the session's context, as a mixture of cool and heat-soaked running by
    // the goal's heat share once the log holds hot runs; the optima are reported (and confirmed) cool.
    let joints: Vec<(Goal, Joint, Joint)> = models.into_iter().map(|(g, m)| {
        let h = if m.space.has("ctx.heat") { Weights::for_goal(g).heat } else { 0.0 };
        let cool: Vec<(f64, Cfg)> = if base.is_empty() { Vec::new() } else { vec![(1.0, base.clone())] };
        let mix = if h > 0.0 { vec![(1.0 - h, base.clone()), (h, heat_cfg.clone())] } else { cool.clone() };
        (g, single(m.clone(), mix), single(m, cool))
    }).collect();
    let keys: Vec<String> = factors.iter().filter(|f| !model::is_ctx(&f.key)).map(|f| f.key.clone()).collect();
    let cands: Vec<Vec<(String, f64)>> = factors.iter().filter(|f| !model::is_ctx(&f.key)).map(|f| {
        let mut v = vec![(f.reference.clone(), 0.0)];
        for x in &f.values { v.push((x.clone(), model::modest_cost(&f.reference, x))); }
        v
    }).collect();
    let probs: Vec<Problem> = joints.iter().map(|(_, j, _)| {
        let mut p = Problem::new(j, keys.clone(), cands.clone(), Vec::new(), model::RISK_Z, MARGIN, bad.to_vec());
        p.crowd = crowd;
        p
    }).collect();
    let (mut risk, mut doubt_pairs, mut optima, mut targets, mut pool) = (0.0, 0, Vec::new(), Vec::new(), Vec::<Cfg>::new());
    for ((g, _, cool), p) in joints.iter().zip(&probs) {
        let mut sel = p.optimize(&vec![0; keys.len()], rng, 6);
        p.prune(&mut sel, model::RISK_Z, MARGIN);
        let mut t = p.targets(&sel, MARGIN);
        risk += t.iter().map(|t| t.pwrong).sum::<f64>();
        let pt = p.pair_targets(&sel, MARGIN, pair_k);
        risk += 0.5 * pt.iter().map(|t| t.pwrong).sum::<f64>();
        doubt_pairs += pt.iter().filter(|t| t.pwrong > 0.2).count();
        t.extend(pt);
        pool.extend(p.pool(&sel, &t, 40, rng));
        let cfg = p.cfg(&sel);
        let (mu, var) = cool.eval(&cfg);
        optima.push((*g, cfg, mu, var.sqrt()));
        targets.push(t);
    }
    pool.retain(|c| !c.is_empty());
    pool.sort(); pool.dedup();
    let mut score = vec![0.0; pool.len()];
    for (p, t) in probs.iter().zip(&targets) { for (i, a) in p.acquisition(t, &pool).into_iter().enumerate() { score[i] += a; } }
    let mut picks = Vec::new();
    for _ in 0..want {
        let Some((bi, &bv)) = score.iter().enumerate().max_by(|a, b| a.1.total_cmp(b.1)) else { break };
        if bv <= 0.0 { break; }
        let c = pool[bi].clone();
        for (i, p) in pool.iter().enumerate() { let sh = p.iter().filter(|kv| c.contains(kv)).count(); if sh > 0 { score[i] *= 0.5f64.powi(sh as i32); } }
        score[bi] = -1.0;
        picks.push(c);
    }
    Some(Analysis { risk, doubt_pairs, optima, picks, r2, cover })
}

/// Hot runs of a batch go last, so the cool ones are not measured right after a heat soak.
fn cool_first(v: Vec<Cfg>) -> Vec<Cfg> {
    let (mut cool, hot): (Vec<Cfg>, Vec<Cfg>) = v.into_iter().partition(|c| !is_hot(c));
    cool.extend(hot);
    cool
}

fn save(cal: &Calibration) {
    let _ = centurion_helpers::secure_dir(centurion_helpers::defaults::DIR);
    let _ = centurion_helpers::write_root_file(calib::FILE, &serde_json::to_vec(&cal.to_json()).unwrap());
}

/// Benchmarks of a phase's runs: idle/load measure CPU, memory and (idle) power on every run,
/// whatever the knobs, so every goal is decided from the same data; the IO phase runs the
/// storage suite alone.
fn union_benches(phase: Phase, its: &[&Item]) -> Benches {
    if phase == Phase::Io { return Benches { io: true, ..Default::default() }; }
    let mut b = Benches { cpu_mem: true, cpu: true, ..Default::default() };
    for i in its { b.io |= i.benches.io; b.idle |= i.benches.idle; }
    b
}

/// A numeric value on a knob's own grid: kHz caps in 100 MHz steps, big counts in thousands,
/// the dirty window in hundredths; None = not a numeric knob.
fn round_dose(key: &str, x: f64) -> Option<String> {
    if key == "vm.dirty" { return Some(format!("{:.2}", x).trim_end_matches('0').trim_end_matches('.').to_owned()); }
    let t = tune::find(key)?;
    let tune::Kind::Int { min, max } = &t.kind else { return None };
    let step = if x >= 1e6 { 100_000.0 } else if x >= 1e4 { 1000.0 } else if x >= 200.0 { 10.0 } else { 1.0 };
    Some((((x / step).round() * step) as i64).clamp(*min, *max).to_string())
}

/// Dose refinement: for the numeric knobs of the predicted optima, the midpoints (geometric
/// where both ends are positive) between the chosen dose and its measured neighbours become
/// new levels, each tried inside that optimum. At most `max` experiments; `factors` grow.
fn ladder(factors: &mut [Factor], optima: &[(Goal, Cfg, f64, f64)], bad: &[Cfg], max: usize) -> Vec<Cfg> {
    let mut out: Vec<Cfg> = Vec::new();
    for (_, cfg, _, _) in optima {
        for (k, v) in cfg {
            if out.len() >= max { return out; }
            let Some(f) = factors.iter_mut().find(|f| f.key == *k) else { continue };
            let Ok(x) = v.parse::<f64>() else { continue };
            let mut lv: Vec<f64> = std::iter::once(&f.reference).chain(&f.values).filter_map(|s| s.parse().ok()).collect();
            if lv.len() < 2 { continue; }
            lv.sort_by(|a, b| a.total_cmp(b)); lv.dedup();
            let Some(i) = lv.iter().position(|l| (l - x).abs() < 1e-9) else { continue };
            for n in [i.checked_sub(1), Some(i + 1)].into_iter().flatten().filter(|&n| n < lv.len()) {
                let (a, b) = (lv[n].min(x), lv[n].max(x));
                let m = if a > 0.0 { (a * b).sqrt() } else { (a + b) / 2.0 };
                let Some(mv) = round_dose(k, m) else { continue };
                let Ok(mx) = mv.parse::<f64>() else { continue };
                if lv.iter().any(|l| (l - mx).abs() < 1e-9) || f.values.contains(&mv) || mv == f.reference { continue; }
                let c: Cfg = cfg.iter().map(|(kk, vv)| (kk.clone(), if kk == k { mv.clone() } else { vv.clone() })).collect();
                if bad.iter().any(|u| u.iter().all(|kv| c.contains(kv))) || out.contains(&c) { continue; }
                f.values.push(mv);
                out.push(c);
                if out.len() >= max { return out; }
            }
        }
    }
    out
}

/// Interactions the model is sure about, pairs and (when modelled) triples.
fn report_interactions(m: &Model, k: usize, indent: &str) {
    let orders: Vec<usize> = if m.space.order >= 3 { vec![2, 3] } else { vec![2] };
    for ord in orders {
        for i in m.interactions(ord, k).into_iter().filter(|i| i.mean.abs() > 1.5 * i.sd) {
            let po: Vec<String> = ["lat", "thr", "pwr", "mem"].iter().zip(i.per_obj).filter_map(|(n, x)| x.filter(|x| x.abs() >= 0.002).map(|x| format!("{n} {:+.1}%", x * 100.0))).collect();
            println!("{indent}{} {:<64} {:+.3} ± {:.3} {:<8} {}", if ord == 2 { "pair  " } else { "triple" }, i.label(), i.mean, i.sd, i.kind(), po.join(" "));
        }
    }
}

struct Design<'a> { phase: Phase, bs: Benches, sess: u64, n_total: usize, kernel: &'a str, refs: &'a BTreeMap<String, String>, runs: Vec<Run>, oom_left: usize,
                    t0: Instant, secs: f64, spent: f64, meta: Meta }

impl Design<'_> {
    fn exec(&mut self, cal: &mut Calibration, rn: &mut Runner, cfg: &Cfg) -> Result<(), String> {
        let t = Instant::now();
        match rn.run(self.phase, self.bs, cfg, "")? {
            Some(Done { s, oom: false, w, t, temp }) => { let order = self.runs.len(); self.runs.push(Run { cfg: cfg.clone(), s, order, t, w, temp }); }
            Some(Done { oom: true, .. }) => {
                let start = self.oom_left.min(6);
                let mut left = start;
                // Heat is a condition, not a cause of an OOM kill: the knobs alone are bisected.
                let knobs: Cfg = cfg.iter().filter(|(k, _)| !model::is_ctx(k)).cloned().collect();
                if !knobs.is_empty() { oom_cull(cal, rn, self.phase, self.bs, &knobs, &mut left, self.refs, self.kernel)?; }
                self.oom_left -= start - left;
            }
            None => {}
        }
        self.spent += t.elapsed().as_secs_f64();
        Ok(())
    }
    /// Runs that still fit in the phase's time (measured cost per run, not the estimate).
    fn left(&self) -> usize {
        if !self.secs.is_finite() { return usize::MAX; }
        let per = if self.runs.len() >= 4 { self.spent / self.runs.len() as f64 } else { self.secs / self.n_total as f64 };
        ((self.secs - self.t0.elapsed().as_secs_f64()).max(0.0) / per.max(0.5)) as usize
    }
    fn commit(&self, cal: &mut Calibration) {
        cal.put_session(self.phase, self.sess, compute_rows(&self.runs, self.phase, self.sess, self.kernel, self.n_total, &self.meta));
        for (k, w) in ref_levels(&self.runs, self.phase, self.meta.src) { cal.ref_w.insert(k, (w * 100.0).round() / 100.0); }
        save(cal);
    }
}

fn phase_analysis(cal: &Calibration, phase: Phase, factors: &[Factor], dp: &Depth, want: usize, rng: &mut Rng, cx: &str) -> Option<Analysis> {
    let pf = cal.phase_fit(phase)?;
    let base = cal.ctx_cfg(phase, cx);
    analyse(goal_models(&pf.set.space, &pf.fits, phase), phase, factors, &cal.unsafe_sets, want, dp.pairs, dp.crowd, rng, &base)
}

fn design_phase(cal: &mut Calibration, rn: &mut Runner, phase: Phase, its: &[&Item], budget: f64, dp: &Depth, kernel: &str, seed: u64, confirm: bool,
                refs: &BTreeMap<String, String>) -> Result<(), String> {
    let name = phase.name();
    let mut factors: Vec<Factor> = its.iter().map(|i| Factor { key: i.key.clone(), reference: i.reference.clone(), values: i.values.clone() }).collect();
    let bs = union_benches(phase, its);
    // Heat as a design factor: heat-soaked runs, balanced against every knob like a knob itself, so
    // the model learns which knob effects change once boost headroom is spent and leakage is up.
    let heat = matches!(phase, Phase::Idle | Phase::Load) && rn.c.heat.unwrap_or_else(|| dp.heat());
    let heat_f = Factor { key: "ctx.heat".into(), reference: "cool".into(), values: vec!["hot".into()] };
    let design_factors = |fs: &[Factor]| -> Vec<Factor> { let mut v = fs.to_vec(); if heat { v.push(heat_f.clone()); } v };
    // vm.dirty's reference is window "1" (what autotune reads it as), not the live limits: every run of
    // this phase starts from it, so the reference runs are what the label says.
    let dirty_ok = tune::find("vm.dirty_bytes").map_or(false, |t| !tune::files(t).is_empty());
    rn.base = factors.iter().filter(|f| f.key == "vm.dirty" && dirty_ok).map(|f| (f.key.clone(), f.reference.clone())).collect();
    let cost = est_secs(phase, bs, rn.c.scale).max(1.0);
    let n_total = ((budget / cost) as usize).clamp(12, dp.cap);
    let n_init = ((n_total as f64 * dp.init) as usize).clamp(8, n_total);
    let sess = now() + phase.idx() as u64;
    let mut rng = Rng::new(seed ^ sess);
    let mut d = Design { phase, bs, sess, n_total, kernel, refs, runs: Vec::new(), oom_left: 12, t0: Instant::now(), secs: budget, spent: 0.0,
                         meta: Meta { src: if phase == Phase::Dev || rn.c.battery { calib::PowerSrc::Battery } else { calib::PowerSrc::Rapl }, cx: rn.c.cx.clone() } };
    rn.disp0 = None;
    if heat { eprintln!("{name}: heat is a design factor (heat-soaked runs: {:.0} s of every CPU busy first)", rn.c.heat_secs); }
    let prior: Vec<Cfg> = cal.rows.iter().filter(|r| r.phase == phase).map(|r| r.cfg.clone()).collect();
    eprintln!("\n{name} design ({}): {} knob(s), up to {n_total} runs of ~{cost:.0} s ({n_init} space-filling{}, then chosen by decision doubt{})",
              dp.name, factors.len(), if dp.mix.crowd > 0.0 { format!(", {:.0} % crowded ({}-{} changes)", dp.mix.crowd * 100.0, dp.mix.crowd_k.0, dp.mix.crowd_k.1) } else { String::new() },
              if dp.pairs > 0 { " and interaction doubt" } else { "" });
    if !prior.is_empty() { eprintln!("{name}: {} earlier run(s) of this phase are in the log: the new runs fill their gaps", prior.len()); }
    let none: Cfg = Vec::new();

    // 1. Space-filling: balanced over knobs, values and knob pairs, crowded runs with the depth.
    d.exec(cal, rn, &none)?;
    let mut since = 0;
    let init: Vec<Cfg> = model::design(&design_factors(&factors), n_init, &mut rng, &dp.mix, &prior).into_iter().filter(|c| !cal.unsafe_cfg(c)).collect();
    'init: for chunk in init.chunks(6) {
        for cfg in cool_first(chunk.to_vec()) {
            if d.secs.is_finite() && d.t0.elapsed().as_secs_f64() > d.secs * (dp.init + 0.1) { break 'init; }
            d.exec(cal, rn, &cfg)?;
            since += 1;
            if since >= 6 { d.exec(cal, rn, &none)?; since = 0; }
        }
    }
    d.exec(cal, rn, &none)?;
    d.commit(cal);
    cal.retune(phase, 2);
    save(cal);

    // 2. Sequential: each batch where it removes the most decision (and interaction) doubt.
    let confirm_runs = if confirm { 2 + 4 * dp.reps.min(2) } else { 0 };
    let reserve = confirm_runs + if dp.ladder > 0 { dp.ladder * 2 + 1 } else { 0 };
    let (mut batches, mut exploring) = (0, false);
    while d.runs.len() + dp.batch + 1 + reserve <= dp.cap && d.left() >= dp.batch + 1 + reserve {
        let Some(an) = phase_analysis(cal, phase, &factors, dp, dp.batch, &mut rng, &rn.c.cx) else { break };
        eprintln!("\n{name}: {} runs, decision doubt {:.2}{}, model R² {:.2}, 90 % interval coverage {:.0} %", d.runs.len(), an.risk,
                  if dp.pairs > 0 { format!(" ({} doubtful interaction(s))", an.doubt_pairs) } else { String::new() }, an.r2, an.cover * 100.0);
        let settled = an.risk < 0.35 && an.doubt_pairs == 0;
        if settled && !dp.explore { eprintln!("{name}: decisions are settled, stopping early"); break; }
        // Settled (progressive) or nothing to ask: the time goes to coverage the log still lacks.
        let picks = if settled || an.picks.is_empty() {
            if settled && !exploring { eprintln!("{name}: decisions are settled; the rest of the session fills gaps ({})", dp.name); exploring = true; }
            let seen: Vec<Cfg> = prior.iter().cloned().chain(d.runs.iter().map(|r| r.cfg.clone())).collect();
            model::design(&design_factors(&factors), dp.batch, &mut rng, &dp.mix, &seen).into_iter().filter(|c| !cal.unsafe_cfg(c)).collect()
        } else {
            // Doubt-driven runs: part of them heat-soaked, so the hot side keeps up with the cool one.
            an.picks.into_iter().map(|mut c| { if heat && rng.unit() < 0.3 { c.push(("ctx.heat".into(), "hot".into())); } c }).collect()
        };
        for cfg in cool_first(picks) { if !cal.unsafe_cfg(&cfg) { d.exec(cal, rn, &cfg)?; } }
        d.exec(cal, rn, &none)?;
        d.commit(cal);
        batches += 1;
        if batches % 4 == 0 { cal.retune(phase, 1); save(cal); }
    }

    // 3. Dose refinement: midpoints of numeric ladders around the chosen doses.
    if dp.ladder > 0 && d.left() >= dp.ladder + confirm_runs {
        if let Some(an) = phase_analysis(cal, phase, &factors, dp, 0, &mut rng, &rn.c.cx) {
            let tries = ladder(&mut factors, &an.optima, &cal.unsafe_sets, dp.ladder);
            if !tries.is_empty() {
                eprintln!("\n{name}: refining {} dose(s) between measured levels", tries.len());
                for (i, cfg) in tries.iter().enumerate() {
                    d.exec(cal, rn, cfg)?;
                    if dp.reps >= 3 { d.exec(cal, rn, cfg)?; }
                    if i % 3 == 2 { d.exec(cal, rn, &none)?; }
                }
                d.exec(cal, rn, &none)?;
                d.commit(cal);
            }
        }
    }

    // 4. Confirmation: measure what the models believe is best, per goal.
    if confirm && (d.left() >= 4 || !d.secs.is_finite()) {
        cal.retune(phase, 1);
        if let Some(an) = phase_analysis(cal, phase, &factors, dp, 0, &mut rng, &rn.c.cx) {
            let mut uniq: Vec<(Cfg, Vec<(Goal, f64, f64)>)> = Vec::new();
            for (g, cfg, mu, sd) in an.optima.iter().filter(|o| !o.1.is_empty()) {
                match uniq.iter_mut().find(|(c, _)| c == cfg) { Some(u) => u.1.push((*g, *mu, *sd)), None => uniq.push((cfg.clone(), vec![(*g, *mu, *sd)])) }
            }
            let reps = if d.secs.is_finite() { dp.reps.min((d.left().saturating_sub(2) / uniq.len().max(1)).max(1)) } else { dp.reps };
            if !uniq.is_empty() {
                eprintln!("\n{name}: confirming {} predicted optimum(s), {reps} run(s) each", uniq.len());
                d.exec(cal, rn, &none)?;
                for r in 0..reps { for (cfg, _) in &uniq { d.exec(cal, rn, cfg)?; } if r % 2 == 1 { d.exec(cal, rn, &none)?; } }
                d.exec(cal, rn, &none)?;
                d.commit(cal);
                let rows = compute_rows(&d.runs, phase, sess, kernel, n_total, &d.meta);
                for (cfg, gs) in &uniq {
                    for (g, mu, sd) in gs {
                        let ws = goal_wts(*g, phase);
                        let obs: Vec<f64> = rows.iter().filter(|r| &r.cfg == cfg).map(|r| (0..4).map(|o| if r.y[o].is_nan() { 0.0 } else { ws[o] * r.y[o] }).sum()).collect();
                        if obs.is_empty() { continue; }
                        let m = obs.iter().sum::<f64>() / obs.len() as f64;
                        let tol = 2.0 * (sd * sd + 0.02f64.powi(2) / obs.len() as f64).sqrt();
                        eprintln!("  {:<30} {} change(s): predicted {:+.3} ± {:.3}, measured {:+.3} ({} runs) {}", g.label(), cfg.len(), mu, sd, m, obs.len(),
                                  if (m - mu).abs() <= tol { "as predicted" } else { "SURPRISE (now part of the data)" });
                    }
                }
            }
        }
    }
    cal.retune(phase, 2);
    if d.runs.len() >= 10 { cal.note_strategy(phase, dp.name); }
    save(cal);
    Ok(())
}

/// Phases with at least two testable knobs and their share of the time budget (each knob gets a similar number of runs).
fn split(items: &[Item], scale: f64, budget: f64) -> Vec<(Phase, Vec<&Item>, f64)> {
    let mut v: Vec<(Phase, Vec<&Item>, f64)> = Phase::ALL.into_iter()
        .map(|ph| (ph, items.iter().filter(|i| i.phase == ph).collect::<Vec<_>>(), 0.0)).filter(|(_, its, _)| its.len() >= 2).collect();
    let w: Vec<f64> = v.iter().map(|(ph, its, _)| its.len() as f64 * est_secs(*ph, union_benches(*ph, its), scale)).collect();
    let sum: f64 = w.iter().sum();
    for (e, w) in v.iter_mut().zip(&w) { e.2 = if budget.is_finite() { budget * w / sum } else { f64::INFINITY }; }
    v
}

fn phase_factors(its: &[&Item]) -> Vec<Factor> { its.iter().map(|i| Factor { key: i.key.clone(), reference: i.reference.clone(), values: i.values.clone() }).collect() }

fn design_main(cal: &mut Calibration, items: &[Item], ctx: &Ctx, rate: u64, ram: u64, kernel: &str, budget: f64, dp: &Depth, progressive: bool, seed: u64, confirm: bool) {
    let refs: BTreeMap<String, String> = items.iter().map(|i| (i.key.clone(), i.reference.clone())).collect();
    for (k, r) in &refs {
        if let Some(old) = cal.refs.get(k) { if old != r { eprintln!("{k}: reference changed ({old} -> {r}): earlier design rows of this knob are dropped"); cal.forget_key(k); } }
        cal.refs.insert(k.clone(), r.clone());
    }
    let phases = split(items, ctx.scale, budget);
    if phases.is_empty() { eprintln!("nothing to design: fewer than two testable knobs"); return; }
    let mut rn = Runner { j: Journal { entries: Vec::new() }, c: ctx, rate, ram, ballast: None, restarts: 0, runs: 0, t0: Instant::now(), busy: 0, base: Vec::new(),
                          cool: [None; 4], last_hot: None, disp0: None, disp_off: 0, gate_s: 0.0 };
    for (ph, its, secs) in &phases {
        let pdp = if progressive {
            let p = progress(cal, *ph, &phase_factors(its));
            eprintln!("\n{}: progressive stage {:?} - {}\n  log: {}", ph.name(), p.stage(), p.stage().what(), p.line());
            Depth::progressive(p.stage(), its.len(), p.polish)
        } else { *dp };
        if let Err(e) = design_phase(cal, &mut rn, *ph, its, *secs, &pdp, kernel, seed, confirm, &refs) { eprintln!("\n{e}"); if STOP.load(Ordering::SeqCst) { break; } }
    }
    for e in rn.j.restore() { eprintln!("restore: {e}"); }
    if let Some(b) = rn.ballast.take() { b.stop(); }
    centurion_helpers::stability::inflight_clear();
    cal.prune_contexts(&ctx.cx);
    if rn.gate_s >= 10.0 { eprintln!("\nwaited {:.0} s in all for the die to cool back to its starting temperature between runs", rn.gate_s); }
    save(cal);
    eprintln!();
    if STOP.load(Ordering::SeqCst) { println!("interrupted: measured runs are saved, everything is restored"); }
    for ph in Phase::ALL {
        let its: Vec<&Item> = items.iter().filter(|i| i.phase == ph).collect();
        let Some(pf) = cal.phase_fit(ph) else { continue };
        let factors: Vec<Factor> = its.iter().map(|i| Factor { key: i.key.clone(), reference: i.reference.clone(), values: i.values.clone() }).collect();
        let Some(an) = analyse(goal_models(&pf.set.space, &pf.fits, ph), ph, &factors, &cal.unsafe_sets, 0, dp.pairs, dp.crowd, &mut Rng::new(seed), &cal.ctx_cfg(ph, &ctx.cx)) else { continue };
        println!("{}: {} run(s), model R² {:.2}, 90 % interval coverage {:.0} %, decision doubt {:.2}, interactions up to {}", ph.name(),
                 pf.set.nrows(), an.r2, an.cover * 100.0, an.risk, if pf.set.space.order >= 3 { "triples" } else { "pairs" });
        for (g, cfg, mu, sd) in &an.optima { println!("  {:<30} {} change(s), predicted {:+.3} ± {:.3}", g.label(), cfg.len(), mu, sd); }
        if let Some(m) = cal.phase_model(ph, [1.0; 4]) { report_interactions(&m, 5, "  "); }
    }
    if progressive {
        for (ph, its, _) in &phases {
            let p = progress(cal, *ph, &phase_factors(its));
            println!("{}: next lean session: stage {:?} ({})", ph.name(), p.stage(), p.line());
        }
    }
    println!("signature: {} design run(s) stored. `centurion-calibrate --show` for effects and interactions; run again to add evidence.", cal.rows.len());
    println!("autotune decides all calibrated knobs jointly from it on its next run (GUI Autotune or centurion-autotune <goal>).");
}

fn show_model(c: &Calibration) {
    if c.rows.is_empty() { return; }
    println!("\nexperiment log: {} run(s), {} unsafe combination(s)", c.rows.len(), c.unsafe_sets.len());
    for ph in Phase::ALL {
        let Some(m) = c.phase_model(ph, [1.0; 4]) else { continue };
        println!("  {:<5} {} knob(s), {} run(s), noise {:.3}, model R² {:.2}, 90 % interval coverage {:.0} %, interactions up to {}", ph.name(),
                 m.space.factors.len(), m.nobs(), m.noise(), m.r2(), m.cover90() * 100.0, if m.space.order >= 3 { "triples" } else { "pairs" });
        report_interactions(&m, 8, "        ");
    }
}

fn pct(x: Option<f64>) -> String { x.map_or("    n/a".into(), |v| format!("{:+6.1}%", v * 100.0)) }

fn show(c: &Calibration) {
    let fp = &c.fingerprint;
    println!("signature of {} · {} · {} MiB · BIOS {}\npower {}\n", fp.product, fp.cpu, fp.ram_mib, fp.bios,
             if c.on_battery { "last measured on battery (whole machine)" } else { "measured on AC (RAPL: CPU package only)" });
    println!("  {:<30} {:<24} {:<5} {:>8} {:>8} {:>8} {:>8} {:>5}", "knob", "value vs ref", "phase", "latency", "thruput", "power", "memory", "n");
    for (k, gs) in &c.keys {
        for g in gs {
            for (v, s) in &g.values {
                for ph in Phase::ALL {
                    let Some(m) = c.get_phase(k, &g.reference, v, ph) else { continue };
                    println!("  {k:<30} {:<24} {:<5} {} {} {} {} {:>5.1}{}", format!("{v} vs {}", g.reference),
                             ph.name(), pct(m.lat), pct(m.thr), pct(m.pwr), pct(m.mem), m.n,
                             if s.unsafe_ { "  UNSAFE (OOM kill)" } else { "" });
                }
            }
        }
    }
    show_model(c);
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    // Disposable children: no machine checks, just measure / hold memory.
    if args.first().map(String::as_str) == Some("__probe") {
        println!("{}", bench::probe_main(args.get(1).and_then(|s| s.parse().ok()).unwrap_or(512)));
        return;
    }
    if args.first().map(String::as_str) == Some("__job") {
        bench::job_main(args.get(1).and_then(|s| s.parse().ok()).unwrap_or(1_000_000));
        return;
    }
    if args.first().map(String::as_str) == Some("__ballast") {
        let n = |i: usize, d: u64| args.get(i).and_then(|s| s.parse().ok()).unwrap_or(d);
        bench::ballast_main(n(1, 0) as usize, n(2, 1) as usize, n(3, 1024), n(4, 0) == 1);
    }
    centurion_helpers::init();
    let flag = |f: &str| args.iter().any(|a| a == f);
    let opt = |f: &str| args.iter().position(|a| a == f).and_then(|i| args.get(i + 1)).cloned();
    if flag("--status") { println!("{}", boot_status()); return; }
    let mut cal = Calibration::load().unwrap_or_else(|| Calibration::new(calib::Fingerprint::current()));
    if flag("--show") {
        if cal.keys.is_empty() && cal.rows.is_empty() { println!("no signature yet (sudo centurion-calibrate)"); } else { show(&cal); }
        return;
    }
    let thorough = flag("--thorough");
    let (rounds, scale) = if thorough { (2, 1.6) } else { (1, 1.0) };
    let only: Option<Vec<String>> = opt("--only").map(|s| s.split(',').map(str::to_owned).collect());
    let phase = opt("--phase").unwrap_or_else(|| "both".into());
    let budget = if flag("--all") { f64::INFINITY } else { opt("--budget").and_then(|s| s.parse::<f64>().ok()).unwrap_or(15.0) * 60.0 };
    let ram = bench::meminfo_kb("MemTotal:").unwrap_or(0) * 1024;
    // The storage suite's disk: --dir, else the first of these on a local disk (not tmpfs).
    let dir = opt("--dir").unwrap_or_else(|| ["/var/tmp", "/var/cache", "/home", "/"].iter()
        .find(|d| bench::disk_of(d).is_ok()).unwrap_or(&"/var/tmp").to_string());
    let disk = bench::disk_of(&dir);
    let battery_now = bench::power_source() == "battery";
    let (items, skipped) = plan(rounds, scale, ram, &only, &phase, &cal, &disk, battery_now);
    let total_secs: f64 = items.iter().map(|i| i.secs).sum();
    let depth_opt = opt("--depth");
    if let Some(d) = &depth_opt { if !["lean", "deep", "max"].contains(&d.as_str()) { die("--depth: lean, deep or max"); } }
    let nf = Phase::ALL.iter().map(|ph| items.iter().filter(|i| i.phase == *ph).count()).max().unwrap_or(0);
    let dp = Depth::pick(budget / 60.0, nf, depth_opt.as_deref());
    // Short sessions without an explicit depth build on each other (see Stage).
    let progressive = depth_opt.is_none() && dp.name == "lean";
    if flag("--list") {
        for i in &items {
            println!("  {:<5} {:<30} ref {:<20} try {:<40} ~{:>4.0} s  measured {}x", i.phase.name(),
                     i.key, i.reference, i.values.join(","), i.secs, cal.coverage(&i.key, i.phase));
        }
        if flag("--oat") {
            println!("\n{} measurements, ~{:.0} min in total; not tested:", items.len(), total_secs / 60.0);
        } else {
            println!();
            if progressive {
                for (ph, its, _) in split(&items, scale, budget) {
                    let p = progress(&cal, ph, &phase_factors(&its));
                    let d = Depth::progressive(p.stage(), its.len(), p.polish);
                    println!("  {:<5} progressive stage {:?} ({}): {}\n        log: {}", ph.name(), p.stage(), d.name, p.stage().what(), p.line());
                }
                println!("  (lean sessions build on each other; --depth lean runs the fixed lean shape)");
            }
            if !progressive { println!("depth {}: space-filling {:.0} % of the runs ({:.0} % inside one cluster, {}-{} changes elsewhere{}), batches of {}, {} confirmation run(s) per optimum{}{}",
                     dp.name, dp.init * 100.0, dp.mix.cluster * 100.0, dp.mix.spread_k.0, dp.mix.spread_k.1,
                     if dp.mix.crowd > 0.0 { format!(", {:.0} % crowded with {}-{}", dp.mix.crowd * 100.0, dp.mix.crowd_k.0, dp.mix.crowd_k.1) } else { String::new() },
                     dp.batch, dp.reps, if dp.pairs > 0 { format!(", up to {} doubtful interactions chased", dp.pairs) } else { String::new() },
                     if dp.ladder > 0 { format!(", up to {} dose refinements", dp.ladder) } else { String::new() }); }
            for (ph, its, secs) in split(&items, scale, budget) {
                let cost = est_secs(ph, union_benches(ph, &its), scale).max(1.0);
                let logged = cal.rows.iter().filter(|r| r.phase == ph).count();
                println!("  {:<5} {} knob(s), ~{:.0} s per run, up to {} runs in {:.0} min ({} logged; interactions: all pairs{})", ph.name(), its.len(), cost,
                         ((secs / cost) as usize).clamp(12, dp.cap), secs.min(1e6) / 60.0, logged,
                         format!(", all triples from {} runs", 150.max(4 * its.len())));
            }
            match &disk { Ok(d) => println!("storage suite on {dir} ({d}); --dir PATH measures another disk"), Err(e) => println!("storage suite off: {e}") }
            println!("sequential design: every run changes several knobs; the runs stop early once the decisions are settled. Not tested:");
        }
        for (k, w) in &skipped { println!("  {k:<32} {w}"); }
        return;
    }
    if unsafe { libc::geteuid() } != 0 { die("needs root (sudo centurion-calibrate)"); }
    if flag("--restore") {
        fan_unlock();  // before the scene's originals (see Teardown)
        for e in centurion_helpers::calctx::restore_scene_power() { eprintln!("scene power context: {e}"); }
        centurion_helpers::stability::inflight_clear();
        if centurion_helpers::calibration_hold().is_none() { let _ = std::fs::remove_file(centurion_helpers::CALIBRATION_HOLD); centurion_helpers::stability::record_calibration(false); }
        let mut j = Journal::load();
        let n = j.entries.len();
        let errs = j.restore();
        let left = j.entries.len();
        println!("restored {} of {n} file(s){}", n - left, if errs.is_empty() { String::new() } else { format!(", errors: {}", errs.join("; ")) });
        if left > 0 {
            if flag("--drop") {
                j.entries.clear();
                let _ = std::fs::remove_file(JOURNAL);
                println!("{left} original(s) that cannot be written back were dropped from the journal (a reboot returns them to their defaults)");
            } else {
                println!("{left} original(s) are kept in the journal; run --restore again, or --restore --drop to give them up");
            }
        }
        return;
    }
    if flag("--schedule-boot") || flag("--cancel-boot") {
        let user = std::env::var("SUDO_USER").ok().filter(|u| !u.is_empty() && u != "root");
        match centurion_helpers::calctx::schedule_boot(!flag("--cancel-boot"), opt("--sessions").and_then(|s| s.parse().ok()).unwrap_or(2),
                            opt("--budget").and_then(|s| s.parse().ok()).unwrap_or(15.0), opt("--scene").as_deref(), user.as_deref()) {
            Ok(m) => println!("{m}"),
            Err(e) => die(&e),
        }
        return;
    }
    if flag("--boot-run") { std::process::exit(boot_run()); }
    if let Some(i) = args.iter().position(|a| a == "--field") {
        let files: Vec<String> = args[i + 1..].iter().take_while(|a| !a.starts_with("--")).cloned().collect();
        match field_import(&mut cal, &files, opt("--game")) { Ok(m) => println!("{m}"), Err(e) => die(&e) }
        return;
    }
    if !Journal::load().entries.is_empty() { die("an interrupted run left changes: sudo centurion-calibrate --restore first"); }
    let state_active = std::fs::read_to_string(TUNE_STATE).ok().and_then(|s| serde_json::from_str::<Value>(&s).ok())
        .map_or(false, |v| v["baseline"].as_array().map_or(false, |a| !a.is_empty()));
    if state_active { die("an Optimizations preset is active: Restore originals first, so every knob is measured from the boot state"); }
    let _ = centurion_helpers::secure_dir("/run/centurion");
    if let Err(e) = centurion_helpers::secure_dir("/run/centurion/tune") { die(&format!("lock dir: {e}")); }
    let lock = std::fs::OpenOptions::new().create(true).write(true).open(TUNE_LOCK).unwrap_or_else(|e| die(&format!("lock: {e}")));
    if unsafe { libc::flock(std::os::unix::io::AsRawFd::as_raw_fd(&lock), libc::LOCK_EX | libc::LOCK_NB) } != 0 { die("Centurion is applying something; try again"); }
    // The lock is held for the whole run on purpose (a scene or game preset applied in the middle would
    // be measured as if it were the knob under test). Say who holds it, so tune-helper can answer at once
    // with the reason instead of "another tune operation is still running" after its 8 s wait.
    {
        let _ = lock.set_len(0);
        let _ = (&lock).write_all(format!("calibrate {}\n", std::process::id()).as_bytes());
    }
    // A signature of other hardware is set aside, not merged.
    if cal.keys.is_empty() && cal.rows.is_empty() && std::path::Path::new(calib::FILE).exists() {
        let _ = std::fs::rename(calib::FILE, format!("{}.other-{}", calib::FILE, now()));
    }
    unsafe {
        libc::signal(libc::SIGINT, on_signal as *const () as libc::sighandler_t);
        libc::signal(libc::SIGTERM, on_signal as *const () as libc::sighandler_t);
    }
    // Session conditions: nothing else changes the power context while this runs (calibration hold),
    // a scene's power context if asked, the fan held at full speed on AC, the battery reading's cadence.
    let mut td = Teardown::default();
    td.hold = take_hold("calibrating");
    centurion_helpers::stability::record_calibration(true);
    td.exposure = true;
    let scene = opt("--scene");
    if let Some(sc) = &scene {
        let v = centurion_helpers::calctx::scene_file(sc).and_then(|p| std::fs::read_to_string(&p).map_err(|e| e.to_string()))
            .and_then(|t| serde_json::from_str::<Value>(&t).map_err(|e| e.to_string()));
        match v {
            Ok(v) => {
                td.scene = true;
                let errs = centurion_helpers::calctx::apply_scene_power(&v);
                for e in &errs { eprintln!("scene \"{sc}\": {e}"); }
                eprintln!("measuring in scene \"{sc}\"'s power context (its Optimizations preset is not applied: those knobs are what is measured)");
                std::thread::sleep(Duration::from_secs(3));
            }
            Err(e) => { drop(td); die(&format!("--scene: {e}")); }
        }
    }
    let src = bench::power_source();
    if src != "battery" && !flag("--no-fan-lock") { td.fan = fan_lock(); }
    let bat_period = if src == "battery" {
        eprint!("battery: measuring how often the discharge reading updates ... ");
        let p = bench::battery_cadence(Duration::from_secs(14));
        eprintln!("{}", p.map_or("no change seen in 14 s: assuming 5 s".to_owned(), |p| format!("every {p:.2} s")));
        Some(p.unwrap_or(5.0))
    } else { None };
    let cdesc = centurion_helpers::calctx::capture(scene.as_deref());
    eprintln!("context: {}", centurion_helpers::calctx::describe(&cdesc));
    let cx = cal.add_context(cdesc);
    let heat_opt = if flag("--heat") { Some(true) } else if flag("--no-heat") { Some(false) } else { None };
    let ctx = Ctx {
        exe: std::env::current_exe().unwrap_or_else(|_| die("cannot find own executable")),
        dir: dir.clone(),
        disk: disk.clone().ok(),
        io_size: if thorough { 512 } else { 256 } << 20,
        scale,
        battery: src == "battery",
        bat_period, heat_secs: 15.0 * scale, cx, heat: heat_opt,
    };
    let rate = centurion_helpers::iorate::gather().bps;
    // Precise timers for the probes: with the default 50 µs timer slack the kernel may deliver every
    // sleep and deadline of the wake-up, frame and game loops that much late, which buried what the
    // idle governor, C-state and timer knobs do to a wake-up under a constant of the same size.
    // Threads and children started from here on inherit it.
    unsafe { libc::prctl(libc::PR_SET_TIMERSLACK, 1 as libc::c_ulong); }
    // From here on the reclaim / stall / pressure counters are this calibration's doing, not the
    // machine's use: the difference is recorded when the session ends, so autotune leaves it out.
    let ev0 = centurion_helpers::autotune::Evidence::live();
    // Fixed work of the game loop and the short jobs, sized once, before any knob is touched.
    eprintln!("work unit: {} k iterations/ms at full clock (game loop: 4.8 ms main thread + 3 x 2.4 ms jobs per 8 ms frame)", bench::unit() / 1000);
    eprintln!("cache unit: {} k dependent loads/ms over a {} MiB shared set, half the largest L3 (cache-bound loop: 2.4 ms main thread + 3 x 1.2 ms jobs where the set is cached)",
              bench::cache_unit() / 1000, bench::cache_set_bytes() >> 20);
    if cal.migrated { eprintln!("note: the stored idle/load measurements came from an older benchmark set that could not see most CPU and scheduler knobs; those phases start over (storage rows and unsafe values are kept)"); }
    if src != "battery" { eprintln!("note: on AC — power is RAPL (CPU package) only; device power states (the DEV phase) and whole-machine power need a session on battery"); }
    match &ctx.disk {
        Some(d) => eprintln!("storage suite: {} on {d} ({} MiB written per IO run, unlinked temp files)", ctx.dir, ctx.io_size * 3 / 2 >> 20),
        None => eprintln!("storage suite off: {}", disk.as_ref().err().map_or("", String::as_str)),
    }
    cal.on_battery = src == "battery";
    let kernel = std::fs::read_to_string("/proc/sys/kernel/osrelease").unwrap_or_default().trim().to_owned();
    if let Some(name) = opt("--verify") {
        let r = verify_main(&mut cal, &ctx, &name, opt("--rounds").and_then(|s| s.parse().ok()).unwrap_or(4), flag("--io"), opt("--goal"), rate, ram, &kernel);
        if let Err(e) = r { eprintln!("centurion-calibrate --verify: {e}"); }
        centurion_helpers::autotune::evidence_taint(&ev0);
        return;
    }
    if flag("--gpu-coupling") {
        if let Err(e) = coupling_main(&mut cal, opt("--secs").and_then(|s| s.parse().ok()).unwrap_or(120)) { eprintln!("centurion-calibrate --gpu-coupling: {e}"); }
        return;
    }

    if !flag("--oat") {
        let seed: u64 = opt("--seed").and_then(|s| s.parse().ok()).unwrap_or(0x1B_5EED);
        // --sessions N: N sessions back to back (walk away; progressive ones build on each other).
        let n = opt("--sessions").and_then(|s| s.parse::<u64>().ok()).unwrap_or(1).clamp(1, 48);
        for i in 0..n {
            if STOP.load(Ordering::SeqCst) { break; }
            if n > 1 { eprintln!("\n══ session {} of {n} ══", i + 1); }
            design_main(&mut cal, &items, &ctx, rate, ram, &kernel, budget, &dp, progressive, seed.wrapping_add(i * 0x9E37), !flag("--no-confirm"));
        }
        centurion_helpers::autotune::evidence_taint(&ev0);
        return;
    }
    // Budget: least-measured first until the time is used.
    let mut chosen = Vec::new();
    let mut spent = 0.0;
    for i in items {
        // The legacy flow keeps idle/load records only; storage is calibrated by the design.
        if matches!(i.phase, Phase::Io | Phase::Dev) { continue; }
        if spent > 0.0 && spent + i.secs > budget { continue; }
        spent += i.secs;
        chosen.push(i);
    }
    let runs: usize = chosen.iter().map(|i| runs_for(i.values.len(), rounds)).sum();
    eprintln!("{} measurement(s), {runs} runs, ~{:.0} of ~{:.0} min for the full signature · Ctrl-C restores and stops",
              chosen.len(), spent / 60.0, total_secs / 60.0);

    let mut j = Journal { entries: Vec::new() };
    let mut done = 0usize;
    let mut ballast: Option<Ballast> = None;
    let mut restarts = 0;
    let t_start = Instant::now();
    'items: for it in chosen {
        if it.phase == Phase::Load && ballast.is_none() {
            eprintln!("\nphase 2: building memory pressure ...");
            match Ballast::start(&ctx.exe) {
                Ok(b) => { eprintln!("ballast holds {} MiB; {} MiB left available", b.held_mib, bench::meminfo_kb("MemAvailable:").unwrap_or(0) / 1024); ballast = Some(b); }
                Err(e) => { eprintln!("load phase skipped: {e}"); break; }
            }
        }
        let mut samples: BTreeMap<String, Vec<Sample>> = BTreeMap::new();
        let mut unsafe_vals: Vec<String> = Vec::new();
        if it.phase == Phase::Load { let _ = std::fs::write("/proc/sys/vm/compact_memory", "1"); }
        {
            for v in &order(&it.reference, &it.values, rounds) {
                if STOP.load(Ordering::SeqCst) { break 'items; }
                if let Some(b) = ballast.as_mut() {
                    if !b.alive() {
                        restarts += 1;
                        let _ = j.restore();
                        if restarts > 2 { eprintln!("\nballast stopped by the brakes repeatedly: load phase ends here"); break 'items; }
                        match Ballast::start(&ctx.exe) { Ok(nb) => *b = nb, Err(e) => { eprintln!("\n{e}"); break 'items; } }
                    }
                }
                done += 1;
                eprint!("\r[{done}/{runs} {:>3.0} min] {:<4} {:<30} = {v:<20}", t_start.elapsed().as_secs_f64() / 60.0,
                        if it.phase == Phase::Idle { "idle" } else { "load" }, it.key);
                let _ = std::io::stderr().flush();
                if let Err(e) = apply(&mut j, &it.key, v, rate, ram) {
                    eprintln!("\n{}: {v}: {e} — value skipped", it.key);
                    let _ = j.restore();
                    continue;
                }
                let r = match it.phase { Phase::Idle => run_idle(it.benches, &ctx).map(|s| (s, false)), Phase::Load => run_load(it.benches, &ctx),
                                         Phase::Io => run_io(&ctx).map(|s| (s, false)), Phase::Dev => run_dev(&ctx).map(|s| (s, false)) };
                match r {
                    Ok((s, oom)) => { if oom && *v != it.reference { unsafe_vals.push(v.clone()); } samples.entry(v.clone()).or_default().push(s); }
                    Err(e) => eprintln!("\n{}: {v}: {e}", it.key),
                }
            }
        }
        for e in j.restore() { eprintln!("\nrestore: {e}"); }
        let refs = samples.remove(&it.reference).unwrap_or_default();
        let t = now();
        for v in &unsafe_vals { cal.add(&it.key, &it.reference, v, it.phase, None, true, t, &kernel); }
        if refs.len() < 2 { continue; }
        for (v, s) in &samples {
            let m = calib::fold(&calib::metric_effects(&refs, s));
            cal.add(&it.key, &it.reference, v, it.phase, Some(m), false, t, &kernel);
        }
        // Saved after every knob: an interrupted run keeps what it measured.
        let _ = centurion_helpers::secure_dir(centurion_helpers::defaults::DIR);
        let _ = centurion_helpers::write_root_file(calib::FILE, &serde_json::to_vec(&cal.to_json()).unwrap());
    }
    for e in j.restore() { eprintln!("\nrestore: {e}"); }
    if let Some(b) = ballast { b.stop(); }
    centurion_helpers::autotune::evidence_taint(&ev0);
    eprintln!();
    let _ = centurion_helpers::secure_dir(centurion_helpers::defaults::DIR);
    if let Err(e) = centurion_helpers::write_root_file(calib::FILE, &serde_json::to_vec(&cal.to_json()).unwrap()) { die(&format!("save: {e}")); }
    if STOP.load(Ordering::SeqCst) { println!("interrupted: measured knobs are saved, everything is restored"); }
    let (left, _) = plan(rounds, scale, ram, &None, "both", &cal, &disk, battery_now);
    let uncovered = left.iter().filter(|i| i.phase != Phase::Io && cal.coverage(&i.key, i.phase) == 0).count();
    println!("signature: {} key(s) measured; {uncovered} measurement(s) never run yet. `centurion-calibrate --show` for details; run again to extend/refine it.",
             cal.keys.len());
    println!("autotune uses it on its next run (GUI Autotune or centurion-autotune <goal>).");
}

// ── session conditions ───────────────────────────────────────────────────────

/// What a session changed outside the knobs, put back when main() ends (every return path; a
/// crash leaves it to `--restore` and the next boot).
#[derive(Default)]
struct Teardown { hold: bool, scene: bool, fan: Option<bool>, exposure: bool }

impl Drop for Teardown {
    fn drop(&mut self) {
        // Reverse order of setup: the fan lock (taken on top of the scene) first, then the scene's
        // originals — the other way round the lock's journal (the scene's fan state) won.
        if self.fan.is_some() { fan_unlock(); }
        if self.scene { for e in centurion_helpers::calctx::restore_scene_power() { eprintln!("scene power context: {e}"); } }
        centurion_helpers::stability::inflight_clear();
        if self.exposure { centurion_helpers::stability::record_calibration(false); }
        if self.hold { let _ = std::fs::remove_file(centurion_helpers::CALIBRATION_HOLD); }
    }
}

/// Takes the calibration hold (see centurion_helpers::CALIBRATION_HOLD). Returns false when the boot
/// calibration that started this process already holds it.
fn take_hold(why: &str) -> bool {
    if let Some(p) = centurion_helpers::calibration_hold() {
        if std::env::var("CENTURION_CALIBRATE_OWNER").ok().and_then(|s| s.parse::<i64>().ok()) == Some(p) { return false; }
        if p != std::process::id() as i64 { die(&format!("another calibration (pid {p}) is running")); }
    }
    let me = std::process::id() as i32;
    let body = json!({"pid": me, "start": centurion_helpers::proc_start_time(me), "why": why, "t": now()});
    if let Err(e) = centurion_helpers::secure_dir("/run/centurion").and_then(|_| centurion_helpers::write_root_file(centurion_helpers::CALIBRATION_HOLD, body.to_string().as_bytes())) {
        eprintln!("calibration hold not written ({e}): a scene switched meanwhile would be measured as a knob");
        return false;
    }
    true
}

const FAN_JOURNAL: &str = "/run/centurion/calibrate-fan.json";

/// On AC the EC fan is held at full speed for the session: its curve has hysteresis and follows the
/// die temperature, so two identical runs met different cooling depending on what ran before.
/// Returns the original state when it was changed.
fn fan_lock() -> Option<bool> {
    let orig = centurion_helpers::legion_wmi::fan_fullspeed_get().ok()?;
    if orig { return None; }
    let _ = centurion_helpers::write_root_file(FAN_JOURNAL, json!({"orig": orig}).to_string().as_bytes());
    match centurion_helpers::legion_wmi::fan_fullspeed_set(true) {
        Ok(_) => { eprintln!("fans held at full speed for the session (constant cooling; --no-fan-lock to leave them on auto)"); std::thread::sleep(Duration::from_secs(4)); Some(orig) }
        Err(e) => { let _ = std::fs::remove_file(FAN_JOURNAL); eprintln!("fan lock not available ({e}): cooling follows the fan curve"); None }
    }
}

fn fan_unlock() {
    let Some(v) = centurion_helpers::read_root_file(FAN_JOURNAL, 4096).and_then(|s| serde_json::from_str::<Value>(&s).ok()) else { return };
    match centurion_helpers::legion_wmi::fan_fullspeed_set(v["orig"].as_bool().unwrap_or(false)) {
        Ok(_) => { let _ = std::fs::remove_file(FAN_JOURNAL); }
        Err(e) => eprintln!("fan mode not restored ({e}); sudo centurion-calibrate --restore tries again"),
    }
}

// ── calibration at the next boot ─────────────────────────────────────────────

use centurion_helpers::calctx::{BOOT_FLAG, BOOT_STATUS, BOOT_TAKEN};
const BOOT_LOG: &str = "/var/log/centurion/calibrate-boot.log";
/// The boot calibration gives up after this long without finishing (the machine stays in use).
const BOOT_DEADLINE_S: u64 = 4 * 3600;
/// Quiet time (no keyboard/mouse/touchpad event) before a session starts.
const BOOT_IDLE_S: u64 = 300;

fn boot_status() -> String {
    if let Some(s) = std::fs::read_to_string(BOOT_STATUS).ok().filter(|s| !s.trim().is_empty()) { return s.trim().to_owned(); }
    match centurion_helpers::read_root_file(BOOT_FLAG, 4096).and_then(|s| serde_json::from_str::<Value>(&s).ok()) {
        Some(v) => json!({"state": "scheduled", "sessions": v["sessions"], "budget": v["budget"], "scene": v["scene"]}).to_string(),
        None => json!({"state": "none"}).to_string(),
    }
}

fn set_status(v: Value) {
    let mut v = v;
    v["t"] = json!(now());
    if centurion_helpers::secure_dir("/run/centurion").is_ok() { let _ = centurion_helpers::write_root_file(BOOT_STATUS, v.to_string().as_bytes()); }
}

/// Unix time of the last keyboard / mouse / touchpad event, watched on every /dev/input/event*
/// (plain reads: nothing is grabbed, the desktop still gets every event).
static LAST_INPUT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

fn watch_input() {
    LAST_INPUT.store(now(), Ordering::SeqCst);
    std::thread::spawn(|| loop {
        let mut files: Vec<std::fs::File> = std::fs::read_dir("/dev/input").into_iter().flatten().flatten()
            .filter(|e| e.file_name().to_string_lossy().starts_with("event"))
            .filter_map(|e| std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NONBLOCK | libc::O_CLOEXEC).open(e.path()).ok()).collect();
        let rescan = Instant::now();
        while rescan.elapsed() < Duration::from_secs(30) {
            let mut fds: Vec<libc::pollfd> = files.iter().map(|f| libc::pollfd { fd: std::os::unix::io::AsRawFd::as_raw_fd(f), events: libc::POLLIN, revents: 0 }).collect();
            if fds.is_empty() { std::thread::sleep(Duration::from_secs(5)); break; }
            let n = unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, 1000) };
            if n <= 0 { continue; }
            let mut buf = [0u8; 4096];
            let mut any = false;
            for (f, p) in files.iter_mut().zip(&fds) {
                if p.revents & libc::POLLIN != 0 { while matches!(std::io::Read::read(f, &mut buf), Ok(n) if n > 0) {} any = true; }
            }
            if any { LAST_INPUT.store(now(), Ordering::SeqCst); }
        }
    });
}

/// Why a boot session cannot start now (None = go).
fn boot_blocker() -> Option<String> {
    if centurion_helpers::power::on_ac() != Some(true) { return Some("waiting for AC power".into()); }
    let idle = now().saturating_sub(LAST_INPUT.load(Ordering::SeqCst));
    if idle < BOOT_IDLE_S { return Some(format!("waiting for {} min without keyboard/mouse use ({} s so far)", BOOT_IDLE_S / 60, idle)); }
    let psi = centurion_helpers::autotune::psi_avg10(&std::fs::read_to_string("/proc/pressure/cpu").unwrap_or_default(), "some").unwrap_or(0.0);
    if psi > 10.0 { return Some(format!("waiting for a quiet machine (CPU pressure {psi:.0} %)")); }
    let st: Value = std::fs::read_to_string(TUNE_STATE).ok().and_then(|s| serde_json::from_str(&s).ok()).unwrap_or(Value::Null);
    if centurion_helpers::live_game_sessions(&st) > 0 { return Some("waiting: a game is running".into()); }
    if st["baseline"].as_array().map_or(false, |a| !a.is_empty()) { return Some("waiting: an Optimizations preset is active (Restore originals)".into()); }
    None
}

/// `centurion-calibrate --boot-run` (the centurion-calibrate-boot service): when a boot calibration is
/// scheduled, holds scenes from boot on (the calibration hold), waits for AC and an idle machine,
/// runs the sessions as children (a key press or mouse move stops the running one cleanly; it
/// resumes after the next quiet stretch), then applies the boot preset and releases the hold.
fn boot_run() -> i32 {
    let Some(v) = centurion_helpers::read_root_file(BOOT_FLAG, 4096).and_then(|s| serde_json::from_str::<Value>(&s).ok()) else { return 0 };
    // Consumed at once: a crash during it must not start it again at every boot. The taken copy
    // carries this boot's id, so it holds the boot preset during this boot only.
    let mut taken = v.clone();
    taken["boot_id"] = json!(centurion_helpers::bootguard::boot_id());
    if centurion_helpers::write_root_file(BOOT_TAKEN, taken.to_string().as_bytes()).is_err() || std::fs::remove_file(BOOT_FLAG).is_err() { return 1; }
    // Service stop / shutdown (SIGINT from both init systems, SIGTERM): end the wait or the running
    // session cleanly instead of being killed with the hold and the taken flag left behind.
    unsafe {
        libc::signal(libc::SIGINT, on_signal as *const () as libc::sighandler_t);
        libc::signal(libc::SIGTERM, on_signal as *const () as libc::sighandler_t);
    }
    let sessions = v["sessions"].as_u64().unwrap_or(2).clamp(1, 12);
    let budget = v["budget"].as_f64().unwrap_or(15.0).clamp(5.0, 60.0);
    let scene = v["scene"].as_str().map(str::to_owned);
    let hold = take_hold("boot calibration");
    let me = std::process::id().to_string();
    watch_input();
    let t0 = now();
    let (mut done, mut stopped) = (0u64, 0u32);
    let _ = std::fs::create_dir_all("/var/log/centurion");
    let log = |m: &str| {
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(BOOT_LOG) { let _ = writeln!(f, "[{}] {m}", now()); }
    };
    log(&format!("boot calibration: {sessions} session(s) of {budget:.0} min{}", scene.as_deref().map(|s| format!(" in scene \"{s}\"")).unwrap_or_default()));
    while done < sessions && now() - t0 < BOOT_DEADLINE_S {
        if STOP.load(Ordering::SeqCst) { break; }
        if let Some(why) = boot_blocker() {
            set_status(json!({"state": "waiting", "why": why, "done": done, "sessions": sessions, "stopped": stopped}));
            std::thread::sleep(Duration::from_secs(10));
            continue;
        }
        set_status(json!({"state": "running", "session": done + 1, "sessions": sessions, "stopped": stopped}));
        log(&format!("session {} of {sessions} starts", done + 1));
        let out = std::fs::OpenOptions::new().create(true).append(true).open(BOOT_LOG);
        let mut cmd = std::process::Command::new(std::env::current_exe().unwrap_or_else(|_| PathBuf::from("/usr/bin/centurion-calibrate")));
        cmd.args(["--sessions", "1", "--budget", &format!("{budget}")]).env("CENTURION_CALIBRATE_OWNER", &me);
        if let Some(sc) = &scene { cmd.args(["--scene", sc]); if let Some(u) = v["user"].as_str() { cmd.env("SUDO_USER", u); } }
        if let Ok(f) = out { if let Ok(f2) = f.try_clone() { cmd.stdout(f).stderr(f2); } }
        let started = now();
        let Ok(mut child) = cmd.spawn() else { log("could not start a session"); break };
        let mut interrupted = false;
        loop {
            if let Ok(Some(_)) = child.try_wait() { break; }
            if !interrupted && LAST_INPUT.load(Ordering::SeqCst) > started {
                // Someone is using the machine: stop cleanly (the child restores and keeps what it measured).
                unsafe { libc::kill(child.id() as i32, libc::SIGINT); }
                interrupted = true;
                log("input seen: session stopped (what it measured is kept)");
            }
            if STOP.load(Ordering::SeqCst) && !interrupted { unsafe { libc::kill(child.id() as i32, libc::SIGINT); } interrupted = true; }
            std::thread::sleep(Duration::from_millis(500));
        }
        let ok = child.wait().map_or(false, |s| s.success());
        if STOP.load(Ordering::SeqCst) { break; }
        if interrupted { stopped += 1; if stopped >= 6 { log("stopped six times: giving up for this boot"); break; } continue; }
        if !ok { log("session failed (see above): giving up"); break; }
        done += 1;
    }
    let _ = std::fs::remove_file(BOOT_TAKEN);
    // Back to the normal state: the boot preset (held this boot whether or not a session ran — the
    // deadline can pass while waiting, a session can fail), then scenes (the GUI re-applies its
    // scene when the hold goes away). Not while the service is being stopped (shutdown).
    // Same gate as the centurion-tune service: presets paused by centurion-boot-guard stay paused.
    let guard = centurion_helpers::bootguard::should_skip();
    if let Some(why) = &guard { log(&format!("boot preset not applied: boot presets are paused ({why})")); }
    if !STOP.load(Ordering::SeqCst) && guard.is_none() {
        match centurion_helpers::calctx::run_helper("tune-helper", &json!({"op": "boot"})) {
            Ok(v) => log(&format!("boot preset: {}", v["note"].as_str().or(v["error"].as_str()).unwrap_or(if v["ok"] == true { "applied" } else { "failed" }))),
            Err(e) => log(&format!("boot preset not applied: {e}")),
        }
    }
    let state = if done >= sessions { "done" } else if STOP.load(Ordering::SeqCst) { "stopped" } else { "incomplete" };
    log(&format!("boot calibration {state}: {done} of {sessions} session(s), {stopped} interrupted"));
    set_status(json!({"state": state, "done": done, "sessions": sessions, "stopped": stopped}));
    if hold { let _ = std::fs::remove_file(centurion_helpers::CALIBRATION_HOLD); }
    0
}

// ── field evidence ───────────────────────────────────────────────────────────

/// `--field LOG...`: frame-time logs of game sessions recorded by centurion-gamemode become rows of the
/// load model (see centurion_helpers::field).
fn field_import(cal: &mut Calibration, files: &[String], game: Option<String>) -> Result<String, String> {
    use centurion_helpers::field;
    if files.is_empty() { return Err("--field needs one or more frame-time logs (FLM or MangoHud CSV)".into()); }
    if centurion_helpers::calibration_hold().is_some() { return Err("a calibration is running: import afterwards".into()); }
    let user = std::env::var("SUDO_USER").ok().filter(|u| !u.is_empty()).ok_or("run it with sudo from the account that plays the games")?;
    let home = std::fs::read_to_string("/etc/passwd").ok().and_then(|p| p.lines().find_map(|l| {
        let f: Vec<&str> = l.split(':').collect();
        (f.len() >= 6 && f[0] == user).then(|| f[5].to_owned())
    })).ok_or("no home directory")?;
    let sessions = field::sessions(&std::fs::read_to_string(field::session_log(std::path::Path::new(&home))).unwrap_or_default());
    if sessions.is_empty() { return Err("no recorded game sessions yet (centurion-gamemode records them while games run)".into()); }
    let defaults = centurion_helpers::defaults::load();
    let refs = cal.refs.clone();
    let boot = |k: &str| defaults.as_ref().and_then(|d| d.values.get(k).cloned()).or_else(|| refs.get(k).cloned());
    let (mut recs, mut msgs) = (Vec::new(), Vec::new());
    for f in files {
        let meta = std::fs::metadata(f).map_err(|e| format!("{f}: {e}"))?;
        if meta.len() > 256 << 20 { msgs.push(format!("{f}: larger than 256 MiB, skipped")); continue; }
        let mtime = meta.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs());
        let Some(ft) = field::parse_log(&std::fs::read_to_string(f).map_err(|e| format!("{f}: {e}"))?) else { msgs.push(format!("{f}: no frame times found")); continue };
        let Some(fr) = field::frames(&ft) else { msgs.push(format!("{f}: too short ({} frames)", ft.len())); continue };
        let Some(s) = field::match_session(&sessions, mtime) else { msgs.push(format!("{f}: no recorded game session around its time")); continue };
        let g = game.clone().or_else(|| s["game"].as_str().map(str::to_owned)).unwrap_or_else(|| "unknown".into());
        let cfg = field::session_cfg(s, &boot);
        msgs.push(format!("{f}: {g} {} - median {:.2} ms, tail {:.2} ms, pacing {:.3} ms, {} frames, {} changed setting(s)",
                          if cfg.is_empty() { "(A: boot defaults)" } else { "(B)" }, fr.med, fr.tail, fr.pacing, fr.n, cfg.len()));
        recs.push(json!({"game": g, "start": s["start"], "cfg": cfg.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
                         "med": fr.med, "tail": fr.tail, "pacing": fr.pacing, "n": fr.n}));
    }
    let n = cal.add_field_sessions(recs);
    save(cal);
    msgs.push(format!("field rows in the load model: {n} (a game counts once it has two A sessions and one B session; centurion-gamemode's A/B mode alternates them)"));
    Ok(msgs.join("\n"))
}

// ── whole-preset A/B check ───────────────────────────────────────────────────

/// Settings a verification never writes: CPUs offlined, driver switches, firmware limits, radios.
const NOT_IN_VERIFY: &[&str] = &["cpu.smt", "cpu.ccd_park", "cpu.pstate_status", "cpu.intel_pstate_status", "sched.ext", "irq.affinity", "wq.cpumask",
    "rf.wlan", "rf.bluetooth", "rf.wwan", "pm.mem_sleep", "cpu.rapl_pl1", "cpu.rapl_pl2", "cpu.tcc_offset", "net.wol"];

/// `--verify PRESET`: the whole preset against the boot state, ABBA-interleaved (drift cancels),
/// on the idle benchmarks (and the storage suite with --io). Reports the measured gain per
/// objective and for the goal, next to what the joint model predicted for the preset's
/// calibrated part; keeps the result in the signature, and the runs as design rows when every
/// change is a calibrated knob.
#[allow(clippy::too_many_arguments)]
fn verify_main(cal: &mut Calibration, ctx: &Ctx, name: &str, rounds: usize, with_io: bool, goal: Option<String>, rate: u64, ram: u64, kernel: &str) -> Result<(), String> {
    if name.is_empty() || name.len() > 64 || !name.chars().all(|c| c.is_ascii_alphanumeric() || " _-.".contains(c)) || name.starts_with('.') { return Err(format!("invalid preset name '{name}'")); }
    let text = centurion_helpers::read_root_file(&format!("/etc/centurion/presets/{name}.json"), 256 * 1024)
        .ok_or_else(|| format!("preset \"{name}\" is not in the approved store (/etc/centurion/presets): save it in the GUI first"))?;
    let v: Value = serde_json::from_str(&text).map_err(|e| e.to_string())?;
    let mut cfg: Cfg = Vec::new();
    let mut skipped = Vec::new();
    for (k, x) in v["values"].as_object().into_iter().flatten() {
        let val = match x { Value::String(s) => s.clone(), o => o.to_string() };
        let Some(t) = tune::find(k) else { continue };
        if NOT_IN_VERIFY.contains(&k.as_str()) || (!t.debugfs && tune::files(t).is_empty()) { skipped.push(k.clone()); continue; }
        if tune::current(t).as_deref() == Some(val.as_str()) { continue; }
        cfg.push((k.clone(), val));
    }
    if cfg.is_empty() { return Err(format!("\"{name}\" changes nothing here (all its values are live already)")); }
    let goal = goal.as_deref().and_then(Goal::parse).or_else(|| {
        let n = name.to_lowercase();
        [("gam", Goal::Gaming), ("throughput", Goal::Throughput), ("compile", Goal::Throughput), ("power", Goal::PowerSave), ("battery", Goal::PowerSave)]
            .iter().find(|(p, _)| n.contains(p)).map(|x| x.1)
    }).unwrap_or(Goal::Desktop);
    let rounds = rounds.clamp(2, 12);
    eprintln!("verify \"{name}\" ({} change(s){}) against the boot state for {}: {rounds} ABBA round(s)",
              cfg.len(), if skipped.is_empty() { String::new() } else { format!("; not written: {}", skipped.join(", ")) }, goal.label());
    let mut rn = Runner { j: Journal { entries: Vec::new() }, c: ctx, rate, ram, ballast: None, restarts: 0, runs: 0, t0: Instant::now(), busy: 0, base: Vec::new(),
                          cool: [None; 4], last_hot: None, disp0: None, disp_off: 0, gate_s: 0.0 };
    let bs = Benches { cpu_mem: true, cpu: true, idle: true, io: false };
    let none: Cfg = Vec::new();
    let mut runs: BTreeMap<usize, Vec<Run>> = BTreeMap::new();
    let phases: Vec<Phase> = if with_io && ctx.disk.is_some() { vec![Phase::Idle, Phase::Io] } else { vec![Phase::Idle] };
    'r: for r in 0..rounds {
        let order: [&Cfg; 4] = if r % 2 == 0 { [&none, &cfg, &cfg, &none] } else { [&cfg, &none, &none, &cfg] };
        for c in order {
            for ph in &phases {
                if STOP.load(Ordering::SeqCst) { break 'r; }
                if let Some(Done { s, oom: false, w, t, temp }) = rn.run(*ph, bs, c, "(verify)")? {
                    let e = runs.entry(ph.idx()).or_default();
                    let order = e.len();
                    e.push(Run { cfg: c.clone(), s, order, t, w, temp });
                }
            }
        }
    }
    for e in rn.j.restore() { eprintln!("\nrestore: {e}"); }
    centurion_helpers::stability::inflight_clear();
    eprintln!();
    let meta = Meta { src: if ctx.battery { calib::PowerSrc::Battery } else { calib::PowerSrc::Rapl }, cx: ctx.cx.clone() };
    let sess = now() + 7;
    let mut objectives = serde_json::Map::new();
    let (mut u_rows, mut stored) = (Vec::<f64>::new(), false);
    let names = ["latency", "throughput", "power", "memory"];
    for (pi, rs) in &runs {
        let ph = Phase::from_idx(*pi as u64);
        let rows = compute_rows(rs, ph, sess, kernel, rs.len(), &meta);
        let b: Vec<&Row> = rows.iter().filter(|r| !r.cfg.is_empty()).collect();
        let wts = goal_wts(goal, ph);
        for o in 0..4 {
            let v: Vec<f64> = b.iter().map(|r| r.y[o]).filter(|x| x.is_finite()).collect();
            if v.len() < 2 { continue; }
            let m = v.iter().sum::<f64>() / v.len() as f64;
            let sd = (v.iter().map(|x| (x - m).powi(2)).sum::<f64>() / (v.len() - 1) as f64).sqrt();
            objectives.insert(format!("{}{}", if ph == Phase::Io { "io " } else { "" }, names[o]), json!([round3(m), round3(sd / (v.len() as f64).sqrt())]));
        }
        for (i, r) in b.iter().enumerate() {
            let u: f64 = (0..4).filter(|o| r.y[*o].is_finite()).map(|o| wts[o] * r.y[o]).sum();
            if u_rows.len() <= i { u_rows.push(u); } else { u_rows[i] += u; }
        }
        // The runs join the design log when every change is a knob the model calibrates from the same reference.
        if ph == Phase::Idle && cfg.iter().all(|(k, _)| cal.refs.contains_key(k) && calib::phases_for(k, tune::find(k).map_or("", |t| t.group)).iter().any(|p| p.0 == Phase::Idle)) {
            cal.put_session(ph, sess, rows.clone());
            stored = true;
        }
    }
    let n = u_rows.len().max(1) as f64;
    let um = u_rows.iter().sum::<f64>() / n;
    let use_ = if u_rows.len() >= 2 { (u_rows.iter().map(|x| (x - um).powi(2)).sum::<f64>() / (n - 1.0)).sqrt() / n.sqrt() } else { f64::NAN };
    // What the model predicts for the calibrated part of the preset (idle phase, the session's context).
    cal.set_target(vec![(1.0, cal.ctx_cfg(Phase::Idle, &ctx.cx))]);
    let pred = cal.phase_model(Phase::Idle, goal_wts(goal, Phase::Idle)).map(|m| {
        let keys: Vec<(String, String)> = cfg.iter().filter(|(k, _)| m.space.has(k)).cloned().collect();
        let mut j = Joint::new(Some(m), None, None, 0.5);
        j.ctxs = cal.target.borrow().clone();
        let (mu, var) = j.eval(&keys);
        (keys.len(), mu, var.sqrt())
    });
    println!("\nverify \"{name}\" for {} ({} run(s) per side):", goal.label(), u_rows.len());
    for (k, x) in &objectives { println!("  {k:<16} {:+6.1} % ± {:.1}", x[0].as_f64().unwrap_or(0.0) * 100.0, x[1].as_f64().unwrap_or(0.0) * 100.0); }
    println!("  weighted gain    {um:+.3} ± {:.3}  -> {}", use_,
             if use_.is_finite() && um - 2.0 * use_ > 0.0 { "better than the boot state" } else if use_.is_finite() && um + 2.0 * use_ < 0.0 { "WORSE than the boot state" } else { "no credible difference" });
    if let Some((k, mu, sd)) = pred {
        let agree = use_.is_finite() && (um - mu).abs() <= 2.0 * (use_ * use_ + sd * sd).sqrt();
        println!("  model predicted  {mu:+.3} ± {sd:.3} for its {k} calibrated change(s) of {} -> {}", cfg.len(),
                 if agree { "as predicted" } else { "SURPRISE: the uncalibrated part or interactions the model has not seen matter" });
    }
    if stored { println!("  the runs joined the design log (every change is a calibrated knob)"); }
    cal.verify.push(json!({"t": now(), "preset": name, "goal": goal.key(), "changes": cfg.len(), "rounds": u_rows.len(), "objectives": objectives,
                           "gain": [round3(um), round3(use_)], "predicted": pred.map(|p| json!([p.0, round3(p.1), round3(p.2)])), "cx": ctx.cx}));
    let drop = cal.verify.len().saturating_sub(20);
    cal.verify.drain(..drop);
    save(cal);
    Ok(())
}

fn round3(x: f64) -> f64 { if x.is_finite() { (x * 1000.0).round() / 1000.0 } else { f64::NAN } }

// ── CPU-GPU power coupling ───────────────────────────────────────────────────

fn nvidia_sample() -> Option<(f64, f64, f64)> {
    let exe = ["/usr/bin/nvidia-smi", "/bin/nvidia-smi", "/opt/bin/nvidia-smi"].iter().find(|p| centurion_helpers::trusted_path(std::path::Path::new(p)))?;
    let out = std::process::Command::new(exe).args(["--query-gpu=power.draw,clocks.gr,utilization.gpu", "--format=csv,noheader,nounits"])
        .env_clear().env("PATH", "/usr/bin:/bin").output().ok()?;
    let t = String::from_utf8_lossy(&out.stdout);
    let v: Vec<f64> = t.lines().next()?.split(',').filter_map(|x| x.trim().parse().ok()).collect();
    (v.len() == 3).then(|| (v[0], v[1], v[2]))
}

/// `--gpu-coupling`: while a GPU-bound game or benchmark runs, the CPU's power is switched between
/// two levels (EPP performance / power, else boost on / off) in 15 s blocks, and what the GPU's
/// power and clock do in response is read: how many of the watts the CPU saves the GPU really gets
/// (Dynamic Boost), and how much clock a GPU watt buys. Autotune's gaming goal uses it to credit CPU
/// power savings as GPU throughput.
fn coupling_main(cal: &mut Calibration, secs: u64) -> Result<(), String> {
    if !centurion_helpers::dgpu_usable() { return Err("the NVIDIA dGPU is off or not usable".into()); }
    let (_, _, util) = nvidia_sample().ok_or("nvidia-smi gives no reading")?;
    if util < 70.0 { return Err(format!("GPU utilisation {util:.0} %: start a GPU-bound game or benchmark first (windowed is fine), then run this")); }
    let (key, hi, lo) = if tune::find("cpu.epp").map_or(false, |t| !tune::files(t).is_empty()) { ("cpu.epp", "performance", "power") } else { ("cpu.boost", "1", "0") };
    let mut j = Journal { entries: Vec::new() };
    let blocks = ((secs / 15).max(4) / 2 * 2) as usize;
    eprintln!("coupling: {blocks} blocks of 15 s, {key} {hi} / {lo} alternating; keep the game running");
    let mut acc: [Vec<(f64, f64, f64, f64)>; 2] = [Vec::new(), Vec::new()];
    for b in 0..blocks {
        if STOP.load(Ordering::SeqCst) { break; }
        let high = b % 2 == 0;
        if let Err(e) = j.set(key, if high { hi } else { lo }) { let _ = j.restore(); return Err(format!("{key}: {e}")); }
        std::thread::sleep(Duration::from_secs(3));
        for _ in 0..12 {
            let ((), cpu) = bench::with_pkg_power(|| std::thread::sleep(Duration::from_millis(800)));
            if let (Some(c), Some((g, clk, u))) = (cpu, nvidia_sample()) { acc[high as usize].push((c, g, clk, u)); }
        }
        eprint!("\r  block {}/{blocks}", b + 1);
    }
    for e in j.restore() { eprintln!("\nrestore: {e}"); }
    eprintln!();
    let mean = |v: &[(f64, f64, f64, f64)], f: fn(&(f64, f64, f64, f64)) -> f64| v.iter().map(f).sum::<f64>() / v.len().max(1) as f64;
    let (lo_s, hi_s) = (&acc[0], &acc[1]);
    if lo_s.len() < 10 || hi_s.len() < 10 { return Err("too few readings".into()); }
    let (c_hi, c_lo) = (mean(hi_s, |x| x.0), mean(lo_s, |x| x.0));
    let (g_hi, g_lo) = (mean(hi_s, |x| x.1), mean(lo_s, |x| x.1));
    let (k_hi, k_lo) = (mean(hi_s, |x| x.2), mean(lo_s, |x| x.2));
    let util_min = mean(hi_s, |x| x.3).min(mean(lo_s, |x| x.3));
    let dc = c_hi - c_lo;
    if dc < 3.0 { return Err(format!("the CPU power hardly moved ({dc:.1} W): the game does not load the CPU enough to measure this")); }
    let c = (-(g_hi - g_lo) / dc).clamp(0.0, 1.5);
    let eps = ((g_lo / g_hi).ln().abs() > 0.02).then(|| ((k_lo / k_hi).ln() / (g_lo / g_hi).ln()).clamp(0.0, 1.0));
    let reliable = util_min >= 85.0;
    println!("CPU {c_lo:.1} -> {c_hi:.1} W, GPU {g_hi:.1} -> {g_lo:.1} W, GPU clock {k_hi:.0} -> {k_lo:.0} MHz, GPU busy >= {util_min:.0} %");
    println!("coupling: the GPU gets {:.0} % of a watt the CPU saves; clock per GPU power (elasticity) {}{}", c * 100.0,
             eps.map_or("not measurable (GPU power hardly moved)".into(), |e| format!("{e:.2}")),
             if reliable { "" } else { " - the GPU was not fully busy in both states (CPU-bound moments): treat as a rough figure" });
    cal.coupling = Some(json!({"c": round3(c), "eps": eps.map(round3), "cpu_w": round3(c_hi), "gpu_w": round3(g_hi), "util_min": util_min.round(),
                               "lever": key, "reliable": reliable, "t": now()}));
    save(cal);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn meta() -> Meta { Meta { src: calib::PowerSrc::Rapl, cx: String::new() } }
    #[test]
    fn rows_carry_source_context_and_start_temperature() {
        let s = |w: f64| -> Sample { [(Metric::IdleW, w), (Metric::CpuSingle, 100.0)].into_iter().collect() };
        let hot = vec![("ctx.heat".to_owned(), "hot".to_owned())];
        let mut runs: Vec<Run> = (0..4).map(|i| Run { cfg: Vec::new(), s: s(10.0), order: i, t: i as u64, w: 1.0, temp: Some(50.0 + i as f64) }).collect();
        runs.push(Run { cfg: hot.clone(), s: s(12.0), order: 4, t: 9, w: 1.0, temp: Some(88.0) });
        runs.push(Run { cfg: hot.clone(), s: s(12.0), order: 5, t: 10, w: 1.0, temp: Some(92.0) });
        runs.push(Run { cfg: vec![("cpu.k".into(), "1".into())], s: s(10.0), order: 6, t: 11, w: 1.0, temp: None });
        let m = Meta { src: calib::PowerSrc::Battery, cx: "c1".into() };
        let rows = compute_rows(&runs, Phase::Idle, 3, "7.2", 7, &m);
        assert!(rows.iter().all(|r| r.src == calib::PowerSrc::Battery && r.cx == "c1"));
        // Cool runs against the cool median (51.5), hot ones against the hot median (90).
        assert!((rows[0].tc + 1.5).abs() < 1e-9 && (rows[4].tc + 2.0).abs() < 1e-9 && (rows[5].tc - 2.0).abs() < 1e-9 && rows[6].tc.is_nan());
        assert!(rows[4].y[2] < -0.1, "a hot run's power reads worse: {:?}", rows[4].y);
        assert_eq!(compute_rows(&runs, Phase::Dev, 3, "", 7, &meta())[0].src, calib::PowerSrc::Battery, "devices are always battery power");
        assert!(is_hot(&hot) && !is_hot(&runs[0].cfg));
        assert_eq!(cool_first(vec![hot.clone(), Vec::new(), hot.clone()]), vec![Vec::new(), hot.clone(), hot]);
        assert!(!Depth::pick(15.0, 20, None).heat() && Depth::pick(40.0, 20, None).heat());
        assert!(!Depth::progressive(Stage::Pairs, 20, [0; 3]).heat() && Depth::progressive(Stage::Crowd, 20, [0; 3]).heat());
        let lv = ref_levels(&runs, Phase::Idle, calib::PowerSrc::Battery);
        assert_eq!(lv, vec![("idle_w_battery".to_owned(), 10.0)]);
    }
    #[test]
    fn schedule() {
        let c: Vec<String> = ["a", "b", "c"].iter().map(|x| x.to_string()).collect();
        assert_eq!(order("r", &c, 1), vec!["r", "a", "b", "c", "r"]);
        assert_eq!(order("r", &c, 2), vec!["r", "a", "b", "c", "r", "b", "c", "a", "r"]);
        assert_eq!(runs_for(3, 1), 5);
        assert_eq!(runs_for(3, 2), 9);
        let cpu = Benches { cpu: true, idle: true, ..Default::default() };
        // A CPU knob with 3 candidates: 5 runs x 7.4 s (frame loop, game loop, cache-bound loop, contended window and short jobs included).
        assert!(est_secs(Phase::Idle, cpu, 1.0) * runs_for(3, 1) as f64 <= 37.5);
        assert!(est_secs(Phase::Load, Benches { cpu: true, ..Default::default() }, 1.0) <= 5.0);
        assert!(est_secs(Phase::Io, Benches { io: true, ..Default::default() }, 1.0) < 6.0);
        assert!(selects("@io", "blk.read_ahead_kb", "Storage") && selects("@io", "vm.dirty", "Memory") && !selects("@io", "vm.swappiness", "Memory"));
        assert_eq!(union_benches(Phase::Io, &[]), Benches { io: true, ..Default::default() });
    }

    fn run(cfg: Cfg, s: &[(Metric, f64)]) -> Run { Run { cfg, s: s.iter().copied().collect(), order: 0, t: 0, w: 1.0, temp: None } }

    #[test]
    fn noisy_metrics_weigh_less_inside_their_objective() {
        // Reference runs: wake tail scatters +-30 %, frame tail +-2 %.
        let refs: Vec<Run> = [1.0, 1.3, 0.75, 1.25, 0.8, 1.0].iter().zip([1.0, 1.02, 0.98, 1.01, 0.99, 1.0])
            .map(|(a, b)| run(vec![], &[(Metric::WakeP99Us, 100.0 * a), (Metric::FrameP99Us, 500.0 * b)])).collect();
        let r: Vec<&Run> = refs.iter().collect();
        let w = metric_weights(&r, &[Metric::WakeP99Us, Metric::FrameP99Us]);
        // Two metrics, one far noisier: the shrinkage settles at 3:1 (1.5 / 0.5 of the equal share).
        assert!(w[&Metric::FrameP99Us] > 1.45 && w[&Metric::WakeP99Us] < 0.55, "{w:?}");
        // Too few references: equal weights.
        let w2 = metric_weights(&r[..3], &[Metric::WakeP99Us, Metric::FrameP99Us]);
        assert_eq!(w2[&Metric::WakeP99Us], 1.0);
        // A run 10 % better on the steady metric only now shows clearly more than half of it.
        let mut runs = refs;
        runs.push(run(vec![("k".into(), "1".into())], &[(Metric::WakeP99Us, 100.0), (Metric::FrameP99Us, 450.0)]));
        let rows = compute_rows(&runs, Phase::Idle, 1, "", 10, &meta());
        let y = rows.last().unwrap().y[0];
        assert!(y > 0.07, "{y}");
    }
    #[test]
    fn an_effect_on_one_metric_is_not_diluted_by_the_others() {
        // Eight latency metrics, a knob that makes the game's frame tail 10 % better and touches nothing else.
        let lat = [Metric::WakeP99Us, Metric::FrameP99Us, Metric::FrameMedUs, Metric::PingPongP99Us, Metric::GameTailMs,
                   Metric::BusyFrameP99Us, Metric::FaultP99Us, Metric::FaultHugeP99Us];
        let base = |game: f64| -> Vec<(Metric, f64)> { lat.iter().map(|m| (*m, if *m == Metric::GameTailMs { game } else { 100.0 })).collect() };
        let mut runs: Vec<Run> = (0..3).map(|_| run(vec![], &base(10.0))).collect();
        runs.push(run(vec![("k".into(), "1".into())], &base(9.0)));
        let y = compute_rows(&runs, Phase::Idle, 1, "", 10, &meta()).last().unwrap().y[0];
        // The plain mean gave ln(1/0.9) / 8 = 1.3 %: under every margin. Summed over the metrics and halved: 5.3 %.
        assert!((y - (1.0f64 / 0.9).ln() / 2.0).abs() < 1e-9, "{y}");
        // The same knob measured in a session without the memory probes (6 metrics) lands on the same scale.
        let few = |game: f64| -> Vec<(Metric, f64)> { base(game).into_iter().filter(|(m, _)| !matches!(m, Metric::FaultP99Us | Metric::FaultHugeP99Us)).collect() };
        let mut runs: Vec<Run> = (0..3).map(|_| run(vec![], &few(10.0))).collect();
        runs.push(run(vec![("k".into(), "1".into())], &few(9.0)));
        let y2 = compute_rows(&runs, Phase::Idle, 1, "", 10, &meta()).last().unwrap().y[0];
        assert!((y - y2).abs() < 1e-9, "{y} vs {y2}");
    }

    #[test]
    fn depth_grows_with_budget_and_ladders_refine_doses() {
        let (l, d, m) = (Depth::pick(15.0, 30, None), Depth::pick(40.0, 30, None), Depth::pick(f64::INFINITY, 30, None));
        assert_eq!((l.name, d.name, m.name), ("lean", "deep", "max"));
        assert!(l.cap < d.cap && d.cap < m.cap && l.mix.crowd == 0.0 && m.mix.crowd > d.mix.crowd && m.mix.crowd_k.1 <= 30);
        assert_eq!(Depth::pick(15.0, 30, Some("max")).name, "max");
        assert!(selects("@thp", "thp", "Memory") && selects("@thp", "thp.defrag", "Memory") && !selects("@thp", "vm.swappiness", "Memory"));
        assert!(selects("@sched", "sched.nr_migrate", "Scheduler") && selects("vm.swappiness", "vm.swappiness", "Memory"));
        // vm.dirty: 1 (ref) with 0.25, 0.5, 2 measured, optimum 0.5 -> geometric midpoints 0.35 and 0.71.
        let mut fs = vec![Factor { key: "vm.dirty".into(), reference: "1".into(), values: vec!["0.25".into(), "0.5".into(), "2".into()] }];
        let opt: Cfg = vec![("vm.dirty".into(), "0.5".into())];
        let tries = ladder(&mut fs, &[(Goal::Gaming, opt, 0.1, 0.01)], &[], 4);
        assert_eq!(tries, vec![vec![("vm.dirty".to_string(), "0.35".to_string())], vec![("vm.dirty".to_string(), "0.71".to_string())]]);
        assert_eq!(fs[0].values.len(), 5);
        assert!(ladder(&mut fs, &[(Goal::Gaming, vec![("vm.dirty".into(), "0.5".into())], 0.1, 0.01)], &[], 4).len() <= 2, "no level twice");
    }
    #[test]
    fn lean_sessions_progress_through_the_stages() {
        let mut fs: Vec<Factor> = (0..12).map(|i| Factor { key: format!("{}.k{i}", if i % 2 == 0 { "vm" } else { "cpu" }), reference: "0".into(), values: vec!["1".into()] }).collect();
        fs.push(Factor { key: "cpu.n2".into(), reference: "10".into(), values: vec!["5".into(), "20".into(), "40".into()] });
        fs.push(Factor { key: "vm.dirty".into(), reference: "1".into(), values: vec!["0.25".into(), "0.5".into(), "2".into()] });
        let mut cal = Calibration::default();
        let mut rng = Rng::new(3);
        let mut seen: Vec<Stage> = Vec::new();
        for sess in 1..=14u64 {
            let p = progress(&cal, Phase::Idle, &fs);
            let st = p.stage();
            assert!(seen.last().map_or(true, |l| *l <= st), "stages only move forward: {seen:?} then {st:?}");
            seen.push(st);
            let dp = Depth::progressive(st, fs.len(), p.polish);
            assert!(dp.explore && dp.cap == 240);
            let prior: Vec<Cfg> = cal.rows.iter().map(|r| r.cfg.clone()).collect();
            let mut cfgs = model::design(&fs, 40, &mut rng, &dp.mix, &prior);
            if dp.ladder > 0 { cfgs.extend(ladder(&mut fs.clone(), &[(Goal::Gaming, vec![("vm.dirty".into(), "0.5".into())], 0.1, 0.01)], &[], dp.ladder)); }
            let rows = cfgs.into_iter().map(|cfg| Row { phase: Phase::Idle, sess, pos: 0.5, t: sess * 1000, kernel: String::new(), cfg,
                                                          y: [0.0, f64::NAN, f64::NAN, f64::NAN], w: 1.0, bv: calib::BENCH_VERSION, ..Default::default() }).collect();
            cal.put_session(Phase::Idle, sess, rows);
            cal.note_strategy(Phase::Idle, dp.name);
        }
        eprintln!("stages: {seen:?}");
        for st in [Stage::Base, Stage::Pairs, Stage::Crowd, Stage::Refine, Stage::Polish] { assert!(seen.contains(&st) || st == Stage::Pairs, "{st:?} reached: {seen:?}"); }
        assert!(seen.iter().filter(|s| **s == Stage::Polish).count() >= 3, "then polish rotates: {seen:?}");
        let pol: Vec<&String> = cal.strategies_of(Phase::Idle).iter().filter(|s| s.starts_with("lean/polish")).collect();
        let kinds: std::collections::BTreeSet<&String> = pol.iter().copied().collect();
        assert!(kinds.len() == 3.min(pol.len()), "polish rotates its three strategies: {pol:?}");
    }
}

#[cfg(test)]
mod sim {
    //! Offline comparison on a synthetic, noisy machine: the same number of runs spent
    //! (a) one knob at a time (reference vs candidate, keep what beats the margin) and
    //! (b) by the sequential design (space-filling start, then runs chosen by decision doubt).
    use super::*;
    fn meta() -> Meta { Meta { src: calib::PowerSrc::Rapl, cx: String::new() } }
    use centurion_helpers::model::PhaseSet;

    fn fac(k: &str, r: &str, v: &[&str]) -> Factor { Factor { key: k.into(), reference: r.into(), values: v.iter().map(|s| s.to_string()).collect() } }

    /// True utility of a configuration: mains, a synergy, a cross-cluster synergy, a redundancy, an antagonism and harmful knobs.
    /// Net utility of a configuration on a machine whose knobs matter (effects x2), minus the modesty cost of its changes.
    pub fn net(cfg: &Cfg, fs: &[Factor]) -> f64 {
        2.0 * truth(cfg) + cfg.iter().map(|(k, v)| model::modest_cost(&fs.iter().find(|f| f.key == *k).unwrap().reference, v)).sum::<f64>()
    }
    pub fn truth(cfg: &Cfg) -> f64 {
        let on = |k: &str| cfg.iter().any(|(a, _)| a == k);
        let is = |k: &str, v: &str| cfg.iter().any(|(a, b)| a == k && b == v);
        let mut y = 0.0;
        if on("vm.a") { y += 0.05 } if on("vm.b") { y += 0.04 }
        if on("vm.a") && on("vm.b") { y += 0.08 }
        if is("vm.c", "2") { y += 0.03 } if is("vm.c", "4") { y += 0.06 }
        if on("cpu.a") { y += 0.05 } if on("cpu.b") { y += 0.03 }
        if on("cpu.a") && on("cpu.c") { y += 0.06 }
        if is("vm.c", "4") && on("cpu.a") { y += 0.05 }
        if on("vm.d") { y += 0.06 } if on("vm.e") { y += 0.06 }
        if on("vm.d") && on("vm.e") { y -= 0.06 }
        if on("cpu.d") { y -= 0.05 } if on("vm.f") { y -= 0.04 }
        if on("cpu.b") && on("cpu.e") { y -= 0.08 } if on("cpu.e") { y += 0.03 }
        y
    }
    pub fn factors() -> Vec<Factor> {
        vec![fac("vm.a", "0", &["1"]), fac("vm.b", "0", &["1"]), fac("vm.c", "1", &["2", "4"]), fac("vm.d", "0", &["1"]), fac("vm.e", "0", &["1"]), fac("vm.f", "0", &["1"]),
             fac("vm.g", "0", &["1"]), fac("vm.h", "0", &["1"]), fac("cpu.a", "0", &["1"]), fac("cpu.b", "0", &["1"]), fac("cpu.c", "0", &["1"]), fac("cpu.d", "0", &["1"]),
             fac("cpu.e", "0", &["1"]), fac("cpu.f", "0", &["1"]), fac("cpu.g", "0", &["1"]), fac("cpu.h", "0", &["1"])]
    }
    pub fn noisy(cfg: &Cfg, rng: &mut Rng, sd: f64) -> f64 { 2.0 * truth(cfg) + ((rng.unit() + rng.unit() + rng.unit() + rng.unit()) - 2.0) * sd * 1.7 }

    fn one_at_a_time(fs: &[Factor], budget: usize, rng: &mut Rng, sd: f64) -> Cfg {
        let per = (budget / fs.len()).max(3);
        let mut cfg: Cfg = Vec::new();
        for f in fs {
            let nref = (per / (f.values.len() + 1)).max(1);
            let r: f64 = (0..nref + 1).map(|_| noisy(&Vec::new(), rng, sd)).sum::<f64>() / (nref + 1) as f64;
            let mut best: Option<(f64, String)> = None;
            for v in &f.values {
                let c: Cfg = vec![(f.key.clone(), v.clone())];
                let m = (0..nref + 1).map(|_| noisy(&c, rng, sd)).sum::<f64>() / (nref + 1) as f64 - r;
                if m + model::modest_cost(&f.reference, v) >= MARGIN && best.as_ref().map_or(true, |b| m > b.0) { best = Some((m, v.clone())); }
            }
            if let Some((_, v)) = best { cfg.push((f.key.clone(), v)); }
        }
        cfg
    }

    fn sequential(fs: &[Factor], budget: usize, rng: &mut Rng, sd: f64) -> (Cfg, usize) {
        let n_init = (budget as f64 * 0.55) as usize;
        let mut runs: Vec<(Cfg, f64)> = vec![(Vec::new(), noisy(&Vec::new(), rng, sd))];
        for (i, c) in model::initial_design(fs, n_init, rng).into_iter().enumerate() {
            let y = noisy(&c, rng, sd);
            runs.push((c, y));
            if i % 6 == 5 { runs.push((Vec::new(), noisy(&Vec::new(), rng, sd))); }
        }
        let rows = |runs: &[(Cfg, f64)]| -> Vec<model::Row> { runs.iter().enumerate().map(|(i, (c, y))| model::Row { cfg: c.clone(), y: [*y, f64::NAN, f64::NAN, f64::NAN], w: 1.0, sess: 1, pos: i as f64 / budget as f64, t: i as f64 * 6.0, tc: 0.0 }).collect() };
        let an_of = |runs: &[(Cfg, f64)], want: usize, rng: &mut Rng| -> Option<Analysis> {
            let ps = PhaseSet::build(fs.to_vec(), rows(runs))?;
            let fits = std::array::from_fn(|o| ps.fit_obj(o, None, 1));
            analyse(goal_models(&ps.space, &fits, Phase::Load), Phase::Load, fs, &[], want, 0, 6, rng, &Vec::new())
        };
        let mut last: Option<Cfg> = None;
        while runs.len() + 6 <= budget {
            let Some(an) = an_of(&runs, 5, rng) else { break };
            last = an.optima.iter().find(|o| o.0 == Goal::Gaming).map(|o| o.1.clone());
            if an.risk < 0.35 { break; }
            let picks = if an.picks.is_empty() { model::initial_design(fs, 5, rng) } else { an.picks };
            for c in picks { let y = noisy(&c, rng, sd); runs.push((c, y)); }
            runs.push((Vec::new(), noisy(&Vec::new(), rng, sd)));
        }
        let an = an_of(&runs, 0, rng).unwrap();
        (an.optima.iter().find(|o| o.0 == Goal::Gaming).map(|o| o.1.clone()).or(last).unwrap_or_default(), runs.len())
    }

    #[test]
    fn sequential_design_beats_one_at_a_time_at_equal_runs() {
        let fs = factors();
        let best = {
            // exhaustive optimum of the truth (2^15 * 3 configurations is too many to matter: coordinate search with restarts)
            let mut rng = Rng::new(1);
            let mut top = f64::MIN;
            for _ in 0..60 {
                let mut c: Cfg = Vec::new();
                for f in &fs { if rng.unit() < 0.4 { c.push((f.key.clone(), f.values[rng.below(f.values.len())].clone())); } }
                loop {
                    let mut imp = false;
                    for f in &fs { for v in std::iter::once(f.reference.clone()).chain(f.values.iter().cloned()) {
                        let mut d: Cfg = c.iter().filter(|(k, _)| *k != f.key).cloned().collect();
                        if v != f.reference { d.push((f.key.clone(), v)); }
                        if net(&d, &fs) > net(&c, &fs) + 1e-12 { c = d; imp = true; }
                    } }
                    if !imp { break; }
                }
                top = top.max(net(&c, &fs));
            }
            top
        };
        let (mut new, mut old, mut used) = (0.0, 0.0, 0);
        let seeds = 4;
        for s in 0..seeds {
            let (c, n) = sequential(&fs, 100, &mut Rng::new(100 + s), 0.05);
            new += net(&c, &fs) / seeds as f64; used += n;
            old += net(&one_at_a_time(&fs, n, &mut Rng::new(200 + s), 0.05), &fs) / seeds as f64;
        }
        eprintln!("true optimum {best:.3} | sequential design {new:.3} | one at a time {old:.3} | runs {}", used / seeds as usize);
        assert!(new > old + 0.01, "design {new:.3} vs one-at-a-time {old:.3}");
        assert!(new > 0.5 * best, "design reaches at least half of the achievable gain: {new:.3} of {best:.3}");
    }

    /// Sensitivity to noise and budget (slow: `cargo test -- --ignored --nocapture sweep`).
    #[test]
    #[ignore]
    fn sweep() {
        let fs = factors();
        for (sd, budget) in [(0.02, 60), (0.05, 60), (0.05, 100), (0.05, 160), (0.10, 100), (0.10, 160)] {
            let (mut new, mut old, mut nothing) = (0.0, 0.0, 0.0);
            let seeds = 3;
            for s in 0..seeds {
                let (c, n) = sequential(&fs, budget, &mut Rng::new(300 + s), sd);
                new += net(&c, &fs) / seeds as f64;
                old += net(&one_at_a_time(&fs, n, &mut Rng::new(400 + s), sd), &fs) / seeds as f64;
                nothing += net(&Vec::new(), &fs) / seeds as f64;
            }
            eprintln!("noise {sd:.2} budget {budget:>3}: design {new:+.3} | one at a time {old:+.3} | nothing {nothing:+.3}");
        }
    }

    #[test]
    fn rows_from_runs_are_signed_clipped_and_robust() {
        let s = |wake: f64, bw: f64, w: f64| -> Sample { [(Metric::WakeP99Us, wake), (Metric::MemBwGbs, bw), (Metric::PkgW, w)].into_iter().collect() };
        let mut runs: Vec<Run> = (0..3).map(|i| Run { cfg: Vec::new(), s: s(100.0, 50.0, 10.0), order: i, t: 100 + i as u64, w: 1.0, temp: None }).collect();
        runs.push(Run { cfg: vec![("k".into(), "1".into())], s: s(80.0, 55.0, 9.0), order: 3, t: 110, w: 1.0, temp: None });     // all better
        runs.push(Run { cfg: vec![("k".into(), "2".into())], s: s(100.0, 500.0, 10.0), order: 4, t: 120, w: 0.4, temp: None });  // absurd outlier on bandwidth, busy machine
        let rows = compute_rows(&runs, Phase::Load, 7, "7.0", 10, &meta());
        assert_eq!(rows.len(), 5);
        let ok = &rows[3];
        assert!(ok.y[0] > 0.2 && ok.y[1] > 0.05 && ok.y[2] > 0.05, "lower latency/power and higher bandwidth are +: {:?}", ok.y);
        assert!((rows[4].y[1] - 0.5).abs() < 1e-9, "one wild metric is clipped, not allowed to dominate: {:?}", rows[4].y);
        assert!(rows[0].y[0].abs() < 1e-9 && rows[3].pos > rows[0].pos);
        assert!(rows[4].t == 120 && rows[4].w == 0.4 && rows[4].bv == calib::BENCH_VERSION, "each row keeps its run's time, weight and bench version");
        assert!(compute_rows(&runs[..3], Phase::Load, 7, "", 10, &meta()).is_empty(), "too few runs: nothing stored");
    }
}
