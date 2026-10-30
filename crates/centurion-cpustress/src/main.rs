//! centurion-cpustress - CPU stability test for Curve Optimizer / undervolt offsets. Runs as the user
//! (never through pkexec) and touches no clocks or curves: it only loads the CPU while the user's curve is in effect.
//!
//!   centurion-cpustress info
//!       {"cores": [{"cpus": [0, 16], "l3": 0}, ...], "loads": {"sse": true, "avx2": true, "avx512": true}}
//!   centurion-cpustress percore --load <sse|avx2|avx512> --seconds <per core> [--cycles N] [--transitions] [--cpus LIST]
//!       one thread, moved from physical core to physical core (the first logical CPU of each; LIST picks some)
//!   centurion-cpustress allcore --load <sse|avx2|avx512> --seconds <s> [--transitions]
//!       one thread on every online logical CPU
//!
//! Output: one JSON object per line on stdout.
//!   {"ev":"start","mode","load","seconds","cycles","transitions","cpus":[...]}
//!   {"ev":"core","t","cpu","cycle"}                     percore: the thread moved to this CPU
//!   {"ev":"progress","t","chunks","errors"[,"cpu"]}       at most once per second
//!   {"ev":"error","t","cpu","seed","expected","actual"}   a chunk that differs from the reference (at most 200 lines)
//!   {"ev":"core_end","t","cpu","chunks","errors"[,"skipped"]}
//!   {"ev":"end","status":"pass|fail|stopped|init_failed","t","chunks","errors","failed":[cpu...],"msg"}
//! Exit code: 0 pass, 1 errors found, 3 init failure or bad arguments, 4 stopped.
//!
//! How a result is known to be right: before the test, the reference hash of every seed is computed on two
//! different cores at the same time and must agree (a third core breaks a tie). Every chunk of the test is
//! compared with it. A core that returns one wrong chunk has failed; the per-core test moves on to the next
//! core, the all-core test stops that CPU's thread. Many unstable offsets never return a wrong number but
//! crash the machine or raise a machine check instead: the GUI covers both (kernel events, unfinished runs).
//!
//! `--transitions`: the last third of each core's time (all-core: of the run) alternates single chunks with
//! pauses of 1-200 ms, so the core keeps dropping into idle states and boosting back out of them - where
//! Curve Optimizer offsets often fail although a steady load passes.

mod topo;
mod work;

use serde_json::{json, Value};
use std::io::Write;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use work::Load;

/// Set by SIGTERM / SIGINT; the workers look at it after every chunk.
static STOP: AtomicBool = AtomicBool::new(false);
extern "C" fn on_stop(_: libc::c_int) { STOP.store(true, Ordering::SeqCst); }

const NSEEDS: u64 = 8;
const MAX_ERROR_LINES: u32 = 200;
const PAUSES_MS: [u64; 8] = [1, 3, 10, 30, 200, 2, 50, 100];

fn emit(v: &Value) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{v}");
    let _ = out.flush();
}

fn secs(t: Instant) -> f64 { (t.elapsed().as_secs_f64() * 1000.0).round() / 1000.0 }

#[derive(Debug, PartialEq)]
struct Args { per_core: bool, load: Load, seconds: u64, cycles: u32, transitions: bool, cpus: Option<Vec<usize>> }

const USAGE: &str = "usage: centurion-cpustress info | percore --load <sse|avx2|avx512> --seconds <s> [--cycles N] [--transitions] [--cpus LIST] | allcore --load <sse|avx2|avx512> --seconds <s> [--transitions]";

fn parse(argv: &[String]) -> Result<Args, String> {
    let per_core = match argv.first().map(String::as_str) {
        Some("percore") => true,
        Some("allcore") => false,
        _ => return Err(USAGE.into()),
    };
    let (mut load, mut seconds, mut cycles, mut transitions, mut cpus) = (None, None, 1, false, None);
    let mut it = argv[1..].iter();
    while let Some(flag) = it.next() {
        if flag == "--transitions" { transitions = true; continue; }
        let val = it.next().ok_or(format!("{flag} needs a value"))?;
        match flag.as_str() {
            "--load" => load = Some(Load::parse(val).ok_or("--load must be sse, avx2 or avx512")?),
            "--seconds" => seconds = Some(val.parse::<u64>().map_err(|_| "--seconds is not a number")?),
            "--cycles" if per_core => cycles = val.parse::<u32>().map_err(|_| "--cycles is not a number")?,
            "--cpus" if per_core => cpus = Some(topo::parse_list(val).filter(|l| !l.is_empty()).ok_or("--cpus is not a CPU list")?),
            _ => return Err(format!("unknown argument {flag}")),
        }
    }
    let seconds = seconds.ok_or("--seconds is required")?;
    if !(1..=86_400).contains(&seconds) { return Err("--seconds must be 1..86400".into()); }
    if !(1..=100).contains(&cycles) { return Err("--cycles must be 1..100".into()); }
    Ok(Args { per_core, load: load.ok_or("--load is required")?, seconds, cycles, transitions, cpus })
}

/// A wrong chunk: time, CPU, seed, expected, actual.
type Wrong = (f64, usize, u64, u64, u64);

/// What the workers report; the main thread prints it.
#[derive(Default)]
struct Shared { chunks: AtomicU64, errors: AtomicU64, wrong: Mutex<Vec<Wrong>> }

/// Runs chunks pinned to `cpu` until `until`, the stop flag or the first wrong chunk. Pauses after each chunk
/// from `pause_from` on (transitions). Returns the chunks done, or why the CPU could not be used.
fn worker(cpu: usize, load: Load, refs: &[u64], until: Instant, pause_from: Option<Instant>, t0: Instant, sh: &Shared) -> Result<u64, String> {
    topo::pin(cpu)?;
    let mut buf = vec![0u64; work::BUF_WORDS];
    let mut n = 0u64;
    while !STOP.load(Ordering::Relaxed) && Instant::now() < until {
        let seed = n % NSEEDS;
        let got = work::chunk(load, seed, &mut buf);
        // Test builds only: the 4th chunk on the highest-numbered CPU comes out wrong.
        #[cfg(feature = "fault-inject")]
        let got = if n == 3 && cpu + 1 == topo::cores().iter().flat_map(|c| c.cpus.iter()).count() { got ^ 1 } else { got };
        n += 1;
        sh.chunks.fetch_add(1, Ordering::Relaxed);
        if got != refs[seed as usize] {
            sh.errors.fetch_add(1, Ordering::Relaxed);
            sh.wrong.lock().unwrap().push((secs(t0), cpu, seed, refs[seed as usize], got));
            break;
        }
        if pause_from.is_some_and(|p| Instant::now() >= p) {
            std::thread::sleep(Duration::from_millis(PAUSES_MS[(n % PAUSES_MS.len() as u64) as usize]));
        }
    }
    Ok(n)
}

/// All seeds on one CPU.
fn reference_on(cpu: usize, load: Load) -> Result<Vec<u64>, String> {
    topo::pin(cpu)?;
    let mut buf = vec![0u64; work::BUF_WORDS];
    Ok((0..NSEEDS).map(|s| work::chunk(load, s, &mut buf)).collect())
}

/// The reference hashes, from two CPUs at once (a third on disagreement). A CPU outvoted on a seed is
/// returned with it as wrong. Err when no two agree.
fn reference(load: Load, candidates: &[usize]) -> Result<(Vec<u64>, Vec<Wrong>), String> {
    let mut picks: Vec<usize> = Vec::new();
    for &c in [candidates.first(), candidates.last()].into_iter().flatten().chain(candidates.iter()) {
        if !picks.contains(&c) { picks.push(c); }
    }
    let run = |cpus: &[usize]| -> Result<Vec<Vec<u64>>, String> {
        let hs: Vec<_> = cpus.iter().map(|&c| std::thread::spawn(move || reference_on(c, load))).collect();
        hs.into_iter().map(|h| h.join().map_err(|_| "reference thread panicked".to_string())?).collect()
    };
    let first: Vec<usize> = picks.iter().take(2).copied().collect();
    let mut res = run(&first)?;
    let mut used = first.clone();
    if res.len() == 2 && res[0] != res[1] {
        let Some(&third) = picks.get(2) else { return Err(format!("the reference runs on CPU {} and {} disagree", used[0], used[1])) };
        res.extend(run(&[third])?);
        used.push(third);
    }
    let mut refs = Vec::new();
    let mut wrong = Vec::new();
    for s in 0..NSEEDS as usize {
        let vals: Vec<u64> = res.iter().map(|r| r[s]).collect();
        let Some(&v) = vals.iter().find(|&&v| vals.iter().filter(|&&w| w == v).count() * 2 > vals.len()) else {
            return Err(format!("the reference runs on CPU {used:?} disagree"));
        };
        for (i, &w) in vals.iter().enumerate() { if w != v { wrong.push((0.0, used[i], s as u64, v, w)); } }
        refs.push(v);
    }
    Ok((refs, wrong))
}

/// Prints what the workers reported; `failed` collects the CPUs of every wrong chunk.
struct Printer { lines: u32, last_progress: f64, failed: Vec<usize> }

impl Printer {
    fn drain(&mut self, sh: &Shared) {
        for (t, cpu, seed, exp, act) in sh.wrong.lock().unwrap().drain(..) {
            if !self.failed.contains(&cpu) { self.failed.push(cpu); }
            if self.lines < MAX_ERROR_LINES {
                self.lines += 1;
                emit(&json!({"ev": "error", "t": t, "cpu": cpu, "seed": seed, "expected": format!("0x{exp:016x}"), "actual": format!("0x{act:016x}")}));
            }
        }
    }
    fn progress(&mut self, sh: &Shared, t0: Instant, cpu: Option<usize>) {
        let t = secs(t0);
        if t - self.last_progress < 1.0 { return; }
        self.last_progress = t;
        let mut v = json!({"ev": "progress", "t": t, "chunks": sh.chunks.load(Ordering::Relaxed), "errors": sh.errors.load(Ordering::Relaxed)});
        if let Some(c) = cpu { v["cpu"] = json!(c); }
        emit(&v);
    }
}

/// Prints the `end` line and returns the exit code that goes with its status.
fn end(status: &str, t: f64, chunks: u64, errors: u64, failed: &[usize], msg: &str) -> i32 {
    emit(&json!({"ev": "end", "status": status, "t": t, "chunks": chunks, "errors": errors, "failed": failed, "msg": msg}));
    match status { "pass" => 0, "fail" => 1, "init_failed" => 3, _ => 4 }
}

fn run(a: &Args) -> i32 {
    if !a.load.supported() { return end("init_failed", 0.0, 0, 0, &[], &format!("this CPU has no {}", a.load.name())); }
    let cores = topo::cores();
    let online: Vec<usize> = cores.iter().flat_map(|c| c.cpus.iter().copied()).collect();
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
    } else { let mut o = online.clone(); o.sort_unstable(); o };
    if tested.is_empty() { return end("init_failed", 0.0, 0, 0, &[], "no online CPU"); }

    // Reference CPUs: the tested ones first, the others as tie-breakers.
    let mut cand = tested.clone();
    cand.extend(firsts.iter().filter(|c| !tested.contains(c)));
    let (refs, outvoted) = match reference(a.load, &cand) {
        Ok(r) => r,
        Err(e) => return end("fail", 0.0, 0, 1, &[], &e),
    };
    let t0 = Instant::now();
    emit(&json!({"ev": "start", "mode": if a.per_core { "percore" } else { "allcore" }, "load": a.load.name(),
                 "seconds": a.seconds, "cycles": a.cycles, "transitions": a.transitions, "cpus": tested}));
    let sh = Arc::new(Shared::default());
    sh.errors.fetch_add(outvoted.len() as u64, Ordering::Relaxed);
    sh.wrong.lock().unwrap().extend(outvoted);
    let mut p = Printer { lines: 0, last_progress: -1.0, failed: Vec::new() };
    p.drain(&sh);
    let refs = Arc::new(refs);
    let span = Duration::from_secs(a.seconds);
    let pause_at = |start: Instant| a.transitions.then(|| start + span * 2 / 3);

    if a.per_core {
        'outer: for cycle in 0..a.cycles {
            for &cpu in &tested {
                if STOP.load(Ordering::SeqCst) { break 'outer; }
                if p.failed.contains(&cpu) { continue; }
                emit(&json!({"ev": "core", "t": secs(t0), "cpu": cpu, "cycle": cycle}));
                let errs_before = sh.errors.load(Ordering::Relaxed);
                let chunks_before = sh.chunks.load(Ordering::Relaxed);
                let start = Instant::now();
                let (sh2, refs2, load, pf) = (sh.clone(), refs.clone(), a.load, pause_at(start));
                let h = std::thread::spawn(move || worker(cpu, load, &refs2, start + span, pf, t0, &sh2));
                while !h.is_finished() {
                    std::thread::sleep(Duration::from_millis(100));
                    p.drain(&sh);
                    p.progress(&sh, t0, Some(cpu));
                }
                let r = h.join().unwrap_or_else(|_| Err("worker panicked".into()));
                p.drain(&sh);
                let errs = sh.errors.load(Ordering::Relaxed) - errs_before;
                let mut v = json!({"ev": "core_end", "t": secs(t0), "cpu": cpu, "chunks": sh.chunks.load(Ordering::Relaxed) - chunks_before, "errors": errs});
                if let Err(e) = r { v["skipped"] = json!(e); }
                emit(&v);
            }
        }
    } else {
        let start = Instant::now();
        let hs: Vec<_> = tested.iter().map(|&cpu| {
            let (sh2, refs2, load, pf) = (sh.clone(), refs.clone(), a.load, pause_at(start));
            (cpu, std::thread::spawn(move || worker(cpu, load, &refs2, start + span, pf, t0, &sh2)))
        }).collect();
        while hs.iter().any(|(_, h)| !h.is_finished()) {
            std::thread::sleep(Duration::from_millis(100));
            p.drain(&sh);
            p.progress(&sh, t0, None);
        }
        for (cpu, h) in hs {
            if let Ok(Err(e)) = h.join() { eprintln!("centurion-cpustress: CPU {cpu}: {e}"); }
        }
        p.drain(&sh);
    }
    let errors = sh.errors.load(Ordering::Relaxed);
    let chunks = sh.chunks.load(Ordering::Relaxed);
    let t = secs(t0);
    let mut failed = p.failed;
    failed.sort_unstable();
    if errors > 0 { end("fail", t, chunks, errors, &failed, "wrong results") }
    else if STOP.load(Ordering::SeqCst) { end("stopped", t, chunks, 0, &[], "stopped") }
    else { end("pass", t, chunks, 0, &[], "") }
}

fn info() -> i32 {
    let cores: Vec<Value> = topo::cores().iter().map(|c| json!({"cpus": c.cpus, "l3": c.l3})).collect();
    emit(&json!({"cores": cores, "loads": {"sse": Load::Sse.supported(), "avx2": Load::Avx2.supported(), "avx512": Load::Avx512.supported()}}));
    0
}

fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    if argv.first().map(String::as_str) == Some("info") { std::process::exit(info()); }
    match parse(&argv) {
        Ok(a) => {
            // SAFETY: the handler only stores to an atomic.
            unsafe {
                libc::signal(libc::SIGTERM, on_stop as *const () as libc::sighandler_t);
                libc::signal(libc::SIGINT, on_stop as *const () as libc::sighandler_t);
            }
            std::process::exit(run(&a))
        }
        Err(e) => std::process::exit(end("init_failed", 0.0, 0, 0, &[], &e)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(s: &str) -> Vec<String> { s.split_whitespace().map(str::to_owned).collect() }

    #[test]
    fn arguments() {
        assert_eq!(parse(&v("percore --load sse --seconds 60 --cycles 2 --transitions --cpus 0,2")).unwrap(),
                   Args { per_core: true, load: Load::Sse, seconds: 60, cycles: 2, transitions: true, cpus: Some(vec![0, 2]) });
        assert_eq!(parse(&v("allcore --seconds 5 --load avx2")).unwrap(),
                   Args { per_core: false, load: Load::Avx2, seconds: 5, cycles: 1, transitions: false, cpus: None });
        assert!(parse(&v("allcore --load sse --seconds 5 --cpus 1")).is_err());  // per-core only
        assert!(parse(&v("percore --load avx --seconds 5")).is_err());
        assert!(parse(&v("percore --load sse --seconds 0")).is_err());
        assert!(parse(&v("percore --load sse")).is_err());
        assert!(parse(&v("percore --load sse --seconds 5 --cycles 0")).is_err());
        assert!(parse(&v("burn")).is_err());
    }

    #[test]
    fn reference_agrees_across_cpus() {
        let cpus: Vec<usize> = topo::cores().iter().map(|c| c.cpus[0]).collect();
        let (refs, wrong) = reference(Load::Sse, &cpus).unwrap();
        assert_eq!(refs.len(), NSEEDS as usize);
        assert!(wrong.is_empty());
    }
}
