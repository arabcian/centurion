//! Health → Sleep → "Why doesn't the hardware sleep?": read-only checks of the settings that keep a
//! suspend from reaching the SoC's hardware low-power state. Called by health-reader (op "sleep_diag").
//!
//! Only fixed paths below `root` (production: "/") are read; nothing is written. `analyse` turns the
//! readings into finding codes, and the GUI words them, so every sentence the user reads is translatable.
//! The AMD (amd_pmc) and Intel (pmc_core) counters are shown raw: their formats are not parsed here.

use serde_json::{json, Value};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

/// Most bytes kept of one file (debugfs counters are short; this only guards against surprises).
const CLIP: u64 = 4096;

fn text(p: &Path) -> Option<String> {
    let mut s = String::new();
    fs::File::open(p).ok()?.take(CLIP).read_to_string(&mut s).ok()?;
    let s = s.trim().to_owned();
    (!s.is_empty()).then_some(s)
}

/// Entry names of `dir`, sorted; empty when the directory is missing.
fn entries(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = fs::read_dir(dir).into_iter().flatten().flatten().filter_map(|e| e.file_name().into_string().ok()).collect();
    v.sort_unstable();
    v
}

/// What the machine says about its sleep path right now. Plain sysfs, plus debugfs counters (root).
pub fn readings(root: &Path) -> Value {
    let at = |rel: &str| -> PathBuf { root.join(rel) };

    // NVMe: runtime power management of each controller (its PCI device carries the power/ node).
    let nvme: Vec<Value> = entries(&at("sys/class/nvme"))
        .into_iter()
        .map(|n| {
            let power = at(&format!("sys/class/nvme/{n}/device/power"));
            json!({"name": n, "control": text(&power.join("control")), "status": text(&power.join("runtime_status"))})
        })
        .collect();

    // USB devices (the root hubs "usbN" are not devices of their own) that keep runtime power management on.
    let usb_awake: Vec<String> = entries(&at("sys/bus/usb/devices"))
        .into_iter()
        .filter(|n| !n.starts_with("usb"))
        .filter_map(|n| {
            let dev = at(&format!("sys/bus/usb/devices/{n}"));
            (text(&dev.join("power/control")).as_deref() == Some("on"))
                .then(|| match text(&dev.join("product")) {
                    Some(p) => format!("{n} ({p})"),
                    None => n,
                })
        })
        .collect();

    // The NVIDIA display adapter (vendor 0x10de, class 0x03xxxx) and whether it is powered now.
    let dgpu = entries(&at("sys/bus/pci/devices")).into_iter().find_map(|a| {
        let dev = at(&format!("sys/bus/pci/devices/{a}"));
        let is_nvidia_display = text(&dev.join("vendor")).as_deref() == Some("0x10de") && text(&dev.join("class")).is_some_and(|c| c.starts_with("0x03"));
        is_nvidia_display.then(|| json!({"address": a, "status": text(&dev.join("power/runtime_status"))}))
    });

    let dbg = at("sys/kernel/debug");
    let counters = json!({
        "amd_s0ix": text(&dbg.join("amd_pmc/s0ix_stats")),
        "amd_idlemask": text(&dbg.join("amd_pmc/idlemask")),
        "intel_substates": text(&dbg.join("pmc_core/substate_residencies")),
        "intel_ltr": text(&dbg.join("pmc_core/ltr_show")),
    });
    let seen = counters.as_object().is_some_and(|o| o.values().any(|v| !v.is_null()));

    json!({
        "mem_sleep": text(&at("sys/power/mem_sleep")),
        "aspm_policy": text(&at("sys/module/pcie_aspm/parameters/policy")),
        "apst_max_latency_us": text(&at("sys/module/nvme_core/parameters/default_ps_max_latency_us")),
        "nvme": nvme,
        "usb_awake": usb_awake,
        "dgpu": dgpu,
        "debugfs": counters,
        "counters_seen": seen,
    })
}

/// Finding codes from the readings, most important first. A "warn" is a setting that keeps the
/// platform awake; "info" explains a reading. "nothing_found" only when no warning applies and the
/// counters could be read, so it never claims a clean bill of health without the evidence.
pub fn analyse(r: &Value) -> Vec<Value> {
    let mut out: Vec<Value> = Vec::new();
    if r["aspm_policy"].as_str().is_some_and(|p| p.contains("[performance]")) {
        out.push(finding("aspm_performance", "warn", Value::Null));
    }
    let nvme = r["nvme"].as_array().cloned().unwrap_or_default();
    if !nvme.is_empty() {
        if r["apst_max_latency_us"].as_str() == Some("0") {
            out.push(finding("nvme_apst_off", "warn", Value::Null));
        }
        let on: Vec<String> = nvme.iter().filter(|d| d["control"].as_str() == Some("on")).filter_map(|d| d["name"].as_str().map(str::to_owned)).collect();
        if !on.is_empty() {
            out.push(finding("nvme_pm_on", "warn", json!(on.join(", "))));
        }
    }
    let usb = r["usb_awake"].as_array().cloned().unwrap_or_default();
    if !usb.is_empty() {
        let devices = usb.iter().filter_map(Value::as_str).collect::<Vec<_>>().join(", ");
        out.push(finding("usb_pm_on", "warn", json!({"count": usb.len(), "devices": devices})));
    }
    if let Some(d) = r["dgpu"].as_object() {
        if d.get("status").and_then(Value::as_str) == Some("active") {
            out.push(finding("dgpu_active", "warn", d.get("address").cloned().unwrap_or(Value::Null)));
        }
    }
    if r["mem_sleep"].as_str().is_some_and(|m| m.contains("[deep]")) {
        out.push(finding("deep_sleep", "info", Value::Null));
    }
    let seen = r["counters_seen"].as_bool().unwrap_or(false);
    if !seen {
        out.push(finding("debugfs_unreadable", "info", Value::Null));
    }
    if seen && !out.iter().any(|f| f["severity"] == "warn") {
        out.push(finding("nothing_found", "info", Value::Null));
    }
    out
}

fn finding(code: &str, severity: &str, detail: Value) -> Value {
    json!({"code": code, "severity": severity, "detail": detail})
}

/// health-reader op "sleep_diag": {"ok", "readings", "findings"}. Read-only.
pub fn run(root: &Path) -> Value {
    let r = readings(root);
    let findings = analyse(&r);
    json!({"ok": true, "readings": r, "findings": findings})
}

#[cfg(test)]
mod tests {
    use super::*;

    fn clean() -> Value {
        json!({"mem_sleep": "s2idle [deep]", "aspm_policy": "[default] performance powersave", "apst_max_latency_us": "1000",
               "nvme": [{"name": "nvme0", "control": "auto", "status": "suspended"}], "usb_awake": [], "dgpu": null,
               "debugfs": {}, "counters_seen": true})
    }
    fn codes(v: &[Value]) -> Vec<String> { v.iter().map(|f| f["code"].as_str().unwrap().to_owned()).collect() }

    #[test]
    fn blockers_are_named() {
        let mut r = clean();
        r["mem_sleep"] = json!("[s2idle] deep");
        r["aspm_policy"] = json!("default [performance] powersave");
        r["apst_max_latency_us"] = json!("0");
        r["nvme"] = json!([{"name": "nvme0", "control": "on", "status": "active"}]);
        r["usb_awake"] = json!(["1-3 (USB Receiver)"]);
        r["dgpu"] = json!({"address": "0000:01:00.0", "status": "active"});
        let f = analyse(&r);
        assert_eq!(codes(&f), vec!["aspm_performance", "nvme_apst_off", "nvme_pm_on", "usb_pm_on", "dgpu_active"]);
        assert_eq!(f[2]["detail"], "nvme0");
        assert_eq!(f[3]["detail"]["count"], 1);
        assert_eq!(f[4]["detail"], "0000:01:00.0");
    }

    #[test]
    fn clean_machine_with_counters_says_nothing_found() {
        let f = analyse(&clean());
        assert_eq!(codes(&f), vec!["deep_sleep", "nothing_found"]);
    }

    #[test]
    fn no_nvme_means_no_nvme_findings() {
        let mut r = clean();
        r["nvme"] = json!([]);
        r["apst_max_latency_us"] = json!("0");
        assert!(!codes(&analyse(&r)).iter().any(|c| c.starts_with("nvme")));
    }

    #[test]
    fn missing_counters_are_said_not_claimed_clean() {
        let mut r = clean();
        r["counters_seen"] = json!(false);
        let c = codes(&analyse(&r));
        assert!(c.contains(&"debugfs_unreadable".to_string()));
        assert!(!c.contains(&"nothing_found".to_string()));
    }

    #[test]
    fn readings_come_from_fixed_paths_only() {
        let root = std::env::temp_dir().join(format!("sleepdiag-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let put = |rel: &str, body: &str| {
            let p = root.join(rel);
            fs::create_dir_all(p.parent().unwrap()).unwrap();
            fs::write(p, body).unwrap();
        };
        put("sys/power/mem_sleep", "s2idle [deep]\n");
        put("sys/module/pcie_aspm/parameters/policy", "[performance] powersave\n");
        put("sys/class/nvme/nvme0/device/power/control", "on\n");
        put("sys/bus/usb/devices/usb1/power/control", "on\n");           // root hub: skipped
        put("sys/bus/usb/devices/1-3/power/control", "on\n");
        put("sys/bus/usb/devices/1-3/product", "USB Receiver\n");
        put("sys/bus/usb/devices/2-1/power/control", "auto\n");
        put("sys/bus/pci/devices/0000:00:02.0/vendor", "0x8086\n");     // not NVIDIA
        put("sys/bus/pci/devices/0000:00:02.0/class", "0x030000\n");
        put("sys/bus/pci/devices/0000:01:00.0/vendor", "0x10de\n");
        put("sys/bus/pci/devices/0000:01:00.0/class", "0x030200\n");
        put("sys/bus/pci/devices/0000:01:00.0/power/runtime_status", "suspended\n");
        put("sys/kernel/debug/amd_pmc/s0ix_stats", "Last S0ix Status: 0\n");

        let r = readings(&root);
        assert_eq!(r["mem_sleep"], "s2idle [deep]");
        assert_eq!(r["aspm_policy"], "[performance] powersave");
        assert_eq!(r["apst_max_latency_us"], Value::Null);
        assert_eq!(r["nvme"][0]["control"], "on");
        assert_eq!(r["usb_awake"], json!(["1-3 (USB Receiver)"]));
        assert_eq!(r["dgpu"]["address"], "0000:01:00.0");
        assert_eq!(r["dgpu"]["status"], "suspended");
        assert_eq!(r["debugfs"]["amd_s0ix"], "Last S0ix Status: 0");
        assert_eq!(r["counters_seen"], true);
        let _ = fs::remove_dir_all(&root);
    }
}
