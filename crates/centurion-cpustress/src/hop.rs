//! `percore --load hop --seconds S [--cpus LIST]`: ONE verified thread that hops from core to core.
//!
//! The other per-core tests keep the load on one core for a whole cycle, so a core leaves idle and the
//! load moves to another core only a few times a minute. On a Ryzen desktop part the cores of a CCD share one
//! voltage rail, set for the core that asks for the most; with per-core Curve Optimizer offsets every core asks
//! for a different voltage at the same clock. When the load jumps from core A to core B, B wakes from an idle
//! state straight into its highest single-core boost while the rail moves from A's request to B's: the
//! moment CoreCycler's "CorePairs" order and its periodic suspend aim at, here thousands of times a minute.
//!
//! Each visit: pin to the next core, optionally wait (the old core goes idle first), then run 2K SSE FFT
//! squares (cowork::wake_chunk, the lightest kernel, so the clock is the highest) for the dwell, every square
//! checked against the reference. Dwell and wait come from three classes each and all nine combinations
//! rotate: dwell 0.3-1 ms (a wake burst), 2-8 ms, 20-60 ms (long enough to reach the full boost); wait none
//! (back-to-back migration), 1-5 ms (shallow idle), 30-120 ms (deep idle). The visit order covers every ordered
//! pair of the tested cores once per sweep (core i, then core i+g, for every i and every distance g), so
//! the cross-CCD and the same-CCD switches of every pair are tested. A wrong square names the core it ran on
//! and the wait before its burst (`idle_ms`).
//!
//! Output as for the other loads (start, progress, error, end); after every sweep a `phase` line
//! (`"phase":"hop"`, `"cycle"`: the sweep), and before `end` a `hops` line: {"ev":"hops","t","hops","sweeps","pairs"}.

use super::*;
use crate::cowork::{self, Scratch};

const DWELL_US: [(u64, u64); 3] = [(300, 1_000), (2_000, 8_000), (20_000, 60_000)];
const WAIT_US: [(u64, u64); 3] = [(0, 0), (1_000, 5_000), (30_000, 120_000)];

/// Visit `k` of a sweep over `n` cores: indices into the tested list. Pairs (i, i+g) for g in 1..n, i in 0..n.
pub fn visit(k: usize, n: usize) -> usize {
    let (pair, second) = (k / 2, k % 2 == 1);
    let (g, i) = (1 + pair / n, pair % n);
    if second { (i + g) % n } else { i }
}

/// Visits per sweep: two per ordered pair.
pub fn sweep_len(n: usize) -> usize { 2 * n * (n - 1) }

/// Dwell and wait of visit `k` (all nine class combinations in turn), jittered within each class.
fn timing(k: usize, rng: &mut u64) -> (Duration, Duration) {
    let pick = |(lo, hi): (u64, u64), rng: &mut u64| lo + if hi > lo { cowork::splitmix(rng) % (hi - lo + 1) } else { 0 };
    let dwell = pick(DWELL_US[k % 3], rng);
    let wait = pick(WAIT_US[(k / 3) % 3], rng);
    (Duration::from_micros(dwell), Duration::from_micros(wait))
}

fn reference_on(cpu: usize) -> Result<Vec<u64>, String> {
    topo::pin(cpu)?;
    let mut s = Scratch::new();
    Ok((0..cowork::NSEEDS).map(|seed| cowork::wake_chunk(seed, &mut s)).collect())
}

/// What the hopping thread reports besides chunks and errors.
#[derive(Default)]
struct Hops { hops: AtomicU64, sweeps: AtomicU64 }

fn worker(cpus: &[usize], refs: &[u64], until: Instant, t0: Instant, sh: &Shared, hp: &Hops) -> Result<u64, String> {
    let n = cpus.len();
    let mut s = Scratch::new();
    let mut rng = t0.elapsed().as_nanos() as u64 ^ 0x3C6E_F372_FE94_F82B;
    let mut done = 0u64;
    #[cfg(feature = "fault-inject")]
    let (last, mut on_last) = (topo::cores().iter().flat_map(|c| c.cpus.iter()).count() - 1, 0u64);
    let mut k = 0usize;
    while !STOP.load(Ordering::Relaxed) && Instant::now() < until {
        let cpu = cpus[visit(k % sweep_len(n), n)];
        let (dwell, wait) = timing(k, &mut rng);
        topo::pin(cpu)?;
        hp.hops.fetch_add(1, Ordering::Relaxed);
        sh.idle_ms.lock().unwrap().insert(cpu, wait.as_millis() as u64);
        if !wait.is_zero() { std::thread::sleep(wait); }
        let burst_end = Instant::now() + dwell;
        loop {
            let seed = done % cowork::NSEEDS;
            let got = cowork::wake_chunk(seed, &mut s);
            // Test builds only: the 4th square on the highest-numbered CPU comes out wrong.
            #[cfg(feature = "fault-inject")]
            let got = if cpu == last { on_last += 1; if on_last == 4 { got ^ 1 } else { got } } else { got };
            done += 1;
            sh.chunks.fetch_add(1, Ordering::Relaxed);
            if got != refs[seed as usize] {
                sh.errors.fetch_add(1, Ordering::Relaxed);
                sh.wrong.lock().unwrap().push((secs(t0), cpu, seed, refs[seed as usize], got));
                return Ok(done);
            }
            if Instant::now() >= burst_end || STOP.load(Ordering::Relaxed) { break; }
        }
        k += 1;
        if k.is_multiple_of(sweep_len(n)) { hp.sweeps.fetch_add(1, Ordering::Relaxed); }
    }
    Ok(done)
}

pub fn run(a: &Args) -> i32 {
    let firsts: Vec<usize> = topo::cores().iter().map(|c| c.cpus[0]).collect();
    let tested = match &a.cpus {
        Some(l) => {
            if let Some(bad) = l.iter().find(|c| !firsts.contains(c)) {
                return end("init_failed", 0.0, 0, 0, &[], &format!("CPU {bad} is not the first online CPU of a core"));
            }
            l.clone()
        }
        None => firsts.clone(),
    };
    if tested.len() < 2 { return end("init_failed", 0.0, 0, 0, &[], "--load hop needs at least two cores"); }
    let mut cand = tested.clone();
    cand.extend(firsts.iter().filter(|c| !tested.contains(c)));
    let (refs, outvoted) = match reference_by(&cand, reference_on) {
        Ok(r) => r,
        Err(e) => return end("fail", 0.0, 0, 1, &[], &e),
    };
    let t0 = Instant::now();
    emit(&json!({"ev": "start", "mode": "percore", "load": "hop", "seconds": a.seconds, "cycles": 1, "transitions": true,
                 "kernels": ["wake-fft-sse"], "cpus": tested}));
    let sh = Arc::new(Shared::default());
    sh.errors.fetch_add(outvoted.len() as u64, Ordering::Relaxed);
    sh.wrong.lock().unwrap().extend(outvoted);
    let mut p = Printer { lines: 0, last_progress: -1.0, failed: Vec::new() };
    p.drain(&sh);
    let hp = Arc::new(Hops::default());
    let until = t0 + Duration::from_secs(a.seconds);
    let h = {
        let (cpus, refs, sh, hp) = (tested.clone(), refs.clone(), sh.clone(), hp.clone());
        std::thread::spawn(move || worker(&cpus, &refs, until, t0, &sh, &hp))
    };
    let mut sweeps = 0;
    while !h.is_finished() {
        std::thread::sleep(Duration::from_millis(100));
        let now = hp.sweeps.load(Ordering::Relaxed);
        if now != sweeps {
            emit(&json!({"ev": "phase", "t": secs(t0), "cycle": sweeps, "phase": "hop", "kernel": "wake-fft-sse",
                         "mhz_avg": null, "mhz_max": null}));
            sweeps = now;
        }
        p.drain(&sh);
        p.progress(&sh, t0, None);
    }
    let incomplete = crate::worker_issue(tested[0], h.join());
    p.drain(&sh);
    let n = tested.len();
    emit(&json!({"ev": "hops", "t": secs(t0), "hops": hp.hops.load(Ordering::Relaxed), "sweeps": hp.sweeps.load(Ordering::Relaxed),
                 "pairs": n * (n - 1)}));
    let (errors, chunks, t) = (sh.errors.load(Ordering::Relaxed), sh.chunks.load(Ordering::Relaxed), secs(t0));
    let mut failed = p.failed;
    failed.sort_unstable();
    if errors > 0 { end("fail", t, chunks, errors, &failed, "wrong results") }
    else if STOP.load(Ordering::SeqCst) { end("stopped", t, chunks, 0, &[], "stopped") }
    else if let Some(issue) = incomplete { end("init_failed", t, chunks, 0, &[], &issue) }
    else { end("pass", t, chunks, 0, &[], "") }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_sweep_covers_every_ordered_pair() {
        for n in [2, 3, 4, 6, 8, 16, 32] {
            let mut pairs = std::collections::HashSet::new();
            for k in (0..sweep_len(n)).step_by(2) {
                let (a, b) = (visit(k, n), visit(k + 1, n));
                assert_ne!(a, b, "n {n} visit {k}");
                pairs.insert((a, b));
            }
            assert_eq!(pairs.len(), n * (n - 1), "n {n}");
            assert!((0..sweep_len(n)).all(|k| visit(k, n) < n));
        }
    }

    #[test]
    fn every_dwell_and_wait_class_in_nine_visits() {
        let mut rng = 1;
        let mut seen = std::collections::HashSet::new();
        for k in 0..9 {
            let (dwell, wait) = timing(k, &mut rng);
            let d = DWELL_US.iter().position(|&(lo, hi)| (lo..=hi).contains(&(dwell.as_micros() as u64))).unwrap();
            let w = WAIT_US.iter().position(|&(lo, hi)| (lo..=hi).contains(&(wait.as_micros() as u64))).unwrap();
            seen.insert((d, w));
        }
        assert_eq!(seen.len(), 9);
    }

    #[test]
    fn hops_and_checks_every_square() {
        let cpus: Vec<usize> = topo::cores().iter().map(|c| c.cpus[0]).collect();
        if cpus.len() < 2 { return; }
        let refs = reference_on(cpus[0]).unwrap();
        let (sh, hp) = (Shared::default(), Hops::default());
        let t0 = Instant::now();
        let n = worker(&cpus, &refs, t0 + Duration::from_millis(1500), t0, &sh, &hp).unwrap();
        assert!(n > 0 && sh.errors.load(Ordering::Relaxed) == 0);
        assert!(hp.hops.load(Ordering::Relaxed) >= 10);
        // A wrong reference is caught on the CPU the square ran on, with the wait before it.
        let mut bad = refs.clone();
        bad[0] ^= 1;
        let sh = Shared::default();
        worker(&cpus, &bad, Instant::now() + Duration::from_secs(5), t0, &sh, &hp).unwrap();
        let wrong = sh.wrong.lock().unwrap();
        assert_eq!(wrong.len(), 1);
        assert_eq!(wrong[0].1, cpus[0]);
        assert!(sh.idle_ms.lock().unwrap().contains_key(&cpus[0]));
    }
}
