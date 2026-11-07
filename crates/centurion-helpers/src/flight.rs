//! Hardware flight recorder: the last hour of cheap sensor readings, kept so that after a crash,
//! hang or power cut Health → Boot history can show what the machine was doing just before.
//!
//! centurion-boot-guard `watch` (root, up for the whole boot) takes one sample every `interval()`
//! seconds (Health → Boot history; INTERVAL_S unless /etc/centurion/flight-interval says otherwise).
//! Only readings that cost nothing and never wake a sleeping device: hwmon CPU temperature and
//! fans, RAPL package energy, cpufreq, battery power, load average, the dGPU's runtime-PM state
//! and — only while the dGPU is already awake — the EC's own GPU temperature (Legion WMI; never
//! NVML or nvidia-smi).
//!
//! /var/lib/centurion/flight.ring (root-owned, world-readable): fixed SLOT-byte text lines, slot 0
//! the header {"boot", "interval"}, sample n in slot 1 + n % SLOTS. Each sample is written in
//! place and fdatasync'ed, so the file never grows and a hard hang loses at most one interval.
//! When a new boot starts recording, the previous boot's ring becomes flight-prev.ring.

use serde_json::{json, Map, Value};
use std::fs::File;
use std::os::unix::fs::{FileExt, OpenOptionsExt};
use std::path::{Path, PathBuf};

pub const RING: &str = "/var/lib/centurion/flight.ring";
pub const PREV: &str = "/var/lib/centurion/flight-prev.ring";
pub const INTERVAL_S: u64 = 5;
/// The intervals Health offers. Shorter = less lost at a hang, more disk flushes (one per sample).
pub const CHOICES: [u64; 4] = [5, 10, 30, 60];
/// Root-owned, world-readable: the chosen interval in seconds; absent = INTERVAL_S.
pub const CONF: &str = "/etc/centurion/flight-interval";
const SLOTS: u64 = 720;   // 1 h at INTERVAL_S, 12 h at 60 s
const SLOT: usize = 192;
const RING_BYTES: u64 = (SLOTS + 1) * SLOT as u64;

fn rd(p: &Path) -> Option<String> { std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned()) }
fn num(p: &Path) -> Option<f64> { rd(p)?.parse().ok() }
fn dirs(p: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(p).into_iter().flatten().flatten().map(|e| e.path()).collect();
    v.sort();
    v
}

/// Sensor paths, found once (under `root`: "/" on a real machine, a fake tree in tests).
#[derive(Default, Debug)]
pub struct Sensors {
    cpu_temp: Option<PathBuf>,
    fans: Vec<PathBuf>,
    freqs: Vec<PathBuf>,
    rapl: Option<(PathBuf, f64)>,   // energy_uj, max_energy_range_uj
    battery: Option<PathBuf>,
    dgpu: Option<PathBuf>,          // power/runtime_status
    loadavg: PathBuf,
    last_energy: Option<(f64, std::time::Instant)>,
}

impl Sensors {
    pub fn discover(root: &Path) -> Sensors {
        let mut s = Sensors { loadavg: root.join("proc/loadavg"), ..Default::default() };
        for h in dirs(&root.join("sys/class/hwmon")) {
            if s.cpu_temp.is_none() && rd(&h.join("name")).is_some_and(|n| matches!(n.as_str(), "k10temp" | "zenpower" | "coretemp")) {
                s.cpu_temp = Some(h.join("temp1_input")).filter(|p| p.is_file());
            }
            s.fans.extend(dirs(&h).into_iter().filter(|p| {
                p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with("fan") && n.ends_with("_input"))
            }));
        }
        for c in dirs(&root.join("sys/devices/system/cpu")) {
            let n = c.file_name().and_then(|n| n.to_str()).unwrap_or("");
            let f = c.join("cpufreq/scaling_cur_freq");
            if n.strip_prefix("cpu").is_some_and(|d| !d.is_empty() && d.bytes().all(|b| b.is_ascii_digit())) && f.is_file() { s.freqs.push(f); }
        }
        let r = root.join("sys/class/powercap/intel-rapl:0");
        if let Some(max) = num(&r.join("max_energy_range_uj")).filter(|_| r.join("energy_uj").is_file()) { s.rapl = Some((r.join("energy_uj"), max)); }
        s.battery = dirs(&root.join("sys/class/power_supply")).into_iter()
            .find(|p| p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with("BAT")));
        s.dgpu = dirs(&root.join("sys/bus/pci/devices")).into_iter().find(|d| {
            rd(&d.join("vendor")).as_deref() == Some("0x10de") && rd(&d.join("class")).is_some_and(|c| c.starts_with("0x03"))
        }).map(|d| d.join("power/runtime_status"));
        s
    }

    /// One reading; missing sensors are left out. `ec_gpu` is asked only while the dGPU is awake.
    pub fn read(&mut self, ec_gpu: &mut dyn FnMut() -> Option<i64>) -> Map<String, Value> {
        let mut m = Map::new();
        if let Some(t) = self.cpu_temp.as_deref().and_then(num) { m.insert("cpu_c".into(), json!((t / 1000.0).round())); }
        let dgpu = self.dgpu.as_deref().and_then(rd);
        if let Some(d) = &dgpu { m.insert("dgpu".into(), json!(d)); }
        if dgpu.as_deref() == Some("active") { if let Some(t) = ec_gpu() { m.insert("gpu_c".into(), json!(t)); } }
        if let Some(f) = self.freqs.iter().filter_map(|p| num(p)).reduce(f64::max) { m.insert("mhz".into(), json!((f / 1000.0).round())); }
        if let Some((p, max)) = &self.rapl {
            if let Some(e) = num(p) {
                let now = std::time::Instant::now();
                if let Some((e0, t0)) = self.last_energy {
                    let de = if e >= e0 { e - e0 } else { e + max - e0 };   // counter wrapped
                    let dt = now.duration_since(t0).as_secs_f64();
                    if dt > 0.5 { m.insert("pkg_w".into(), json!((de / 1e6 / dt * 10.0).round() / 10.0)); }
                }
                self.last_energy = Some((e, now));
            }
        }
        if let Some(b) = &self.battery {
            let w = num(&b.join("power_now")).map(|p| p / 1e6)
                .or_else(|| Some(num(&b.join("current_now"))? * num(&b.join("voltage_now"))? / 1e12));
            if let Some(w) = w { m.insert("bat_w".into(), json!((w * 10.0).round() / 10.0)); }
        }
        let fans: Vec<i64> = self.fans.iter().filter_map(|p| num(p)).map(|r| r as i64).collect();
        if !fans.is_empty() { m.insert("fan".into(), json!(fans)); }
        if let Some(l) = rd(&self.loadavg).and_then(|s| s.split_whitespace().next()?.parse::<f64>().ok()) { m.insert("load".into(), json!(l)); }
        m
    }
}

/// One fixed-width line: the JSON, space padding, '\n'. Fans are dropped if it would not fit.
fn slot_line(mut v: Map<String, Value>) -> Vec<u8> {
    let mut s = Value::Object(v.clone()).to_string();
    if s.len() >= SLOT { v.remove("fan"); s = Value::Object(v).to_string(); }
    let mut b = s.into_bytes();
    b.truncate(SLOT - 1);
    b.resize(SLOT - 1, b' ');
    b.push(b'\n');
    b
}

/// {"boot", "interval", "samples": [oldest first]} from a ring file's text (Null: no header).
/// Unwritten slots (NUL bytes) and damaged lines are skipped.
pub fn parse(text: &str) -> Value {
    let mut head = Value::Null;
    let mut samples: Vec<Value> = Vec::new();
    for l in text.split('\n') {
        let Ok(v) = serde_json::from_str::<Value>(l.trim_matches(|c| c == ' ' || c == '\0')) else { continue };
        if v["boot"].is_string() { head = v; } else if v["s"].is_u64() { samples.push(v); }
    }
    if head.is_null() { return Value::Null; }
    samples.sort_by_key(|v| v["s"].as_u64());
    head["samples"] = json!(samples);
    head
}

/// `ec`: consecutive EC read failures; None = given up for this boot (no Legion WMI / acpi_call).
pub struct Recorder { f: File, seq: u64, sensors: Sensors, ec: Option<u32> }

impl Recorder {
    /// Production: the root-owned directory, this boot, the real sensors.
    pub fn open() -> Option<Recorder> {
        crate::secure_dir(crate::defaults::DIR).ok()?;
        let boot = crate::bootguard::boot_id();
        if boot.is_empty() { return None; }
        Recorder::open_at(Path::new(RING), Path::new(PREV), &boot, Sensors::discover(Path::new("/")), true, interval()).ok()
    }

    /// The ring of another boot is moved to `prev`; this boot's (service restarted) is continued.
    pub fn open_at(ring: &Path, prev: &Path, boot: &str, sensors: Sensors, ec: bool, interval_s: u64) -> Result<Recorder, String> {
        let mut seq = 0;
        if let Ok(old) = std::fs::read(ring) {
            let old = parse(&String::from_utf8_lossy(&old[..old.len().min(RING_BYTES as usize)]));
            if old["boot"].as_str() == Some(boot) {
                seq = old["samples"].as_array().and_then(|a| a.last()).and_then(|v| v["s"].as_u64()).map_or(0, |s| s + 1);
            } else {
                std::fs::rename(ring, prev).map_err(|e| format!("{}: {e}", prev.display()))?;
            }
        }
        let f = std::fs::OpenOptions::new().create(true).write(true).mode(0o644)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(ring).map_err(|e| format!("{}: {e}", ring.display()))?;
        if seq == 0 {
            let mut h = Map::new();
            h.insert("boot".into(), json!(boot));
            h.insert("interval".into(), json!(interval_s));   // at the start; may change during the boot
            f.set_len(0).and_then(|_| f.write_all_at(&slot_line(h), 0)).map_err(|e| e.to_string())?;
        }
        Ok(Recorder { f, seq, sensors, ec: ec.then_some(0) })
    }

    /// One sample into its slot, then fdatasync (it has to survive the hang it is there to explain).
    pub fn sample(&mut self) {
        let ec = &mut self.ec;
        let mut ec_gpu = || {
            let n = (*ec)?;
            match crate::legion_wmi::ec_gpu_temp() {
                Ok(t) => { *ec = Some(0); t }
                Err(_) => { *ec = (n < 4).then_some(n + 1); None }   // 5 in a row: this machine cannot
            }
        };
        let mut m = self.sensors.read(&mut ec_gpu);
        let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        m.insert("s".into(), json!(self.seq));
        m.insert("t".into(), json!(t));
        let off = (1 + self.seq % SLOTS) * SLOT as u64;
        if self.f.write_all_at(&slot_line(m), off).is_ok() { let _ = self.f.sync_data(); }
        self.seq += 1;
    }
}

/// The configured interval (one of CHOICES; anything else in the file = INTERVAL_S).
pub fn interval() -> u64 { parse_interval(crate::read_root_file(CONF, 64).as_deref()) }

pub fn parse_interval(text: Option<&str>) -> u64 {
    text.and_then(|t| t.trim().parse().ok()).filter(|s| CHOICES.contains(s)).unwrap_or(INTERVAL_S)
}

/// tune-helper op `flight_interval`: {"seconds": n} sets it (n one of CHOICES; the default removes
/// the file), without "seconds" just reports. The running recorder picks it up at its next sample.
pub fn interval_op(seconds: Option<u64>) -> Value {
    let Some(n) = seconds else { return json!({"ok": true, "seconds": interval(), "choices": CHOICES}) };
    if !CHOICES.contains(&n) { return json!({"ok": false, "error": format!("seconds must be one of {CHOICES:?}")}); }
    let r = if n == INTERVAL_S {
        match std::fs::remove_file(CONF) { Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e.to_string()), _ => Ok(()) }
    } else {
        crate::secure_dir("/etc/centurion").and_then(|_| crate::write_root_file(CONF, format!("{n}\n").as_bytes()))
    };
    match r { Ok(()) => json!({"ok": true, "seconds": n}), Err(e) => json!({"ok": false, "error": format!("{CONF}: {e}")}) }
}

/// The previous boot's recording (Null: none), its last `keep` samples.
pub fn load_prev(keep: usize) -> Value {
    let Some(text) = crate::read_root_file(PREV, RING_BYTES) else { return Value::Null };
    let mut v = parse(&text);
    if let Some(a) = v["samples"].as_array_mut() { let drop = a.len().saturating_sub(keep); a.drain(..drop); }
    v
}
