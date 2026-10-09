//! centurion-boot-guard — keeps an unstable boot preset from crash-looping the machine.
//!
//!   centurion-boot-guard arm      (root) start of boot: arm, or trip if the last boot died armed
//!   centurion-boot-guard watch    (root) arm, disarm after the stable window, stay up; SIGTERM =
//!                                  clean shutdown (systemd Type=simple; OpenRC uses arm + stop)
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

use centurion_helpers::bootguard;
use std::sync::atomic::{AtomicBool, Ordering};

static STOP: AtomicBool = AtomicBool::new(false);
extern "C" fn on_term(_: libc::c_int) { STOP.store(true, Ordering::SeqCst); }

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
    if code != 0 || bootguard::read()["state"] != "armed" { return code; }
    for s in [libc::SIGTERM, libc::SIGINT, libc::SIGHUP] {
        unsafe { libc::signal(s, on_term as *const () as libc::sighandler_t) };
    }
    // Sleep in short steps so a shutdown within the window disarms promptly.
    let end = std::time::Instant::now() + std::time::Duration::from_secs(bootguard::WINDOW_SECS);
    while std::time::Instant::now() < end && !STOP.load(Ordering::SeqCst) {
        std::thread::sleep(std::time::Duration::from_millis(250));
    }
    if !STOP.load(Ordering::SeqCst) {
        if let Err(e) = bootguard::disarm() { fail(e); }
        // Stay up until the service is stopped: SIGTERM at shutdown is how a
        // clean end of this boot gets recorded (for the GUI's login guard).
        // Blocked in sigsuspend() until then (it used to wake once a second for the whole uptime).
        // The signals are blocked outside sigsuspend, so one cannot slip in between the check and the wait.
        unsafe {
            let (mut block, mut old): (libc::sigset_t, libc::sigset_t) = (std::mem::zeroed(), std::mem::zeroed());
            libc::sigemptyset(&mut block);
            for s in [libc::SIGTERM, libc::SIGINT, libc::SIGHUP] { libc::sigaddset(&mut block, s); }
            libc::sigprocmask(libc::SIG_BLOCK, &block, &mut old);
            while !STOP.load(Ordering::SeqCst) { libc::sigsuspend(&old); }
            libc::sigprocmask(libc::SIG_SETMASK, &old, std::ptr::null_mut());
        }
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
