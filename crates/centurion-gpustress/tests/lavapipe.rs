// Runs the real binary against Mesa's lavapipe (a CPU Vulkan device, listed with "type":"cpu"); every
// test is skipped with a message when no such device exists. `cargo test` runs the pass tests,
// `cargo test --features fault-inject` the fault-injection ones (the pass tests would see the fault).
use serde_json::Value;
use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};

const BIN: &str = env!("CARGO_BIN_EXE_centurion-gpustress");

fn cpu_device() -> Option<String> {
    let out = Command::new(BIN).arg("list").output().unwrap();
    let list: Vec<Value> = serde_json::from_slice(&out.stdout).expect("`list` prints a JSON array");
    let pci = list.iter().find(|d| d["type"] == "cpu").map(|d| d["pci"].as_str().unwrap().to_string());
    if pci.is_none() { eprintln!("skipped: no CPU Vulkan device (install Mesa lavapipe)"); }
    pci
}

/// stdout lines (parsed), exit code, stderr.
fn run(args: &[&str]) -> (Vec<Value>, i32, String) {
    let out = Command::new(BIN).args(args).output().unwrap();
    let lines = String::from_utf8(out.stdout).unwrap().lines().map(|l| serde_json::from_str(l).expect("one JSON object per line")).collect();
    (lines, out.status.code().unwrap(), String::from_utf8(out.stderr).unwrap())
}

fn end(lines: &[Value]) -> &Value {
    let last = lines.last().unwrap();
    assert_eq!(last["ev"], "end", "the last line is the end line");
    last
}

#[cfg(not(feature = "fault-inject"))]
#[test]
fn vram_passes() {
    let Some(dev) = cpu_device() else { return };
    let (lines, code, _) = run(&["vram", "--device", &dev, "--percent", "10", "--seconds", "3"]);
    assert_eq!(lines[0]["ev"], "start");
    assert!(lines.iter().all(|l| l["ev"] != "error"));
    assert_eq!((end(&lines)["status"].as_str(), code), (Some("pass"), 0));
    assert_eq!(end(&lines)["errors"], 0);
}

#[cfg(feature = "fault-inject")]
#[test]
fn vram_fault_is_found_once() {
    let Some(dev) = cpu_device() else { return };
    // Pass 3 is the first with a fault; a pass takes about a second on lavapipe, so leave room.
    let (lines, code, stderr) = run(&["vram", "--device", &dev, "--percent", "10", "--seconds", "12"]);
    let errors: Vec<&Value> = lines.iter().filter(|l| l["ev"] == "error").collect();
    assert_eq!(errors.len(), 1, "{lines:?}");
    // The binary names the word it corrupted and what it should hold; the shader must agree.
    let injected = stderr.lines().find_map(|l| l.strip_prefix("fault-inject: word ")).expect("fault-inject line on stderr");
    let (word, expected) = injected.split_once(" expected ").unwrap();
    let flipped = u32::from_str_radix(expected.trim_start_matches("0x"), 16).unwrap() ^ 1;
    assert_eq!(errors[0]["word"].as_u64().unwrap().to_string(), word);
    assert_eq!(errors[0]["expected"], expected);
    assert_eq!(errors[0]["actual"].as_str().unwrap(), format!("0x{flipped:08x}"));
    assert_eq!((errors[0]["pass"].as_u64(), errors[0]["pattern"].as_str()), (Some(3), Some("walk0")));
    assert_eq!((end(&lines)["status"].as_str(), code, end(&lines)["errors"].as_u64()), (Some("fail"), 1, Some(1)));
}

#[cfg(not(feature = "fault-inject"))]
#[test]
fn core_passes() {
    let Some(dev) = cpu_device() else { return };
    let (lines, code, _) = run(&["core", "--device", &dev, "--seconds", "3"]);
    assert_eq!(lines[0]["ev"], "start");
    assert!(lines.iter().all(|l| l["ev"] != "error"));
    assert_eq!((end(&lines)["status"].as_str(), code, end(&lines)["errors"].as_u64()), (Some("pass"), 0, Some(0)));
    assert!(end(&lines)["passes"].as_u64().unwrap() > 0, "the seconds count after start-up, so some runs are compared");
}

#[cfg(feature = "fault-inject")]
#[test]
fn core_perturbation_is_detected() {
    let Some(dev) = cpu_device() else { return };
    // The third run has one invocation's input disturbed; the runs are ~100 ms each.
    let (lines, code, stderr) = run(&["core", "--device", &dev, "--seconds", "5"]);
    let errors: Vec<&Value> = lines.iter().filter(|l| l["ev"] == "error").collect();
    assert!(!errors.is_empty(), "{lines:?}");
    // The disturbed invocation has independent lanes: only its first word differs from the reference.
    let word = stderr.lines().find_map(|l| l.strip_prefix("fault-inject: word ")).expect("fault-inject line on stderr");
    assert_eq!(errors.len(), 1, "{lines:?}");
    assert_eq!(errors[0]["word"].as_u64().unwrap().to_string(), word);
    assert_ne!(errors[0]["expected"], errors[0]["actual"]);
    assert_eq!((errors[0]["pass"].as_u64(), errors[0]["pattern"].as_str()), (Some(3), Some("core")));
    assert_eq!((end(&lines)["status"].as_str(), code, end(&lines)["errors"].as_u64()), (Some("fail"), 1, Some(1)));
}

#[test]
fn sigterm_stops_at_a_boundary() {
    let Some(dev) = cpu_device() else { return };
    let mut child = Command::new(BIN).args(["vram", "--device", &dev, "--percent", "10", "--seconds", "60"])
        .stdout(Stdio::piped()).stderr(Stdio::null()).spawn().unwrap();
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
    let start: Value = serde_json::from_str(&lines.next().unwrap().unwrap()).unwrap();
    assert_eq!(start["ev"], "start");  // the handler is installed before `start` is printed
    // SAFETY: plain kill(2) of our own child.
    unsafe { libc::kill(child.id() as i32, libc::SIGTERM) };
    let rest: Vec<Value> = lines.map(|l| serde_json::from_str(&l.unwrap()).unwrap()).collect();
    assert_eq!(end(&rest)["status"], "stopped");
    assert_eq!(child.wait().unwrap().code(), Some(4));
}

#[test]
fn bad_arguments_and_unknown_device_are_init_failed() {
    for args in [&["vram", "--device", "x", "--percent", "5", "--seconds", "3"][..], &["core"], &["core", "--device", "9999:99:99.9", "--seconds", "3"],
        &["vram", "--device", "9999:99:99.9", "--percent", "10", "--seconds", "3"]] {
        let (lines, code, _) = run(args);
        assert_eq!((end(&lines)["status"].as_str(), code), (Some("init_failed"), 3), "{args:?}");
    }
}
