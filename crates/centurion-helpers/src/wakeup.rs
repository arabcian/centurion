//! ACPI wake sources (/proc/acpi/wakeup) for Health → Sleep: which devices may wake the machine from
//! sleep. The kernel forgets the choice at every boot, so the user's choices are kept in
//! /etc/centurion/wakeup.json ({"acpi": {"XHC0": false, …}}) and put back by `tune-helper boot`
//! (centurion-tune service).
//!
//! /proc/acpi/wakeup is a toggle: writing a device name flips it. Every write is therefore preceded by
//! a read of the current state and followed by a read-back.

use serde_json::{json, Map, Value};

pub const PROC: &str = "/proc/acpi/wakeup";
pub const SAVED: &str = "/etc/centurion/wakeup.json";

/// (device, S-state, enabled, sysfs node) per line of /proc/acpi/wakeup.
pub fn parse(text: &str) -> Vec<(String, String, bool, String)> {
    text.lines().skip(1).filter_map(|l| {
        let mut f = l.split_whitespace();
        let dev = f.next()?.to_owned();
        let s = f.next()?.to_owned();
        let st = f.next()?.trim_start_matches('*');
        let node = f.next().unwrap_or("").trim_start_matches("pci:").to_owned();
        Some((dev, s, st == "enabled", node))
    }).collect()
}

fn valid_name(n: &str) -> bool { !n.is_empty() && n.len() <= 8 && n.bytes().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit() || b == b'_') }

fn state_of(dev: &str) -> Option<bool> {
    parse(&std::fs::read_to_string(PROC).ok()?).into_iter().find(|d| d.0 == dev).map(|d| d.2)
}

fn write_toggle(dev: &str, enabled: bool) -> Result<(), String> {
    match state_of(dev) {
        None => return Err(format!("{dev} is not listed in {PROC}")),
        Some(s) if s == enabled => return Ok(()),
        Some(_) => {}
    }
    std::fs::write(PROC, dev).map_err(|e| format!("{PROC}: {e}"))?;
    crate::wlog::log("wakeup", &format!("{PROC}: {dev} → {}", if enabled { "enabled" } else { "disabled" }));
    match state_of(dev) {
        Some(s) if s == enabled => Ok(()),
        _ => Err(format!("{dev}: the kernel did not take the change (some devices are fixed by firmware)")),
    }
}

/// Sets one device and remembers the choice for the next boots.
pub fn set(dev: &str, enabled: bool) -> Value {
    if !valid_name(dev) { return json!({"ok": false, "error": "invalid device name"}); }
    if let Err(e) = write_toggle(dev, enabled) { return json!({"ok": false, "error": e}); }
    let mut saved: Value = crate::read_root_file(SAVED, 64 * 1024).and_then(|s| serde_json::from_str(&s).ok())
        .filter(Value::is_object).unwrap_or_else(|| json!({"acpi": {}}));
    if !saved["acpi"].is_object() { saved["acpi"] = json!({}); }
    saved["acpi"][dev] = json!(enabled);
    let _ = crate::secure_dir("/etc/centurion");
    match crate::write_root_file(SAVED, serde_json::to_string_pretty(&saved).unwrap_or_default().as_bytes()) {
        Ok(()) => json!({"ok": true, "device": dev, "enabled": enabled}),
        Err(e) => json!({"ok": true, "device": dev, "enabled": enabled, "warning": format!("not remembered: {e}")}),
    }
}

/// Puts the saved choices back (boot). Returns the devices that could not be set.
pub fn apply_saved() -> Vec<String> {
    let Some(saved) = crate::read_root_file(SAVED, 64 * 1024).and_then(|s| serde_json::from_str::<Value>(&s).ok()) else { return Vec::new() };
    let empty = Map::new();
    saved["acpi"].as_object().unwrap_or(&empty).iter()
        .filter_map(|(dev, on)| Some((dev, on.as_bool()?)))
        .filter(|(dev, _)| valid_name(dev))
        .filter_map(|(dev, on)| write_toggle(dev, on).err())
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parses_proc_wakeup() {
        let t = "Device\tS-state\t  Status   Sysfs node\nGPP0\t  S4\t*disabled  pci:0000:00:01.1\nXHC0\t  S4\t*enabled   pci:0000:c4:00.3\nLID0\t  S4\t*enabled   platform:PNP0C0D:00\n";
        let d = parse(t);
        assert_eq!(d.len(), 3);
        assert_eq!(d[0], ("GPP0".into(), "S4".into(), false, "0000:00:01.1".into()));
        assert!(d[1].2);
        assert!(valid_name("XHC0") && !valid_name("../x") && !valid_name("TOOLONGNAME"));
    }
}
