//! legion-ec-sensors — root-only, read-only EC sensor stream for the Home tab.
//!
//! The embedded controller measures the dGPU temperature on its own (thermal
//! sensor on the board), so reading it does not go through the NVIDIA driver
//! and never wakes the GPU or resets its idle timer — unlike nvidia-smi/NVML.
//! Source: Lenovo WMI "Other Method" \_SB.GZFD.WMAE get (0x11), feature
//! 0x05050000 (GPUCurrentTemperature), via acpi_call — hence root.
//!
//! Started through pkexec while the Home tab is visible (read-only polkit
//! action, silent for the active local session); one JSON line every 2 s:
//!   {"gpu_temp_c": 47}        or {"gpu_temp_c": null} while the EC has no reading
//! plus, only where the firmware's capability list offers them and no hwmon
//! driver shows the fans already: "fans": {"cpu": rpm, "gpu": rpm, "pch": rpm};
//! and every 30 s on AC "charger_weak": bool (the firmware's own "this adapter
//! cannot feed full power" verdict).
//!
//! No arguments, no input. Stops on stdin EOF (tab hidden), when stdout goes
//! away, or when the parent dies. A firmware without the feature → one
//! {"error": …} line and exit 1.

use centurion_helpers::legion_wmi;
use std::io::{Read, Write};
use std::time::Duration;

const INTERVAL: Duration = Duration::from_secs(2);

fn main() {
    centurion_helpers::init();
    if unsafe { libc::geteuid() } != 0 {
        eprintln!("legion-ec-sensors must run as root (via pkexec)");
        std::process::exit(1);
    }
    unsafe {
        libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM);
        if libc::getppid() == 1 { std::process::exit(0); }  // parent already gone
    }
    // stdin EOF = the tab was hidden.
    std::thread::spawn(|| {
        let mut b = [0u8; 64];
        while matches!(std::io::stdin().read(&mut b), Ok(n) if n > 0) {}
        std::process::exit(0);
    });
    let mut out = std::io::stdout().lock();
    // A single failed EC read (the EC is busy during a profile or limit change, or another
    // acpi_call user got in between) used to end the stream: the Home tab then showed no GPU
    // temperature for the rest of the session. Only a read that never worked, or MAX_FAILS
    // failures in a row, is treated as "this firmware cannot do it".
    const MAX_FAILS: u32 = 5;
    let (mut fails, mut worked) = (0u32, false);
    let fans = legion_wmi::ec_fans();
    let has_extras = !fans.is_empty();
    const CHARGER_EVERY: u32 = 15;  // x INTERVAL = 30 s
    let (mut tick, mut weak) = (0u32, None::<bool>);
    loop {
        let mut line = match legion_wmi::ec_gpu_temp() {
            Ok(t) => { fails = 0; worked = true; serde_json::json!({"gpu_temp_c": t}) }
            Err(e) => {
                fails += 1;
                // With other EC readings on offer the stream stays useful without a GPU temperature.
                if !has_extras && (!worked || fails >= MAX_FAILS) {
                    let _ = writeln!(out, "{}", serde_json::json!({"error": e}));
                    let _ = out.flush();
                    std::process::exit(1);
                }
                serde_json::json!({"gpu_temp_c": null})
            }
        };
        if has_extras {
            let m = legion_wmi::ec_fans_read(&fans);
            if !m.is_empty() { line["fans"] = serde_json::Value::Object(m); }
        }
        if tick % CHARGER_EVERY == 0 { weak = legion_wmi::charger_weak(); }
        tick = tick.wrapping_add(1);
        if let Some(w) = weak { line["charger_weak"] = serde_json::json!(w); }
        if writeln!(out, "{line}").and_then(|_| out.flush()).is_err() { break; }
        std::thread::sleep(INTERVAL);
    }
}
