//! Virtual hardware test lab: the helpers' reading and fault-handling paths run against fake
//! hardware — a sysfs tree in a temp directory, scripted kernel-log fault sequences, a fake SMU
//! mailbox — so the cases a developer's own machine never shows (no sensors, a wrapped energy
//! counter, a suspended dGPU, a GPU falling off the bus, an SMU that stops answering) are tested.
//! WMI / EC calls are answered by a fake acpi_call (legion_wmi::set_acpi_fake).

use centurion_helpers::flight::{self, Recorder, Sensors};
use centurion_helpers::smu::{mailbox, Mbox, Smn, SmuError};
use centurion_helpers::{boothist, health, legion_wmi};
use serde_json::{json, Value};
use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Duration;

/// A fresh fake root per test.
fn lab(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("centurion-lab-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}
fn put(root: &Path, rel: &str, body: &str) {
    let p = root.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, body).unwrap();
}

/// An AMD laptop with an NVIDIA dGPU, as sysfs shows it.
fn amd_laptop(name: &str) -> PathBuf {
    let r = lab(name);
    put(&r, "sys/class/hwmon/hwmon0/name", "k10temp\n");
    put(&r, "sys/class/hwmon/hwmon0/temp1_input", "71500\n");
    put(&r, "sys/class/hwmon/hwmon1/name", "legion_hwmon\n");
    put(&r, "sys/class/hwmon/hwmon1/fan1_input", "2400\n");
    put(&r, "sys/class/hwmon/hwmon1/fan2_input", "2600\n");
    put(&r, "sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "3200000\n");
    put(&r, "sys/devices/system/cpu/cpu1/cpufreq/scaling_cur_freq", "4100000\n");
    put(&r, "sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", "9900000\n");  // not a cpuN dir
    put(&r, "sys/class/powercap/intel-rapl:0/energy_uj", "1000000\n");
    put(&r, "sys/class/powercap/intel-rapl:0/max_energy_range_uj", "2000000\n");
    put(&r, "sys/class/power_supply/AC/online", "0\n");
    put(&r, "sys/class/power_supply/BAT0/power_now", "25300000\n");
    put(&r, "sys/bus/pci/devices/0000:00:08.1/vendor", "0x1002\n");
    put(&r, "sys/bus/pci/devices/0000:00:08.1/class", "0x030000\n");
    put(&r, "sys/bus/pci/devices/0000:01:00.0/vendor", "0x10de\n");
    put(&r, "sys/bus/pci/devices/0000:01:00.0/class", "0x030000\n");
    put(&r, "sys/bus/pci/devices/0000:01:00.0/power/runtime_status", "active\n");
    put(&r, "proc/loadavg", "1.50 1.20 0.90 2/900 4242\n");
    r
}

#[test]
fn sensors_on_a_fake_amd_laptop() {
    let r = amd_laptop("amd");
    let mut s = Sensors::discover(&r);
    let m = s.read(&mut || Some(65));
    assert_eq!(m["cpu_c"], json!(72.0));
    assert_eq!(m["gpu_c"], json!(65));
    assert_eq!(m["mhz"], json!(4100.0), "highest cpuN clock, policy dirs ignored");
    assert_eq!(m["bat_w"], json!(25.3));
    assert_eq!(m["fan"], json!([2400, 2600]));
    assert_eq!(m["dgpu"], json!("active"));
    assert_eq!(m["load"], json!(1.5));
    assert!(!m.contains_key("pkg_w"), "power needs two energy readings");

    // The energy counter wraps between two readings; the dGPU goes to sleep.
    std::thread::sleep(Duration::from_millis(600));
    put(&r, "sys/class/powercap/intel-rapl:0/energy_uj", "200000\n");
    put(&r, "sys/bus/pci/devices/0000:01:00.0/power/runtime_status", "suspended\n");
    let m = s.read(&mut || panic!("the EC must not be asked while the dGPU sleeps"));
    let w = m["pkg_w"].as_f64().unwrap();
    assert!((1.0..=2.1).contains(&w), "1.2 J over ~0.6 s after a wrap, got {w}");
    assert!(!m.contains_key("gpu_c"));
    assert_eq!(m["dgpu"], json!("suspended"));
}

#[test]
fn sensors_on_a_machine_without_any() {
    let r = lab("empty");
    put(&r, "sys/class/hwmon/hwmon0/name", "acpitz\n");        // no CPU sensor, no fans
    put(&r, "sys/class/power_supply/BAT1/current_now", "1000000\n");
    put(&r, "sys/class/power_supply/BAT1/voltage_now", "12000000\n");
    let m = Sensors::discover(&r).read(&mut || panic!("no dGPU: no EC read"));
    assert_eq!(Value::Object(m), json!({"bat_w": 12.0}), "current x voltage when power_now is missing");
    assert!(Sensors::discover(&lab("nothing")).read(&mut || None).is_empty());
}

#[test]
fn ring_wraps_continues_and_rotates() {
    let r = lab("ring");
    let (ring, prev) = (r.join("flight.ring"), r.join("flight-prev.ring"));
    let read = |p: &Path| flight::parse(&String::from_utf8_lossy(&std::fs::read(p).unwrap()));
    let mut rec = Recorder::open_at(&ring, &prev, "boot-a", Sensors::discover(&r), false, flight::INTERVAL_S).unwrap();
    for _ in 0..730 { rec.sample(); }
    let size = std::fs::metadata(&ring).unwrap().len();
    let v = read(&ring);
    let s = v["samples"].as_array().unwrap();
    assert_eq!((s.len(), s[0]["s"].as_u64(), s.last().unwrap()["s"].as_u64()), (720, Some(10), Some(729)), "one hour kept");
    assert_eq!(v["interval"], json!(flight::INTERVAL_S));

    // Service restarted in the same boot: continues, never overwrites the newest sample.
    drop(rec);
    let mut rec = Recorder::open_at(&ring, &prev, "boot-a", Sensors::discover(&r), false, flight::INTERVAL_S).unwrap();
    rec.sample();
    assert_eq!(read(&ring)["samples"].as_array().unwrap().last().unwrap()["s"], json!(730));
    assert_eq!(std::fs::metadata(&ring).unwrap().len(), size, "the file never grows");

    // Next boot: the old ring becomes the previous boot's record.
    drop(rec);
    Recorder::open_at(&ring, &prev, "boot-b", Sensors::discover(&r), false, flight::INTERVAL_S).unwrap();
    assert_eq!(read(&prev)["boot"], json!("boot-a"));
    assert_eq!(read(&ring)["boot"], json!("boot-b"));
    assert!(read(&ring)["samples"].as_array().unwrap().is_empty());
}

#[test]
fn interval_setting_only_takes_the_offered_choices() {
    assert_eq!(flight::parse_interval(None), flight::INTERVAL_S);
    assert_eq!(flight::parse_interval(Some("30\n")), 30);
    for bad in ["0", "1", "7", "-5", "abc", "", "3600"] { assert_eq!(flight::parse_interval(Some(bad)), flight::INTERVAL_S, "{bad}"); }
    assert_eq!(flight::interval_op(Some(7))["ok"], json!(false));
}

#[test]
fn ring_parse_survives_damage() {
    let v = flight::parse("\0\0\0\n{bad\n{\"boot\":\"x\",\"interval\":5}   \n{\"s\":2,\"t\":1}\n{\"s\":1,\"t\":0}\n\0\0");
    assert_eq!(v["samples"], json!([{"s": 1, "t": 0}, {"s": 2, "t": 1}]));
    assert!(flight::parse("").is_null());
    assert!(flight::parse("{\"s\":1}").is_null(), "no header: not a ring");
}

#[test]
fn flight_samples_go_to_their_boot() {
    let ex = json!({"boots": [{"boot": "b1", "start": 0, "end": 100, "clean": false, "segs": []}, {"boot": "b2", "start": 200, "segs": []}]});
    let mut out = boothist::build(&ex, "", "b2", 300, &json!({}));
    boothist::attach_flight(&mut out, &json!({"boot": "b1", "samples": [{"s": 0, "t": 90, "cpu_c": 95}]}));
    let boots = out["boots"].as_array().unwrap();
    assert_eq!(boots[1]["flight"][0]["cpu_c"], json!(95));
    assert!(boots[0].get("flight").is_none());
    boothist::attach_flight(&mut out, &Value::Null);   // no recording: nothing changes
}

/// A scripted crash: the NVIDIA GPU falls off the bus amid noise and a corrected-MCE detail storm.
#[test]
fn gpu_off_the_bus_scenario() {
    let log = [
        "usb 1-1: new high-speed USB device number 2 using xhci_hcd",
        "[Hardware Error]: Corrected error, no action required.",
        "[Hardware Error]: Error Addr: 0x00000000a1b2c3c0",
        "[Hardware Error]: IPID: 0x0001000100000000, Syndrome: 0x000000005a020001",
        "[Hardware Error]: cache level: L1, tx: INSN",
        "NVRM: _kgspIsHeartbeatTimedOut: Heartbeat timed out",
        "NVRM: Xid (PCI:0000:01:00): 79, pid='<unknown>', name=<unknown>, GPU has fallen off the bus.",
        "NVRM: Xid (PCI:0000:01:00): 154, GPU recovery action changed from 0x0 (None) to 0x2 (Node Reboot Required)",
        "nvidia 0000:01:00.0: Unable to change power state from D3cold to D0, device inaccessible",
    ];
    let ev: Vec<(String, String)> = log.iter().filter_map(|l| health::classify(l))
        .map(|e| (e["kind"].as_str().unwrap().to_owned(), e["level"].as_str().unwrap().to_owned())).collect();
    let pairs: Vec<(&str, &str)> = ev.iter().map(|(k, l)| (k.as_str(), l.as_str())).collect();
    assert_eq!(pairs, [("mce", "warn"), ("gsp", "critical"), ("xid", "critical"), ("xid", "critical"), ("d3cold", "critical")],
               "one corrected MCE (detail lines not counted), the GPU chain in order");
}

/// Fake SMN bus: `fail_rd` = every read errors, `answer` = what the response register shows.
struct FakeSmu { fail_rd: bool, answer: u32 }
impl Smn for FakeSmu {
    fn rd(&mut self, _: u32) -> io::Result<u32> {
        if self.fail_rd { Err(io::Error::from_raw_os_error(libc::EIO)) } else { Ok(self.answer) }
    }
    fn wr(&mut self, _: u32, _: u32) -> io::Result<()> { Ok(()) }
}
const MB: Mbox = Mbox { msg: 0x10, rsp: 0x14, arg: 0x18 };

#[test]
fn smu_fault_scenarios() {
    let t = Duration::from_millis(5);
    let bus_error = mailbox(&mut FakeSmu { fail_rd: true, answer: 1 }, MB, 0x19, 0, t);
    assert!(matches!(&bus_error, Err(e) if e.is_fatal() && e.message().contains("SMN access")), "{bus_error:?}");
    let silent = mailbox(&mut FakeSmu { fail_rd: false, answer: 0 }, MB, 0x19, 0, t);
    assert!(matches!(&silent, Err(SmuError::Fatal(m)) if m.contains("did not answer")), "{silent:?}");
    assert_eq!(mailbox(&mut FakeSmu { fail_rd: false, answer: 0xFE }, MB, 0x19, 0, t).unwrap(), 0xFE, "a rejection is an answer");
    assert_eq!(mailbox(&mut FakeSmu { fail_rd: false, answer: 1 }, MB, 0x19, 0, t).unwrap(), 1);
}

/// Fake acpi_call: the EC's answer is picked by EC_MODE, every call is counted and checked.
static EC_MODE: AtomicUsize = AtomicUsize::new(0);
static EC_CALLS: AtomicUsize = AtomicUsize::new(0);
fn fake_ec(expr: &str) -> String {
    EC_CALLS.fetch_add(1, Ordering::SeqCst);
    assert!(expr.ends_with(".WMAE 0x0 0x11 {0x00, 0x00, 0x05, 0x05}"), "only the GPU temperature read is expected: {expr}");
    match EC_MODE.load(Ordering::SeqCst) {
        0 => "0x41\0\0".into(),               // 65 °C, NUL-padded like the module's buffer
        1 => "0x0".into(),                      // GPU powered off
        2 => "0xc8".into(),                     // 200 °C: implausible
        _ => "Error: AE_NOT_FOUND".into(),      // firmware without the method / EC busy
    }
}

/// One test for everything behind the fake: it replaces acpi_call for the whole process.
#[test]
fn wmi_ec_scenarios() {
    legion_wmi::set_acpi_fake(Some(fake_ec));
    let mode = |m: usize| EC_MODE.store(m, Ordering::SeqCst);
    mode(0); assert_eq!(legion_wmi::ec_gpu_temp(), Ok(Some(65)));
    mode(1); assert_eq!(legion_wmi::ec_gpu_temp(), Ok(None));
    mode(2); assert_eq!(legion_wmi::ec_gpu_temp(), Ok(None));
    mode(3); assert!(legion_wmi::ec_gpu_temp().is_err_and(|e| e.contains("AE_NOT_FOUND")));

    // The recorder asks the EC while the dGPU is awake, and gives up after 5 failures in a row.
    let r = amd_laptop("wmi");
    let (ring, prev) = (r.join("flight.ring"), r.join("flight-prev.ring"));
    let mut rec = Recorder::open_at(&ring, &prev, "boot-w", Sensors::discover(&r), true, flight::INTERVAL_S).unwrap();
    mode(0); rec.sample();
    mode(3); for _ in 0..4 { rec.sample(); }
    mode(0); rec.sample();                       // a good reading resets the count
    mode(3); for _ in 0..5 { rec.sample(); }
    let calls = EC_CALLS.load(Ordering::SeqCst);
    mode(0); for _ in 0..3 { rec.sample(); }
    assert_eq!(EC_CALLS.load(Ordering::SeqCst), calls, "given up: the EC is not asked again this boot");
    let v = flight::parse(&String::from_utf8_lossy(&std::fs::read(&ring).unwrap()));
    let gpu: Vec<Value> = v["samples"].as_array().unwrap().iter().map(|x| x["gpu_c"].clone()).collect();
    assert_eq!(gpu[0], json!(65));
    assert_eq!(gpu[5], json!(65));
    assert!(gpu[1..5].iter().chain(&gpu[6..]).all(Value::is_null));
    legion_wmi::set_acpi_fake(None);
}
