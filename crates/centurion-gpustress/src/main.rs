//! centurion-gpustress - GPU stability test over Vulkan compute. Runs as the user (never through pkexec),
//! touches no clocks or curves: it only loads the GPU while the user's curve is in effect.
//!
//!   centurion-gpustress list                                           JSON array of the Vulkan devices
//!   centurion-gpustress vram --device <pci> --percent <10..95> --seconds <s>
//!   centurion-gpustress core --device <pci> --seconds <s>
//!
//! `vram` / `core` print one JSON object per line on stdout (`start`, `progress`, `error`, `end`);
//! diagnostics go to stderr. Exit code: 0 pass, 1 errors found, 2 device lost, 3 init failure or bad
//! arguments, 4 stopped. The full specification is docs/GPUSTRESS.md.

mod cores;
mod ctx;
mod device;
mod vram;

use ash::vk;
use serde_json::{json, Value};
use std::io::Write;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Instant;

/// Set by SIGTERM / SIGINT; the tests look at it between submissions.
static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn on_stop(_: libc::c_int) { STOP.store(true, Ordering::SeqCst); }

fn emit(v: &Value) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{v}");
    let _ = out.flush();
}

const LINES_PER_PASS: u32 = 16;
const LINES_PER_RUN: u32 = 200;

/// What `progress` and `end` report, and the caps on `error` lines.
#[derive(Default)]
struct State { passes: u64, errors: u64, bytes_checked: u64, lines: u32, last_progress: f64 }

impl State {
    /// At most one `progress` line per second.
    fn progress(&mut self, t0: Instant) {
        let t = t0.elapsed().as_secs_f64();
        if t - self.last_progress >= 1.0 {
            self.last_progress = t;
            emit(&json!({"ev": "progress", "t": secs(t), "passes": self.passes, "bytes_checked": self.bytes_checked, "errors": self.errors}));
        }
    }

    /// Counts `count` wrong words; prints the first few as `error` lines within the caps (16 per pass, 200 per
    /// run). `in_pass` is how many lines this pass has printed.
    fn wrong(&mut self, t0: Instant, in_pass: &mut u32, pass: u64, pattern: &str, count: u32, first: &[[u64; 3]]) {
        self.errors += count as u64;
        for &[word, expected, actual] in first {
            if *in_pass < LINES_PER_PASS && self.lines < LINES_PER_RUN {
                *in_pass += 1;
                self.lines += 1;
                emit(&json!({"ev": "error", "t": secs(t0.elapsed().as_secs_f64()), "pass": pass, "pattern": pattern,
                    "word": word, "expected": format!("0x{expected:08x}"), "actual": format!("0x{actual:08x}")}));
            }
        }
    }
}

/// Seconds to milliseconds precision, for the JSON lines.
fn secs(t: f64) -> f64 { (t * 1000.0).round() / 1000.0 }

/// Prints the `end` line and returns the exit code that goes with `status`, so the two always agree.
fn end(status: &str, t: f64, passes: u64, errors: u64, msg: &str) -> i32 {
    emit(&json!({"ev": "end", "status": status, "t": secs(t), "passes": passes, "errors": errors, "msg": msg}));
    match status { "pass" => 0, "fail" => 1, "device_lost" => 2, "init_failed" => 3, _ => 4 }
}

/// `end` line and exit code 3: nothing was tested.
fn init_failed(msg: &str) -> ! {
    std::process::exit(end("init_failed", 0.0, 0, 0, msg))
}

/// A Vulkan error after the test started. A lost device (or a hung one, see below) is itself a failed test and is not recreated;
/// any other error ends the run as `init_failed` (not a verdict about the GPU).
fn vk_end(e: vk::Result, t: f64, passes: u64, errors: u64) -> i32 {
    if e == vk::Result::ERROR_DEVICE_LOST { end("device_lost", t, passes, errors, "the device was lost") }
    // A fence that never signals is a hung GPU, which under a curve is a failed test.
    else if e == vk::Result::TIMEOUT { end("device_lost", t, passes, errors, "the GPU stopped responding") }
    else { end("init_failed", t, passes, errors, &format!("Vulkan error: {e}")) }
}

struct Args { vram: bool, device: String, percent: u32, seconds: u64 }

fn parse(argv: &[String]) -> Result<Args, String> {
    let vram = match argv.first().map(String::as_str) {
        Some("vram") => true,
        Some("core") => false,
        _ => return Err("usage: centurion-gpustress list | vram --device <pci> --percent <10..95> --seconds <s> | core --device <pci> --seconds <s>".into()),
    };
    let (mut device, mut percent, mut seconds) = (None, None, None);
    let mut it = argv[1..].iter();
    while let Some(flag) = it.next() {
        let val = it.next().ok_or(format!("{flag} needs a value"))?;
        match flag.as_str() {
            "--device" => device = Some(val.clone()),
            "--percent" if vram => percent = Some(val.parse::<u32>().map_err(|_| "--percent is not a number")?),
            "--seconds" => seconds = Some(val.parse::<u64>().map_err(|_| "--seconds is not a number")?),
            _ => return Err(format!("unknown argument {flag}")),
        }
    }
    let percent = if vram { percent.ok_or("--percent is required")? } else { 0 };
    if vram && !(10..=95).contains(&percent) { return Err("--percent must be 10..95".into()); }
    let seconds = seconds.ok_or("--seconds is required")?;
    if seconds == 0 { return Err("--seconds must be at least 1".into()); }
    Ok(Args { vram, device: device.ok_or("--device is required")?, percent, seconds })
}

fn list() -> i32 {
    let gpus = device::open().and_then(|(_entry, instance)| device::list(&instance));
    match gpus {
        Ok(g) => {
            let arr: Vec<Value> = g.iter().map(|g| json!({
                "pci": g.pci, "name": g.name, "vendor_id": g.vendor_id, "device_id": g.device_id,
                "type": g.kind, "vram_mib": g.vram_mib, "driver": g.driver})).collect();
            println!("{}", Value::Array(arr));
            0
        }
        Err(e) => {
            eprintln!("centurion-gpustress: {e}");
            println!("[]");  // still valid JSON for the GUI; the exit code says it failed
            3
        }
    }
}

fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    if argv.first().map(String::as_str) == Some("list") {
        std::process::exit(list());
    }
    match parse(&argv) {
        Ok(a) => {
            // SAFETY: the handler only stores to an atomic.
            unsafe {
                libc::signal(libc::SIGTERM, on_stop as *const () as libc::sighandler_t);
                libc::signal(libc::SIGINT, on_stop as *const () as libc::sighandler_t);
            }
            std::process::exit(if a.vram { vram::run(&a.device, a.percent, a.seconds) } else { cores::run(&a.device, a.seconds) })
        }
        Err(e) => init_failed(&e),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vk_errors_map_to_exit_codes() {
        assert_eq!(vk_end(vk::Result::ERROR_DEVICE_LOST, 0.0, 0, 0), 2);
        assert_eq!(vk_end(vk::Result::TIMEOUT, 0.0, 0, 0), 2);  // fence timeout = hung GPU
        assert_eq!(vk_end(vk::Result::ERROR_OUT_OF_HOST_MEMORY, 0.0, 0, 0), 3);
    }
}
