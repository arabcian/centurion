//! Health → tool pages that need root: a fixed whitelist of read-only
//! commands with fixed arguments (nothing comes from the request but the
//! tool's name). Binaries must be root-owned in system directories.

use serde_json::{json, Value};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Duration;

const MAX_OUT: u64 = 1 << 20;
const DIRS: &[&str] = &["/usr/sbin", "/usr/bin", "/sbin", "/bin", "/usr/local/sbin", "/usr/local/bin"];

fn bin(name: &str) -> Option<PathBuf> {
    DIRS.iter().map(|d| Path::new(d).join(name)).find(|p| crate::trusted_path(p))
}

/// stdout and stderr of `bin args`, killed after `secs`.
fn capture_split(bin: &Path, args: &[&str], secs: u64) -> (String, String) {
    let child = Command::new(bin).args(args).env_clear().env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin").env("LC_ALL", "C")
        .current_dir("/").stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn();
    let mut child = match child { Ok(c) => c, Err(e) => return (format!("{}: {e}\n", bin.display()), String::new()) };
    let (mut out, mut err) = (child.stdout.take().unwrap(), child.stderr.take().unwrap());
    let (tx, rx) = std::sync::mpsc::channel();
    let tx2 = tx.clone();
    std::thread::spawn(move || { let mut v = Vec::new(); let _ = (&mut out).take(MAX_OUT).read_to_end(&mut v); let _ = tx.send((0, v)); });
    std::thread::spawn(move || { let mut v = Vec::new(); let _ = (&mut err).take(64 * 1024).read_to_end(&mut v); let _ = tx2.send((1, v)); });
    let (mut so, mut se) = (Vec::new(), Vec::new());
    let deadline = std::time::Instant::now() + Duration::from_secs(secs);
    for _ in 0..2 {
        let left = deadline.saturating_duration_since(std::time::Instant::now());
        match rx.recv_timeout(left) {
            Ok((0, v)) => so = v,
            Ok((_, v)) => se = v,
            Err(_) => { crate::kill_reap(&mut child); return (format!("{}: timed out after {secs} s\n", bin.display()), String::new()); }
        }
    }
    let _ = child.wait();
    (String::from_utf8_lossy(&so).into_owned(), String::from_utf8_lossy(&se).into_owned())
}

/// stdout, then stderr, of `bin args`.
fn capture(bin: &Path, args: &[&str], secs: u64) -> String {
    let (mut s, e) = capture_split(bin, args, secs);
    s.push_str(&e);
    s
}

fn section(out: &mut String, cmd: &str, body: &str) {
    out.push_str(&format!("$ {cmd}\n{body}"));
    if !body.ends_with('\n') { out.push('\n'); }
    out.push('\n');
}

/// The package that provides a tool, per distribution family (the names the install hint uses).
struct Pkg { gentoo: &'static str, arch: &'static str, fedora: &'static str, debian: &'static str }
const PKG_DMIDECODE: Pkg = Pkg { gentoo: "sys-apps/dmidecode", arch: "dmidecode", fedora: "dmidecode", debian: "dmidecode" };
const PKG_PCIUTILS: Pkg = Pkg { gentoo: "sys-apps/pciutils", arch: "pciutils", fedora: "pciutils", debian: "pciutils" };
const PKG_UTIL_LINUX: Pkg = Pkg { gentoo: "sys-apps/util-linux", arch: "util-linux", fedora: "util-linux", debian: "util-linux" };
const PKG_SMARTMONTOOLS: Pkg = Pkg { gentoo: "sys-apps/smartmontools", arch: "smartmontools", fedora: "smartmontools", debian: "smartmontools" };
const PKG_NVME_CLI: Pkg = Pkg { gentoo: "sys-apps/nvme-cli", arch: "nvme-cli", fedora: "nvme-cli", debian: "nvme-cli" };

/// "gentoo", "arch", "fedora" (Fedora and the RHEL family), "debian" (Debian and Ubuntu), or "" for any other
/// distribution, from the ID and ID_LIKE lines of /etc/os-release.
fn family(os_release: &str) -> &'static str {
    os_release.lines()
        .filter_map(|l| l.strip_prefix("ID=").or_else(|| l.strip_prefix("ID_LIKE=")))
        .flat_map(|v| v.trim_matches('"').split_whitespace())
        .find_map(|id| match id {
            "gentoo" => Some("gentoo"),
            "arch" => Some("arch"),
            "fedora" | "rhel" | "centos" => Some("fedora"),
            "debian" | "ubuntu" => Some("debian"),
            _ => None,
        })
        .unwrap_or("")
}

/// The command that installs the tool for this family ("emerge sys-apps/dmidecode"), one per package
/// alternative. The tool's name alone when the family is not known.
fn install_hint(family: &str, name: &str, pkgs: &[&Pkg]) -> String {
    let hints: Vec<String> = pkgs.iter().filter_map(|p| match family {
        "gentoo" => Some(format!("emerge {}", p.gentoo)),
        "arch" => Some(format!("pacman -S {}", p.arch)),
        "fedora" => Some(format!("dnf install {}", p.fedora)),
        "debian" => Some(format!("apt install {}", p.debian)),
        _ => None,
    }).collect();
    if hints.is_empty() { name.to_owned() } else { hints.join(" or ") }
}

fn missing(out: &mut String, name: &str, pkgs: &[&Pkg]) {
    let os_release = std::fs::read_to_string("/etc/os-release").unwrap_or_default();
    section(out, name, &format!("(not installed — {})", install_hint(family(&os_release), name, pkgs)));
}

/// Storage devices smartctl / nvme-cli can read (NVMe controllers, SATA disks).
fn disks() -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir("/dev").into_iter().flatten().flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| (n.starts_with("nvme") && n[4..].chars().all(|c| c.is_ascii_digit()) && n.len() > 4)
            || (n.starts_with("sd") && n.len() == 3 && n.as_bytes()[2].is_ascii_lowercase()))
        .map(|n| format!("/dev/{n}")).collect();
    v.sort();
    v
}

/// Stable per disk and free of the serial number: model, capacity, and a 48-bit FNV-1a hash of the serial.
fn disk_id(model: &str, capacity: u64, serial: &str) -> String {
    let h = serial.trim().bytes().fold(0xcbf2_9ce4_8422_2325u64, |h, b| (h ^ b as u64).wrapping_mul(0x0100_0000_01b3));
    format!("{}|{}|{:012x}", model.trim(), capacity, h & 0xffff_ffff_ffff)
}

/// Model, serial and size of an NVMe controller (`nvme0`), read from sysfs (nvme-cli prints none of them).
fn nvme_identity(name: &str) -> (String, String, u64) {
    let ctrl = Path::new("/sys/class/nvme").join(name);
    let rd = |f: &str| std::fs::read_to_string(ctrl.join(f)).map(|s| s.trim().to_string()).unwrap_or_default();
    let prefix = format!("{name}n");
    let cap = std::fs::read_dir(&ctrl).into_iter().flatten().flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .find(|n| n.starts_with(prefix.as_str()))
        .and_then(|ns| std::fs::read_to_string(format!("/sys/block/{ns}/size")).ok())
        .and_then(|s| s.trim().parse::<u64>().ok())
        .map_or(0, |sectors| sectors * 512);
    (rd("model"), rd("serial"), cap)
}

/// NVMe health fields, from smartctl's `nvme_smart_health_information_log` or nvme-cli's flat log.
fn fill_nvme_health(d: &mut Value, n: &Value) {
    let pick = |a: &str, b: &str| n.get(a).or_else(|| n.get(b)).cloned().unwrap_or(Value::Null);
    d["life_used_pct"] = pick("percentage_used", "percent_used");
    d["spare_pct"] = pick("available_spare", "avail_spare");
    d["spare_threshold_pct"] = pick("available_spare_threshold", "spare_thresh");
    d["media_errors"] = n["media_errors"].clone();
    d["unsafe_shutdowns"] = n["unsafe_shutdowns"].clone();
    d["critical_warning"] = n["critical_warning"].clone();
    d["power_on_hours"] = n["power_on_hours"].clone();
    // Data units are 1000 × 512 bytes; written in decimal TB, two decimals.
    d["written_tb"] = n["data_units_written"].as_f64().map_or(Value::Null, |u| json!((u * 512_000.0 / 1e10).round() / 100.0));
}

/// `smartctl -j -a` (SATA and NVMe, smartmontools ≥ 7.0).
fn parse_smartctl(dev: &str, j: &Value) -> Value {
    let model = j["model_name"].as_str().unwrap_or("");
    let cap = j["user_capacity"]["bytes"].as_u64().unwrap_or(0);
    let mut d = json!({
        "dev": dev,
        "model": model,
        "capacity_bytes": cap,
        "id": disk_id(model, cap, j["serial_number"].as_str().unwrap_or("")),
        "passed": j["smart_status"]["passed"].as_bool(),
        "temp_c": j["temperature"]["current"].as_i64(),
    });
    if j["nvme_smart_health_information_log"].is_object() { fill_nvme_health(&mut d, &j["nvme_smart_health_information_log"]); }
    d
}

/// `nvme smart-log -o json`: the log only, with the temperature in kelvin.
fn parse_nvme_cli(dev: &str, j: &Value) -> Value {
    let (model, serial, cap) = nvme_identity(dev.trim_start_matches("/dev/"));
    let id = disk_id(&model, cap, &serial);
    let mut d = json!({
        "dev": dev,
        "model": model,
        "capacity_bytes": cap,
        "id": id,
        "temp_c": j["temperature"].as_i64().map(|k| k - 273),
    });
    fill_nvme_health(&mut d, j);
    d
}

/// Disk health for the SMART page and the daily record. One object per disk; no serial number leaves here.
fn smart_json() -> Value {
    let (sc, nv) = (bin("smartctl"), bin("nvme"));
    if sc.is_none() && nv.is_none() {
        return json!({"ok": true, "disks": [], "missing": "sys-apps/smartmontools (or sys-apps/nvme-cli)"});
    }
    let mut rows = Vec::new();
    for d in disks() {
        let smart = sc.as_ref().and_then(|b| serde_json::from_str::<Value>(&capture_split(b, &["-j", "-a", &d], 30).0).ok());
        if let Some(j) = smart.filter(|j| j["device"].is_object()) { rows.push(parse_smartctl(&d, &j)); continue; }
        let Some(b) = nv.as_ref().filter(|_| d.starts_with("/dev/nvme")) else {
            rows.push(json!({"dev": d, "error": "no JSON from smartctl (smartmontools older than 7.0?)"}));
            continue;
        };
        let out = capture_split(b, &["smart-log", "-o", "json", &d], 30).0;
        rows.push(match serde_json::from_str::<Value>(&out) {
            Ok(j) => parse_nvme_cli(&d, &j),
            Err(_) => json!({"dev": d, "error": "no JSON from nvme smart-log"}),
        });
    }
    json!({"ok": true, "disks": rows})
}

pub fn run(tool: &str) -> Value {
    let mut out = String::new();
    match tool {
        "dmidecode" => match bin("dmidecode") {
            Some(b) => section(&mut out, "dmidecode", &capture(&b, &[], 20)),
            None => missing(&mut out, "dmidecode", &[&PKG_DMIDECODE]),
        },
        "smart" => {
            let (sc, nv) = (bin("smartctl"), bin("nvme"));
            if sc.is_none() && nv.is_none() { missing(&mut out, "smartctl", &[&PKG_SMARTMONTOOLS, &PKG_NVME_CLI]); }
            for d in disks() {
                if let Some(b) = &sc { section(&mut out, &format!("smartctl -a {d}"), &capture(b, &["-a", &d], 30)); }
                else if let Some(b) = nv.as_ref().filter(|_| d.starts_with("/dev/nvme")) {
                    section(&mut out, &format!("nvme smart-log {d}"), &capture(b, &["smart-log", &d], 30));
                }
            }
        }
        "dmesg" => match bin("dmesg") {
            Some(b) => section(&mut out, "dmesg --level=emerg,alert,crit,err,warn", &capture(&b, &["--level=emerg,alert,crit,err,warn"], 20)),
            None => missing(&mut out, "dmesg", &[&PKG_UTIL_LINUX]),
        },
        "pcie" => match bin("lspci") {
            Some(b) => section(&mut out, "lspci -vv", &capture(&b, &["-vv"], 20)),
            None => missing(&mut out, "lspci", &[&PKG_PCIUTILS]),
        },
        "smart_json" => return smart_json(),
        _ => return json!({"ok": false, "error": "unknown tool"}),
    }
    json!({"ok": true, "output": out})
}

#[cfg(test)]
mod tests {
    use super::*;

    const SMARTCTL_NVME: &str = r#"{
      "device": {"name": "/dev/nvme0", "protocol": "NVMe"},
      "model_name": "Samsung SSD 980 PRO 1TB",
      "serial_number": "S4EVNX0T123456",
      "user_capacity": {"blocks": 1953525168, "bytes": 1000204886016},
      "smart_status": {"passed": true},
      "temperature": {"current": 41},
      "nvme_smart_health_information_log": {
        "critical_warning": 0, "temperature": 314, "available_spare": 100,
        "available_spare_threshold": 10, "percentage_used": 3, "data_units_written": 25000000,
        "power_on_hours": 3000, "unsafe_shutdowns": 12, "media_errors": 0}
    }"#;

    const SMARTCTL_SATA: &str = r#"{
      "device": {"name": "/dev/sda", "protocol": "ATA"},
      "model_name": "WDC WD10EZEX-00BN5A0",
      "serial_number": "WD-WCC6Y0123456",
      "user_capacity": {"bytes": 1000204886016},
      "smart_status": {"passed": false},
      "temperature": {"current": 38}
    }"#;

    const NVME_CLI: &str = r#"{
      "critical_warning": 4, "temperature": 310, "avail_spare": 97, "spare_thresh": 10,
      "percent_used": 12, "data_units_written": 2000000, "power_on_hours": 900,
      "unsafe_shutdowns": 3, "media_errors": 7, "num_err_log_entries": 9
    }"#;

    #[test]
    fn smartctl_nvme_reduces_to_health_fields_without_serial() {
        let d = parse_smartctl("/dev/nvme0", &serde_json::from_str(SMARTCTL_NVME).unwrap());
        assert_eq!(d["temp_c"], 41);
        assert_eq!(d["passed"], true);
        assert_eq!(d["life_used_pct"], 3);
        assert_eq!(d["spare_pct"], 100);
        assert_eq!(d["spare_threshold_pct"], 10);
        assert_eq!(d["media_errors"], 0);
        assert_eq!(d["unsafe_shutdowns"], 12);
        assert_eq!(d["critical_warning"], 0);
        assert_eq!(d["written_tb"], 12.8);
        assert!(d["id"].as_str().unwrap().starts_with("Samsung SSD 980 PRO 1TB|1000204886016|"));
        assert!(!d.to_string().contains("S4EVNX0T123456"));
    }

    #[test]
    fn sata_has_status_and_temperature_only() {
        let d = parse_smartctl("/dev/sda", &serde_json::from_str(SMARTCTL_SATA).unwrap());
        assert_eq!(d["passed"], false);
        assert_eq!(d["temp_c"], 38);
        assert!(d.get("media_errors").is_none());
    }

    #[test]
    fn nvme_cli_flat_names_and_kelvin_temperature() {
        let d = parse_nvme_cli("/dev/nvme9", &serde_json::from_str(NVME_CLI).unwrap());
        assert_eq!(d["temp_c"], 37);
        assert_eq!(d["spare_pct"], 97);
        assert_eq!(d["spare_threshold_pct"], 10);
        assert_eq!(d["life_used_pct"], 12);
        assert_eq!(d["media_errors"], 7);
        assert_eq!(d["critical_warning"], 4);
        assert_eq!(d["written_tb"], 1.02);
    }

    #[test]
    fn family_comes_from_id_and_id_like() {
        assert_eq!(family("NAME=\"Gentoo\"\nID=gentoo\n"), "gentoo");
        assert_eq!(family("ID=arch\n"), "arch");
        assert_eq!(family("ID=fedora\n"), "fedora");
        assert_eq!(family("ID=\"rocky\"\nID_LIKE=\"rhel centos fedora\"\n"), "fedora");
        assert_eq!(family("ID=ubuntu\nID_LIKE=debian\n"), "debian");
        assert_eq!(family("ID=linuxmint\nID_LIKE=\"ubuntu debian\"\n"), "debian");
        assert_eq!(family("ID=manjaro\nID_LIKE=arch\n"), "arch");
        assert_eq!(family("ID=alpine\n"), "");
        assert_eq!(family(""), "");
    }

    #[test]
    fn install_hint_names_the_family_command() {
        assert_eq!(install_hint("gentoo", "dmidecode", &[&PKG_DMIDECODE]), "emerge sys-apps/dmidecode");
        assert_eq!(install_hint("arch", "dmidecode", &[&PKG_DMIDECODE]), "pacman -S dmidecode");
        assert_eq!(install_hint("fedora", "lspci", &[&PKG_PCIUTILS]), "dnf install pciutils");
        assert_eq!(install_hint("debian", "smartctl", &[&PKG_SMARTMONTOOLS]), "apt install smartmontools");
        assert_eq!(install_hint("gentoo", "smartctl", &[&PKG_SMARTMONTOOLS, &PKG_NVME_CLI]),
                   "emerge sys-apps/smartmontools or emerge sys-apps/nvme-cli");
    }

    #[test]
    fn install_hint_is_the_tool_name_on_an_unknown_distribution() {
        assert_eq!(install_hint("", "lspci", &[&PKG_PCIUTILS]), "lspci");
        assert_eq!(install_hint("", "smartctl", &[&PKG_SMARTMONTOOLS, &PKG_NVME_CLI]), "smartctl");
    }

    #[test]
    fn disk_id_ignores_whitespace_and_hides_serial() {
        let id = disk_id("  Model X ", 512, "  SERIAL-1 ");
        assert_eq!(id, disk_id("Model X", 512, "SERIAL-1"));
        assert!(id.starts_with("Model X|512|"));
        assert!(!id.contains("SERIAL"));
    }
}
