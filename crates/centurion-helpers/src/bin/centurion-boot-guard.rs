//! centurion-boot-guard — keeps an unstable boot preset from crash-looping the machine.
//!
//!   centurion-boot-guard arm      (root) start of boot: arm, or trip if the last boot died armed
//!   centurion-boot-guard watch    (root) arm, disarm after the stable window, stay up running the
//!                                  flight recorder (flight.rs); SIGTERM = clean shutdown
//!   centurion-boot-guard disarm   (root) this boot is fine (stable window over)
//!   centurion-boot-guard shutdown (root) clean shutdown/reboot: disarm + record it (OpenRC stop)
//!   centurion-boot-guard check    exit 0 = apply presets, 1 = skip (reason on stderr) — for
//!                           systemd ExecCondition= and the OpenRC scripts
//!   centurion-boot-guard status   print the state as JSON
//!   centurion-boot-guard dgpu-awake (root) re-apply the "Keep the dGPU awake" switch if it is on
//!                           (udev 90-centurion-dgpu.rules, whenever the NVIDIA GPU appears)
//!   centurion-boot-guard reset    (root) resume the presets after a trip
//!
//! See bootguard.rs for the state machine.

use centurion_helpers::{bootguard, flight};
use std::time::{Duration, Instant};

fn fail(e: String) -> i32 { eprintln!("centurion-boot-guard: {e}"); 2 }

fn arm() -> i32 {
    centurion_helpers::legion_wmi::log_state("boot-state");
    // Boot defaults for autotune: before TLP and before any preset service.
    if let Err(e) = centurion_helpers::defaults::capture() { eprintln!("centurion-boot-guard: boot-default snapshot failed: {e}"); }
    // Field stability: close the previous boot (an unclean end is recorded as such; a calibration run
    // that was in flight then becomes an unsafe set of the signature) and open this one.
    for m in centurion_helpers::stability::on_boot() { eprintln!("centurion-boot-guard: {m}"); }
    centurion_helpers::stability::record_now();
    match bootguard::arm() {
        Ok(v) => {
            if v["tripped"] == true {
                eprintln!("centurion-boot-guard: boot presets PAUSED — {}. Resume them in Centurion (Home).",
                          v["reason"].as_str().unwrap_or("previous boot failed"));
            }
            0
        }
        Err(e) => fail(e),
    }
}

fn watch() -> i32 {
    let code = arm();
    if code != 0 { return code; }
    // Paused (the presets tripped the guard): nothing to disarm, but the recorder runs and SIGTERM
    // still records the clean end — these crash-looping boots are the ones worth recording.
    let mut disarmed = bootguard::read()["state"] != "armed";
    // The signals stay blocked and are taken with sigtimedwait: a flight-recorder sample and the
    // end of the stable window are both timeouts of the same wait, and a SIGTERM cannot slip in
    // between a check and the wait.
    let mut set: libc::sigset_t = unsafe { std::mem::zeroed() };
    unsafe {
        libc::sigemptyset(&mut set);
        for s in [libc::SIGTERM, libc::SIGINT, libc::SIGHUP] { libc::sigaddset(&mut set, s); }
        libc::sigprocmask(libc::SIG_BLOCK, &set, std::ptr::null_mut());
    }
    let mut rec = flight::Recorder::open();
    if rec.is_none() { eprintln!("centurion-boot-guard: flight recorder unavailable"); }
    let window_end = Instant::now() + Duration::from_secs(bootguard::WINDOW_SECS);
    loop {
        if let Some(r) = rec.as_mut() { r.sample(); }
        // Re-read every time (a 64-byte file): a change in Health applies from the next sample.
        let interval = Duration::from_secs(flight::interval());
        let now = Instant::now();
        if !disarmed && now >= window_end {
            if let Err(e) = bootguard::disarm() { fail(e); }
            disarmed = true;
        }
        // Stay up until the service is stopped: SIGTERM at shutdown is how a clean end of this
        // boot gets recorded (for the GUI's login guard). Without a recorder nothing wakes it.
        let wait = match (disarmed, rec.is_some()) {
            (false, _) => interval.min(window_end.saturating_duration_since(now)),
            (true, true) => interval,
            (true, false) => Duration::from_secs(86_400),
        };
        let ts = libc::timespec { tv_sec: wait.as_secs() as libc::time_t, tv_nsec: wait.subsec_nanos() as libc::c_long };
        if unsafe { libc::sigtimedwait(&set, std::ptr::null_mut(), &ts) } > 0 { break; }
    }
    shutdown()
}

/// Clean end of this boot: the state log, this boot's reclaim / stall counters for autotune's
/// history (how the machine is really used; read on later boots, costs nothing while it runs),
/// then the guard's own record.
fn shutdown() -> i32 {
    centurion_helpers::legion_wmi::log_state("shutdown-state");
    if let Err(e) = centurion_helpers::autotune::evidence_record_boot() { eprintln!("centurion-boot-guard: usage history not saved: {e}"); }
    centurion_helpers::stability::on_shutdown();
    bootguard::shutdown().map_or_else(fail, |_| 0)
}

fn main() {
    centurion_helpers::init();
    let code = match std::env::args().nth(1).as_deref() {
        Some("arm") => arm(),
        Some("watch") => watch(),
        Some("disarm") => bootguard::disarm().map_or_else(fail, |_| 0),
        Some("shutdown") => shutdown(),
        Some("check") => match bootguard::should_skip() {
            None => 0,
            Some(why) => { eprintln!("centurion-boot-guard: skipping boot presets — {why}"); 1 }
        },
        Some("status") => { println!("{}", bootguard::read()); 0 }
        Some("reset") => bootguard::reset().map_or_else(fail, |v| { println!("{v}"); 0 }),
        Some("dgpu-awake") => {
            if !centurion_helpers::dgpu::awake_enabled() { 0 }
            else { centurion_helpers::dgpu::awake_apply(true).map_or_else(fail, |_| 0) }
        }
        _ => { eprintln!("usage: centurion-boot-guard arm|watch|disarm|shutdown|check|status|reset|dgpu-awake"); 2 }
    };
    std::process::exit(code);
}
