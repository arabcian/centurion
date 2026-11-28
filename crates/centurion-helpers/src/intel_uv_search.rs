//! Automatic Intel undervolt search (XTU Speed Optimizer / AI Assist counterpart, conservative).
//!
//! Core + cache (one plane since Skylake) are lowered together in `step` mV steps. Each step runs
//! centurion-cpustress core by core (AVX2, SSE when the CPU has none) and fails on a wrong result,
//! a machine check or a lockup in the kernel log. The value under test is written to disk (and
//! synced) before it is applied: if the machine dies, the next run sees a test left over from an
//! earlier boot and counts that value as failed. The result is the last passing value plus a
//! MARGIN_MV safety margin; it is applied, never stored as the boot profile by itself. Every other
//! voltage plane keeps the offset it had when the run started.
//!
//! Limits: a corrected machine check reaches the kernel log at once through CMCI, but where the
//! kernel polls instead (mce check_interval, 300 s by default) it can show up during a later step
//! and fail that one; MARGIN_MV covers the step in between. A kernel log that cannot be read
//! fails the step (and stops a run before it starts).
//!
//!   /var/lib/centurion/intel-uv-search.json  (root-owned)
//!   {"step", "floor", "seconds", "good", "testing"?, "failed"?, "boot"}

use crate::intel_uv::{self, Profile};
use serde_json::{json, Value};
use std::sync::atomic::{AtomicBool, Ordering};

pub const STATE_FILE: &str = "/var/lib/centurion/intel-uv-search.json";
/// Added back (towards 0 mV) to the last passing offset.
pub const MARGIN_MV: f64 = 10.0;

#[derive(Debug, Clone, PartialEq)]
pub struct Search { pub step: f64, pub floor: f64, pub seconds: u64, pub good: f64, pub testing: Option<f64>, pub failed: Option<f64>, pub boot: String }

#[derive(Debug, PartialEq)]
pub enum Next { Test(f64), Done(String) }

impl Search {
    pub fn new(step: f64, floor: f64, seconds: u64) -> Result<Search, String> {
        if !(1.0..=25.0).contains(&step) { return Err("step must be 1..25 mV".into()); }
        let lo = crate::intel_uv::uv_floor_mv();
        if !(lo..=-5.0).contains(&floor) { return Err(format!("floor must be {lo}..-5 mV on this CPU")); }
        if !(5..=600).contains(&seconds) { return Err("seconds per core must be 5..600".into()); }
        Ok(Search { step, floor, seconds, good: 0.0, testing: None, failed: None, boot: String::new() })
    }

    /// What to do now. A test still marked in the state belongs either to an earlier boot (the
    /// machine died under it: failed) or to this one (the run was killed: test it again).
    pub fn next(&mut self, boot_now: &str) -> Next {
        if let Some(t) = self.testing {
            if self.boot != boot_now {
                self.testing = None;
                self.failed = Some(t);
                return Next::Done(format!("the machine went down while testing {t} mV — counted as failed"));
            }
            return Next::Test(t);
        }
        if let Some(f) = self.failed { return Next::Done(format!("{f} mV failed")); }
        let cand = self.good - self.step;
        if cand < self.floor { return Next::Done(format!("reached the floor {} mV", self.floor)); }
        Next::Test(cand)
    }

    pub fn record(&mut self, mv: f64, pass: bool) {
        self.testing = None;
        if pass { self.good = mv } else { self.failed = Some(mv) }
    }

    /// The offset to use: the last passing one plus the margin, never positive.
    pub fn result(&self) -> f64 { (self.good + MARGIN_MV).min(0.0) }

    pub fn to_json(&self) -> Value {
        json!({"step": self.step, "floor": self.floor, "seconds": self.seconds, "good": self.good,
               "testing": self.testing, "failed": self.failed, "boot": self.boot})
    }

    pub fn from_json(v: &Value) -> Option<Search> {
        let mut s = Search::new(v["step"].as_f64()?, v["floor"].as_f64()?, v["seconds"].as_u64()?).ok()?;
        s.good = v["good"].as_f64().filter(|g| (s.floor..=0.0).contains(g))?;
        // `testing` is applied again as it is (same boot): it, like `failed`, must lie in floor..0 too.
        let offset = |k: &str| match &v[k] {
            Value::Null => Some(None),
            x => x.as_f64().filter(|m| (s.floor..=0.0).contains(m)).map(Some),
        };
        s.testing = offset("testing")?;
        s.failed = offset("failed")?;
        s.boot = v["boot"].as_str()?.to_owned();
        Some(s)
    }
}

pub fn load() -> Option<Search> {
    Search::from_json(&serde_json::from_str(&crate::read_root_file(STATE_FILE, 16 * 1024)?).ok()?)
}

fn save(s: &Search) -> Result<(), String> {
    crate::secure_dir("/var/lib/centurion")?;
    crate::write_root_file(STATE_FILE, s.to_json().to_string().as_bytes())?;
    unsafe { libc::sync() };  // on disk before the value is applied: a crash must find it
    Ok(())
}

pub fn clear() { let _ = std::fs::remove_file(STATE_FILE); }

/// Core + cache at `mv`; every other plane keeps the offset it had when the search started
/// (`apply_ex` would otherwise put the planes a profile leaves out back to 0 mV).
fn core_cache(mv: f64, others: &[(&'static str, u64, f64)]) -> Profile {
    let mut voltage = vec![("core", 0, mv), ("cache", 2, mv)];
    voltage.extend_from_slice(others);
    Profile { voltage, allow_positive: others.iter().any(|o| o.2 > 0.0), ..Default::default() }
}

/// Current offsets of the planes the search does not touch (non-zero ones only).
fn other_planes() -> Result<Vec<(&'static str, u64, f64)>, String> {
    let msr = intel_uv::Msr::open(true).map_err(|e| format!("/dev/cpu/0/msr: {e}"))?;
    let mut v = Vec::new();
    for &(k, idx, _) in intel_uv::cpu_planes().filter(|p| p.0 != "core" && p.0 != "cache") {
        let r = msr.mailbox(intel_uv::uv_read_cmd(idx)).map_err(|e| format!("{k}: {e}"))?;
        if intel_uv::mb_status(r) == 0 && intel_uv::decode_mv(r) != 0.0 { v.push((k, idx, intel_uv::decode_mv(r))); }
    }
    Ok(v)
}

/// apply_atomic + the curve journal (Health → Curve stability attributes kernel errors to it).
fn apply_cc(mv: f64, others: &[(&'static str, u64, f64)], what: &str) -> Value {
    let out = intel_uv::apply_atomic(&core_cache(mv, others));
    if out["ok"] == true { crate::ocjournal::record("cpu_uv", &format!("uvsearch {what}:{mv}")); }
    out
}

/// Whether a mailbox read response carries the offset `mv` (the quantisation step is ~0.98 mV).
fn offset_matches(resp: u64, mv: f64) -> bool {
    intel_uv::mb_status(resp) == 0 && (intel_uv::decode_mv(resp) - mv).abs() <= 1.0
}

/// The candidate must still be in force after the stress run: the service, the GUI or another tool
/// may have written another offset meanwhile, and then the verdict says nothing about `mv`.
fn candidate_active(mv: f64) -> Result<(), String> {
    let msr = intel_uv::Msr::open(true).map_err(|e| format!("/dev/cpu/0/msr: {e}"))?;
    for (name, idx) in [("core", 0u64), ("cache", 2u64)] {
        let r = msr.mailbox(intel_uv::uv_read_cmd(idx)).map_err(|e| format!("{name}: {e}"))?;
        if !offset_matches(r, mv) {
            return Err(format!("{name} reads {:.2} mV (status {:#04x}), not the {mv} mV under test", intel_uv::decode_mv(r), intel_uv::mb_status(r)));
        }
    }
    Ok(())
}

/// Kernel-log sequence number now (events after it are this step's).
fn klog_seq() -> Result<u64, String> {
    let s = crate::health::scan(0);
    s["last_seq"].as_u64().filter(|_| s["ok"] == true).ok_or_else(|| s["error"].as_str().unwrap_or("kernel log unreadable").to_owned())
}

/// Machine checks / lockups logged after `since`; Err when the log could not be read.
fn bad_events(since: u64) -> Result<Vec<String>, String> {
    let s = crate::health::scan(since);
    if s["ok"] != true { return Err(s["error"].as_str().unwrap_or("kernel log unreadable").to_owned()); }
    Ok(s["events"].as_array().into_iter().flatten()
        .filter(|e| matches!(e["kind"].as_str(), Some("mce" | "lockup" | "oops")))
        .map(|e| e["title"].as_str().unwrap_or("").to_owned()).collect())
}

/// (pass, what to print) for one step: a pass needs a clean exit *and* a kernel log that was read.
fn verdict(code: i32, events: Result<Vec<String>, String>) -> (bool, String) {
    match (code, events) {
        (0, Ok(e)) if e.is_empty() => (true, "pass".into()),
        (0, Ok(e)) => (false, format!("kernel: {}", e.join(", "))),
        (0, Err(e)) => (false, format!("kernel log unreadable ({e}) — counted as failed")),
        _ => (false, "wrong results".into()),
    }
}

/// centurion-cpustress next to this binary, else from PATH.
fn stress_bin() -> std::path::PathBuf {
    std::env::current_exe().ok().and_then(|p| Some(p.parent()?.join("centurion-cpustress")))
        .filter(|p| p.is_file()).unwrap_or_else(|| "centurion-cpustress".into())
}

// A signal during an unstable offset is a failure, even if SSE would pass.
fn exit_code(status: std::process::ExitStatus) -> i32 { status.code().unwrap_or(1) }

/// Exit code of one per-core pass: 0 pass, 1 errors, 3 cannot run, 4 stopped. AVX2 first, SSE
/// when AVX2 cannot run here. `stop` (the caller's signal flag) ends the stressor too: it does not
/// get a terminal's SIGINT when the search runs under ssh -T, a service or `kill`.
fn stress(seconds: u64, stop: &AtomicBool) -> i32 {
    let run = |load: &str| -> i32 {
        let Ok(mut child) = std::process::Command::new(stress_bin())
            .args(["percore", "--load", load, "--seconds", &seconds.to_string()])
            .stdout(std::process::Stdio::null()).spawn() else { return 3 };
        loop {
            match child.try_wait() {
                Ok(Some(s)) => return exit_code(s),
                Ok(None) if stop.load(Ordering::SeqCst) => {
                    unsafe { libc::kill(child.id() as libc::pid_t, libc::SIGTERM) };  // it stops after the chunk
                    let _ = child.wait();
                    return 4;
                }
                Ok(None) => std::thread::sleep(std::time::Duration::from_millis(100)),
                Err(_) => { let _ = child.kill(); let _ = child.wait(); return 3; }
            }
        }
    };
    match run("avx2") { 3 if !stop.load(Ordering::SeqCst) => run("sse"), c => c }
}

/// Runs (or resumes) the search; prints progress. Returns the process exit code. `stop` is set by
/// the caller's SIGINT/SIGTERM/SIGHUP handler.
pub fn run(fresh: Option<Search>, stop: &AtomicBool) -> i32 {
    if let Some(r) = crate::refuse_during_calibration() { eprintln!("{}", r["error"].as_str().unwrap_or("calibration running")); return 1; }
    if !intel_uv::uv_map_verified() { eprintln!("voltage-domain map not verified for this CPU — no search"); return 1; }
    // A crash is told from a killed run by the boot id; without one every crash would be retried.
    let boot = crate::bootguard::boot_id();
    if boot.is_empty() { eprintln!("/proc/sys/kernel/random/boot_id unreadable — a crash could not be detected, no search"); return 1; }
    if let Err(e) = klog_seq() { eprintln!("kernel log unreadable ({e}) — machine checks could not be seen, no search"); return 1; }
    let lock = intel_uv::probe_uv_lock();
    if lock["uv_locked"] != false { eprintln!("voltage offsets are not writable here: {}", lock["message"].as_str().or(lock["error"].as_str()).unwrap_or("?")); return 1; }
    let others = match other_planes() { Ok(o) => o, Err(e) => { eprintln!("reading the other voltage planes: {e}"); return 1; } };
    if !others.is_empty() {
        println!("kept as they are: {}", others.iter().map(|o| format!("{} {:.1} mV", o.0, o.2)).collect::<Vec<_>>().join(", "));
    }
    let mut s = match (fresh, load()) {
        (Some(f), _) => f,
        (None, Some(s)) => s,
        (None, None) => { eprintln!("no search in progress — start one with: centurion-intel-uv uvsearch start"); return 2; }
    };
    loop {
        match s.next(&boot) {
            Next::Done(why) => {
                if let Err(e) = save(&s) { eprintln!("state: {e}"); }
                let r = s.result();
                println!("search finished: {why}. Last passing offset {} mV; applying {r} mV (+{MARGIN_MV} mV margin) to core and cache.", s.good);
                let out = apply_cc(r, &others, "result");
                if out["ok"] != true { eprintln!("apply failed: {out}"); return 1; }
                println!("Test it in normal use (idle, suspend/resume), then store it in the Intel tab (Apply at boot & resume).");
                return 0;
            }
            Next::Test(_) if stop.load(Ordering::SeqCst) => {
                println!("stopped; still at {} mV. Run 'uvsearch' again to continue.", s.good);
                return 4;
            }
            Next::Test(mv) => {
                s.testing = Some(mv);
                s.boot = boot.clone();
                if let Err(e) = save(&s) { eprintln!("state not saved, stopping: {e}"); return 1; }
                println!("testing {mv} mV ({} s per core)…", s.seconds);
                let out = apply_cc(mv, &others, "test");
                if out["ok"] != true {
                    eprintln!("could not apply {mv} mV: {out}");
                    s.testing = None;
                    let _ = save(&s);
                    let _ = apply_cc(s.good, &others, "back");
                    return 1;
                }
                // Checked at the start, so a failure here is counted against the step (verdict).
                let seq = klog_seq();
                let code = stress(s.seconds, stop);
                let events = seq.and_then(bad_events);
                // Before any verdict: if another writer changed the offset, this result is not about `mv`.
                if code != 4 && code != 3 {
                    if let Err(e) = candidate_active(mv) {
                        eprintln!("the offset under test was changed during the stress run ({e}) — step discarded, search stopped");
                        s.testing = None;
                        let _ = save(&s);
                        let _ = apply_cc(s.good, &others, "back");
                        return 1;
                    }
                }
                match code {
                    4 => {  // stopped (Ctrl-C / signal): no verdict, back to the last good value
                        s.testing = None;
                        let _ = save(&s);
                        let _ = apply_cc(s.good, &others, "back");
                        println!("stopped; back to {} mV. Run 'uvsearch' again to continue.", s.good);
                        return 4;
                    }
                    3 => { eprintln!("centurion-cpustress could not run"); s.testing = None; let _ = save(&s); let _ = apply_cc(s.good, &others, "back"); return 1; }
                    _ => {}
                }
                let (pass, what) = verdict(code, events);
                println!("  {mv} mV: {what}");
                s.record(mv, pass);
                if let Err(e) = save(&s) { eprintln!("state: {e}"); }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn steps_down_until_failure() {
        let mut s = Search::new(5.0, -100.0, 20).unwrap();
        assert_eq!(s.next("b1"), Next::Test(-5.0));
        s.record(-5.0, true);
        assert_eq!(s.next("b1"), Next::Test(-10.0));
        s.record(-10.0, false);
        assert!(matches!(s.next("b1"), Next::Done(_)));
        assert_eq!((s.good, s.result()), (-5.0, 0.0));  // margin never goes positive
    }

    #[test]
    fn stops_at_floor() {
        let mut s = Search::new(10.0, -30.0, 20).unwrap();
        for mv in [-10.0, -20.0, -30.0] { assert_eq!(s.next("b"), Next::Test(mv)); s.record(mv, true); }
        assert!(matches!(s.next("b"), Next::Done(_)));
        assert_eq!(s.result(), -20.0);
    }

    #[test]
    fn crash_in_earlier_boot_counts_as_failure() {
        let mut s = Search::new(5.0, -100.0, 20).unwrap();
        s.good = -60.0;
        s.testing = Some(-65.0);
        s.boot = "old".into();
        // Same boot: the run was only killed, test again.
        assert_eq!(s.clone().next("old"), Next::Test(-65.0));
        assert!(matches!(s.next("new"), Next::Done(_)));
        assert_eq!((s.failed, s.testing, s.result()), (Some(-65.0), None, -50.0));
    }

    #[test]
    fn stressor_killed_by_signal_is_a_failure() {
        use std::os::unix::process::ExitStatusExt;
        assert_eq!(exit_code(std::process::ExitStatus::from_raw(libc::SIGSEGV)), 1);
        assert_eq!(exit_code(std::process::ExitStatus::from_raw(3 << 8)), 3);
        assert_eq!(exit_code(std::process::ExitStatus::from_raw(0)), 0);
    }

    #[test]
    fn candidate_must_still_be_in_force() {
        let ticks = |mv: f64| ((mv * 1.024).round() as i64 & 0x7FF) as u64;
        let resp = |mv: f64| ticks(mv) << 21;
        assert!(offset_matches(resp(-50.0), -50.0));
        assert!(!offset_matches(resp(-30.0), -50.0));   // someone else wrote -30 mV during the run
        assert!(!offset_matches(resp(-50.0) | (1 << 32), -50.0));  // non-zero mailbox status
    }

    #[test]
    fn unknown_kernel_log_is_never_a_pass() {
        assert!(verdict(0, Ok(vec![])).0);
        assert!(!verdict(0, Err("kernel log unreadable".into())).0);  // scan failed: no evidence = fail
        assert!(!verdict(0, Ok(vec!["mce".into()])).0);
        assert!(!verdict(1, Ok(vec![])).0);
        assert!(!verdict(2, Ok(vec![])).0);
    }

    #[test]
    fn validation_and_roundtrip() {
        assert!(Search::new(0.0, -100.0, 20).is_err());
        assert!(Search::new(5.0, -300.0, 20).is_err());
        assert!(Search::new(5.0, 0.0, 20).is_err());
        assert!(Search::new(5.0, -100.0, 1).is_err());
        let mut s = Search::new(5.0, -100.0, 20).unwrap();
        s.good = -40.0; s.testing = Some(-45.0); s.boot = "x".into();
        assert_eq!(Search::from_json(&s.to_json()), Some(s.clone()));
        let mut bad = s.to_json();
        bad["good"] = json!(-500.0);  // outside floor..0: a tampered or stale file is not resumed
        assert_eq!(Search::from_json(&bad), None);
        for (k, v) in [("testing", json!(-500.0)), ("testing", json!(5.0)), ("testing", json!("-45")), ("failed", json!(-101.0))] {
            let mut bad = s.to_json();
            bad[k] = v.clone();
            assert_eq!(Search::from_json(&bad), None, "{k} = {v}");
        }
        let mut none = s.to_json();
        none["testing"] = Value::Null;
        assert_eq!(Search::from_json(&none).map(|x| x.testing), Some(None));
    }
}
