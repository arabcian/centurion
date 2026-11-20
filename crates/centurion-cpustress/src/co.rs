//! `--load co`: Curve Optimizer cycles default to 60 s. Optional `--seconds 24` quick probes
//! have 12 s of steady load and the SAME ten load-transition patterns for 1.2 s each.
//! Full cycle:
//!
//!   0-40 s   steady: one kernel (cowork.rs) as fast as it goes, the maximum-load check;
//!   40-60 s  ten 2-second steps of load changes (FLUCT): short bursts and pauses at several rates, so the
//!            core keeps leaving and entering idle states and boosting back up; chunk-by-chunk kernel swaps
//!            (a step in current); and, all-core, only part of the cores working, so Precision Boost picks
//!            a different clock for the same core. The clock is never set: the load moves it.
//!
//! The kernel rotates from cycle to cycle (`--kernel` picks the first one). All-core, the first wrong chunk
//! ends the run; per-core, it ends that core's turn as in the classic per-core test. After every phase a
//! `phase` line gives the clock the tested CPUs ran at (scaling_cur_freq; null where cpufreq is missing).

use super::*;
use crate::cowork::{self, Kernel, Scratch};

pub const CYCLE: Duration = Duration::from_secs(60);
pub const STEADY: Duration = Duration::from_secs(40);
const STEP: Duration = Duration::from_secs(2);
const QUICK: Duration = Duration::from_secs(24);
const QUICK_STEADY: Duration = Duration::from_secs(12);
const QUICK_STEP: Duration = Duration::from_millis(1200);

#[derive(Clone, Copy)]
struct Timing { steady: Duration, step: Duration }

fn timing(seconds: u64) -> Timing {
    if seconds == QUICK.as_secs() { Timing { steady: QUICK_STEADY, step: QUICK_STEP } }
    else { Timing { steady: STEADY, step: STEP } }
}

/// One fluctuation step. `on_ms`: burst length (ONE = a single chunk, ALL = the whole step, RAND = 1-200 ms
/// each time); `off_ms`: pause after each burst (RAND = 1-200 ms); `swap`: alternate the cycle's kernel
/// with the next one chunk by chunk; `eighths`: the share of the CPUs working in an all-core run.
#[derive(Clone, Copy, Debug)]
struct Step { on_ms: u32, off_ms: u32, swap: bool, eighths: usize }
const ONE: u32 = 0;
const ALL: u32 = u32::MAX;
const RAND: u32 = u32::MAX - 1;

const FLUCT: [Step; 10] = [
    Step { on_ms: 5, off_ms: 5, swap: false, eighths: 8 },        // 100 Hz load / idle
    Step { on_ms: ONE, off_ms: 1, swap: false, eighths: 8 },      // single chunks, 1 ms gaps
    Step { on_ms: 20, off_ms: 50, swap: false, eighths: 4 },      // half of the cores
    Step { on_ms: ALL, off_ms: 0, swap: true, eighths: 8 },       // kernel swaps: steps in current
    Step { on_ms: 100, off_ms: 100, swap: false, eighths: 1 },    // one core in eight: the highest boost
    Step { on_ms: 300, off_ms: 700, swap: false, eighths: 8 },    // long pauses: deep idle, a full ramp up
    Step { on_ms: RAND, off_ms: RAND, swap: false, eighths: 2 },  // irregular
    Step { on_ms: ALL, off_ms: 0, swap: true, eighths: 1 },       // swaps on a lone, fully boosted core
    Step { on_ms: 50, off_ms: 10, swap: false, eighths: 8 },      // mostly busy, short dips
    Step { on_ms: 10, off_ms: 300, swap: false, eighths: 8 },     // mostly idle, short spikes
];

/// Phase at `elapsed` into a cycle: 0 = steady, 1..=10 = fluctuation step.
fn phase_at(elapsed: Duration, span: Timing) -> usize {
    if elapsed < span.steady { 0 }
    else { (1 + ((elapsed - span.steady).as_millis() / span.step.as_millis()) as usize).min(FLUCT.len()) }
}

fn phase_name(p: usize) -> String { if p == 0 { "steady".into() } else { format!("fluct-{p}") } }

/// Whether worker `slot` of `n` works in step `step`: the working share moves along the CPUs from step to
/// step, so a different core gets to be the lone, highest-boosting one.
fn active(slot: usize, n: usize, step: usize, eighths: usize) -> bool {
    let k = (n * eighths).div_ceil(8).max(1);
    (slot + n - (step * 3) % n) % n < k
}

fn sleep_until(t: Instant, stop: &AtomicBool) {
    while !STOP.load(Ordering::Relaxed) && !stop.load(Ordering::Relaxed) {
        let now = Instant::now();
        if now >= t { return; }
        std::thread::sleep((t - now).min(Duration::from_millis(50)));
    }
}

/// The kernels this CPU can run; `refs` holds NSEEDS hashes per kernel in this order.
fn kernels() -> Vec<Kernel> { cowork::KERNELS.into_iter().filter(|k| k.supported()).collect() }

fn reference_on(cpu: usize) -> Result<Vec<u64>, String> {
    topo::pin(cpu)?;
    let mut s = Scratch::new();
    Ok(kernels().into_iter().flat_map(|k| (0..cowork::NSEEDS).map(move |seed| (k, seed))).map(|(k, seed)| cowork::chunk(k, seed, &mut s)).collect())
}

struct Job { cpu: usize, slot: usize, n: usize, kernel: usize, per_core: bool,
             start: Instant, t0: Instant, span: Timing }

/// One cycle on one CPU. Stops at the first wrong chunk (recorded in `sh`), at STOP or at `abort`.
fn worker(j: Job, ks: &[Kernel], refs: &[u64], sh: &Shared, abort: &AtomicBool) -> Result<u64, String> {
    topo::pin(j.cpu)?;
    let mut s = Scratch::new();
    let mut n = 0u64;
    let mut rng = (j.cpu as u64) << 32 ^ j.start.duration_since(j.t0).as_nanos() as u64;
    let halted = || STOP.load(Ordering::Relaxed) || abort.load(Ordering::Relaxed);
    // One chunk of kernel `k`: false when it came out wrong.
    let mut run = |k: usize, n: &mut u64| -> bool {
        let seed = *n % cowork::NSEEDS;
        let got = cowork::chunk(ks[k], seed, &mut s);
        #[cfg(feature = "fault-inject")]
        let got = if *n == 3 && j.cpu + 1 == topo::cores().iter().flat_map(|c| c.cpus.iter()).count() { got ^ 1 } else { got };
        *n += 1;
        sh.chunks.fetch_add(1, Ordering::Relaxed);
        let i = k * cowork::NSEEDS as usize + seed as usize;
        if got == refs[i] { return true; }
        sh.errors.fetch_add(1, Ordering::Relaxed);
        sh.wrong.lock().unwrap().push((secs(j.t0), j.cpu, i as u64, refs[i], got));
        false
    };
    let steady_end = j.start + j.span.steady;
    while !halted() && Instant::now() < steady_end {
        if !run(j.kernel, &mut n) { return Ok(n); }
    }
    let other = (j.kernel + 1) % ks.len();
    for (i, st) in FLUCT.iter().enumerate() {
        let end = steady_end + j.span.step * (i as u32 + 1);
        if !j.per_core && !active(j.slot, j.n, i, st.eighths) { sleep_until(end, abort); continue; }
        let mut flip = false;
        while !halted() && Instant::now() < end {
            let mut rand_ms = || 1 + cowork::splitmix(&mut rng) % 200;
            let burst_end = match st.on_ms {
                ALL => end,
                ONE => Instant::now(),
                RAND => Instant::now() + Duration::from_millis(rand_ms()),
                ms => Instant::now() + Duration::from_millis(ms as u64),
            };
            loop {
                let k = if st.swap && flip { other } else { j.kernel };
                flip = !flip;
                if !run(k, &mut n) { return Ok(n); }
                let now = Instant::now();
                if now >= burst_end || now >= end || halted() { break; }
            }
            let off = match st.off_ms { RAND => rand_ms(), ms => ms as u64 };
            if off > 0 { sleep_until((Instant::now() + Duration::from_millis(off)).min(end), abort); }
        }
    }
    Ok(n)
}

/// Current clock of `cpu` in MHz (cpufreq: APERF/MPERF based on x86), None where cpufreq is missing.
fn mhz(cpu: usize) -> Option<f64> {
    std::fs::read_to_string(format!("/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq")).ok()?.trim().parse::<f64>().ok().map(|k| k / 1000.0)
}

/// Clock samples of one phase.
#[derive(Default)]
struct Clock { sum: f64, count: u32, max: f64 }

impl Clock {
    /// avg: mean over the samples of the CPUs' mean clock; max: the highest clock of any one CPU.
    fn sample(&mut self, cpus: &[usize]) {
        let v: Vec<f64> = cpus.iter().filter_map(|&c| mhz(c)).collect();
        if v.is_empty() { return; }
        self.sum += v.iter().sum::<f64>() / v.len() as f64;
        self.count += 1;
        self.max = v.iter().copied().fold(self.max, f64::max);
    }
    fn json(&self) -> (Value, Value) {
        if self.count == 0 { return (Value::Null, Value::Null); }
        (json!((self.sum / self.count as f64).round()), json!(self.max.round()))
    }
}

/// What every cycle of a run shares.
struct Ctx { ks: Arc<Vec<Kernel>>, refs: Arc<Vec<u64>>, sh: Arc<Shared>, t0: Instant }

/// Runs one cycle on `cpus` (one worker each) and prints its phases; true when it ended with an error.
fn cycle(cpus: &[usize], per_core: bool, kernel: usize, cyc: u32, cx: &Ctx, p: &mut Printer, span: Timing) -> bool {
    let (ks, sh, t0) = (&cx.ks, &cx.sh, cx.t0);
    let start = Instant::now();
    let abort = Arc::new(AtomicBool::new(false));
    let errs_before = sh.errors.load(Ordering::Relaxed);
    let hs: Vec<_> = cpus.iter().enumerate().map(|(slot, &cpu)| {
        let (ks, refs, sh, abort) = (ks.clone(), cx.refs.clone(), sh.clone(), abort.clone());
        let j = Job { cpu, slot, n: cpus.len(), kernel, per_core, start, t0, span };
        (cpu, std::thread::spawn(move || worker(j, &ks, &refs, &sh, &abort)))
    }).collect();
    let (mut phase, mut clock) = (0usize, Clock::default());
    let end_phase = |phase: usize, clock: &Clock| {
        let (avg, max) = clock.json();
        let mut v = json!({"ev": "phase", "t": secs(t0), "cycle": cyc, "phase": phase_name(phase), "kernel": ks[kernel].name(),
                           "mhz_avg": avg, "mhz_max": max});
        if per_core { v["cpu"] = json!(cpus[0]); }
        emit(&v);
    };
    while hs.iter().any(|(_, h)| !h.is_finished()) {
        std::thread::sleep(Duration::from_millis(100));
        let now = phase_at(start.elapsed(), span);
        if now != phase { end_phase(phase, &clock); phase = now; clock = Clock::default(); }
        clock.sample(cpus);
        // All-core: the first wrong chunk ends every thread; there is nothing left to learn from this cycle.
        if !per_core && sh.errors.load(Ordering::Relaxed) > errs_before { abort.store(true, Ordering::Relaxed); }
        p.drain(sh);
        p.progress(sh, t0, per_core.then(|| cpus[0]));
    }
    end_phase(phase, &clock);
    for (cpu, h) in hs {
        match h.join() {
            Ok(Err(e)) => eprintln!("centurion-cpustress: CPU {cpu}: {e}"),
            Err(_) => eprintln!("centurion-cpustress: CPU {cpu}: worker panicked"),
            _ => {}
        }
    }
    p.drain(sh);
    sh.errors.load(Ordering::Relaxed) > errs_before
}

pub fn run(a: &Args) -> i32 {
    let ks = kernels();
    let cores = topo::cores();
    let firsts: Vec<usize> = cores.iter().map(|c| c.cpus[0]).collect();
    let tested: Vec<usize> = if a.per_core {
        match &a.cpus {
            Some(l) => {
                if let Some(bad) = l.iter().find(|c| !firsts.contains(c)) {
                    return end("init_failed", 0.0, 0, 0, &[], &format!("CPU {bad} is not the first online CPU of a core"));
                }
                l.clone()
            }
            None => firsts.clone(),
        }
    } else { let mut o: Vec<usize> = cores.iter().flat_map(|c| c.cpus.iter().copied()).collect(); o.sort_unstable(); o };
    if tested.is_empty() { return end("init_failed", 0.0, 0, 0, &[], "no online CPU"); }
    let mut cand = tested.clone();
    cand.extend(firsts.iter().filter(|c| !tested.contains(c)));
    let (refs, outvoted) = match reference_by(&cand, reference_on) {
        Ok(r) => r,
        Err(e) => return end("fail", 0.0, 0, 1, &[], &e),
    };
    let t0 = Instant::now();
    let span = timing(a.seconds);
    let names: Vec<&str> = ks.iter().map(|k| k.name()).collect();
    emit(&json!({"ev": "start", "mode": if a.per_core { "percore" } else { "allcore" }, "load": "co", "seconds": a.seconds,
                 "steady_seconds": span.steady.as_secs(), "cycles": a.cycles, "transitions": true, "kernels": names,
                 "wide": cowork::wide_isa(), "cpus": tested}));
    let sh = Arc::new(Shared::default());
    sh.errors.fetch_add(outvoted.len() as u64, Ordering::Relaxed);
    sh.wrong.lock().unwrap().extend(outvoted);
    let mut p = Printer { lines: 0, last_progress: -1.0, failed: Vec::new() };
    p.drain(&sh);
    let cx = Ctx { ks: Arc::new(ks), refs: Arc::new(refs), sh: sh.clone(), t0 };
    let ks = &cx.ks;
    let kernel_of = |c: u32| (a.kernel + c as usize) % ks.len();
    'outer: for c in 0..a.cycles {
        if a.per_core {
            for &cpu in &tested {
                if STOP.load(Ordering::SeqCst) { break 'outer; }
                if p.failed.contains(&cpu) { continue; }
                emit(&json!({"ev": "core", "t": secs(t0), "cpu": cpu, "cycle": c, "kernel": ks[kernel_of(c)].name()}));
                let (errs, chunks) = (sh.errors.load(Ordering::Relaxed), sh.chunks.load(Ordering::Relaxed));
                cycle(&[cpu], true, kernel_of(c), c, &cx, &mut p, span);
                emit(&json!({"ev": "core_end", "t": secs(t0), "cpu": cpu, "chunks": sh.chunks.load(Ordering::Relaxed) - chunks,
                             "errors": sh.errors.load(Ordering::Relaxed) - errs}));
            }
        } else if STOP.load(Ordering::SeqCst) || cycle(&tested, false, kernel_of(c), c, &cx, &mut p, span) {
            break;
        }
    }
    let (errors, chunks, t) = (sh.errors.load(Ordering::Relaxed), sh.chunks.load(Ordering::Relaxed), secs(t0));
    let mut failed = p.failed;
    failed.sort_unstable();
    if errors > 0 { end("fail", t, chunks, errors, &failed, "wrong results") }
    else if STOP.load(Ordering::SeqCst) { end("stopped", t, chunks, 0, &[], "stopped") }
    else { end("pass", t, chunks, 0, &[], "") }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cycle_is_40_seconds_steady_then_20_of_steps() {
        assert_eq!(STEADY + STEP * FLUCT.len() as u32, CYCLE);
        let full = timing(60);
        assert_eq!(phase_at(Duration::from_millis(39_999), full), 0);
        assert_eq!(phase_at(STEADY, full), 1);
        assert_eq!(phase_at(Duration::from_millis(41_999), full), 1);
        assert_eq!(phase_at(Duration::from_millis(42_000), full), 2);
        assert_eq!(phase_at(Duration::from_millis(59_999), full), 10);
        assert_eq!(phase_at(Duration::from_secs(75), full), 10);  // late sample stays on last step
    }

    #[test]
    fn quick_cycle_keeps_all_transition_patterns() {
        let quick = timing(24);
        assert_eq!(quick.steady + quick.step * FLUCT.len() as u32, QUICK);
        assert_eq!(phase_at(Duration::from_millis(11_999), quick), 0);
        assert_eq!(phase_at(Duration::from_millis(12_000), quick), 1);
        assert_eq!(phase_at(Duration::from_millis(13_199), quick), 1);
        assert_eq!(phase_at(Duration::from_millis(13_200), quick), 2);
        assert_eq!(phase_at(Duration::from_millis(23_999), quick), 10);
    }

    #[test]
    fn working_share_and_rotation() {
        for n in [1, 2, 6, 16, 32] {
            for (i, st) in FLUCT.iter().enumerate() {
                let k = (0..n).filter(|&s| active(s, n, i, st.eighths)).count();
                assert_eq!(k, (n * st.eighths).div_ceil(8).max(1), "n {n} step {i}");
            }
        }
        // The lone core of the two one-in-eight steps is not the same core.
        let lone = |step| (0..16).find(|&s| active(s, 16, step, 1)).unwrap();
        assert_ne!(lone(4), lone(7));
    }
}
