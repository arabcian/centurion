//! H06: on-demand bounded, read-only NVMe SMART/Error Information Log.
//! Never accepts user-supplied device paths or executes a shell. Does not
//! perform self-tests, format, sanitize, firmware updates or feature writes.
use serde_json::{json, Map, Value};
use std::io::Read;
use std::sync::mpsc;
use std::os::unix::fs::FileTypeExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

const TIMEOUT: Duration = Duration::from_secs(4);
const MAX_OUTPUT: u64 = 48 * 1024;
const MAX_CONTROLLERS: usize = 8;

fn rd(path: &Path) -> String {
    std::fs::read_to_string(path).unwrap_or_default().trim().chars().take(80).collect()
}
fn nvme_binary() -> Option<&'static str> {
    ["/usr/sbin/nvme", "/usr/bin/nvme", "/sbin/nvme", "/bin/nvme"]
        .into_iter().find(|p| Path::new(p).is_file())
}
fn controller(name: &str) -> bool {
    let Some(num) = name.strip_prefix("nvme") else { return false };
    !num.is_empty() && num.len() <= 3 && num.bytes().all(|b| b.is_ascii_digit())
}

fn query(bin: &str, operation: &str, device: &str) -> Result<Value, &'static str> {
    let mut child = Command::new(bin)
        .args(match operation {
            "smart-log" => vec!["smart-log", device, "-o", "json"],
            "error-log" => vec!["error-log", device, "-e", "16", "-o", "json"],
            _ => return Err("unsupported operation"),
        })
        .stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::null())
        .spawn().map_err(|_| "nvme-cli could not start")?;
    // Drain stdout concurrently with wait: nvme-cli JSON can exceed the OS
    // pipe capacity, so waiting first can deadlock a successful command.
    let stdout = child.stdout.take().ok_or("NVMe stdout unavailable")?;
    let (sender, receiver) = mpsc::sync_channel(1);
    std::thread::spawn(move || {
        let mut bytes = Vec::new();
        let result = stdout.take(MAX_OUTPUT + 1).read_to_end(&mut bytes)
            .map(|_| bytes).map_err(|_| "NVMe output read error");
        let _ = sender.send(result);
    });
    let start = Instant::now();
    let outcome = loop {
        match child.try_wait() {
            Ok(Some(s)) => break Ok(s),
            Ok(None) if start.elapsed() < TIMEOUT => std::thread::sleep(Duration::from_millis(40)),
            Ok(None) => break Err("nvme-cli timed out (killed)"),
            Err(_) => break Err("nvme-cli wait failed"),
        }
    };
    if let Err(e) = outcome { let _ = child.kill(); let _ = child.wait(); return Err(e); }
    if !outcome.unwrap().success() { return Err("NVMe log not accessible or command failed"); }
    let raw = receiver.recv_timeout(Duration::from_millis(300))
        .map_err(|_| "NVMe stdout collection timed out")??;
    if raw.len() as u64 > MAX_OUTPUT { return Err("NVMe log exceeds bounded output size"); }
    serde_json::from_slice(&raw).map_err(|_| "NVMe CLI returned invalid JSON")
}

/// A number from nvme-cli JSON: 128-bit counters can come as strings.
fn num(x: &Value) -> Option<f64> {
    x.as_f64().or_else(|| x.as_str().and_then(|s| {
        let s = s.trim();
        s.strip_prefix("0x").map_or_else(|| s.replace(',', "").parse().ok(), |h| u64::from_str_radix(h, 16).ok().map(|v| v as f64))
    }))
}

/// The SMART / Health log under fixed names, whichever names the tool used (nvme-cli: avail_spare,
/// percent_used, kelvin; smartctl: available_spare, percentage_used). Only counters, no identifiers.
fn smart_public(v: &Value) -> Value {
    let pick = |names: &[&str]| names.iter().find_map(|n| v.get(*n).and_then(num));
    let mut out = Map::new();
    let mut put = |k: &str, x: Option<f64>| { if let Some(x) = x { out.insert(k.to_owned(), json!(x)); } };
    put("critical_warning", pick(&["critical_warning"]));
    // nvme-cli reports the composite temperature in kelvin; a value that already looks like Celsius is kept.
    put("temp_c", pick(&["temperature"]).map(|t| if t > 200.0 { t - 273.0 } else { t }).filter(|t| (-40.0..150.0).contains(t)));
    put("spare_pct", pick(&["avail_spare", "available_spare"]));
    put("spare_threshold_pct", pick(&["spare_thresh", "available_spare_threshold"]));
    put("life_used_pct", pick(&["percent_used", "percentage_used"]));
    put("media_errors", pick(&["media_errors"]));
    put("error_log_entries", pick(&["num_err_log_entries", "num_err_log_entries_count"]));
    put("unsafe_shutdowns", pick(&["unsafe_shutdowns"]));
    put("power_on_hours", pick(&["power_on_hours"]));
    put("power_cycles", pick(&["power_cycles"]));
    put("warning_temp_minutes", pick(&["warning_temp_time"]));
    put("critical_temp_minutes", pick(&["critical_comp_time"]));
    put("throttle_light", pick(&["thm_temp1_trans_count", "thermal_mgmt_temp1_transition_count"]));
    put("throttle_heavy", pick(&["thm_temp2_trans_count", "thermal_mgmt_temp2_transition_count"]));
    // Data units are 1000 x 512 bytes.
    put("written_tb", pick(&["data_units_written"]).map(|u| (u * 512_000.0 / 1e10).round() / 100.0));
    Value::Object(out)
}

/// The Error Information Log entries that hold an error (error_count > 0), newest first: the lifetime
/// error counter and the NVMe status of the command that failed (the GUI tells a probe of an unsupported
/// feature from a media error). Nothing that identifies the drive.
fn error_entries(v: &Value) -> Option<Vec<Value>> {
    let a = v.get("error_log_entries").or_else(|| v.get("errors")).and_then(Value::as_array)?;
    Some(a.iter().take(16).filter(|e| e.get("error_count").and_then(num).unwrap_or(0.0) > 0.0).map(|e| {
        let mut o = Map::new();
        for k in ["error_count", "status_field", "sqid", "cmdid", "lba", "nsid"] {
            if let Some(n) = e.get(k).and_then(num) { o.insert(k.to_owned(), json!(n as u64)); }
        }
        Value::Object(o)
    }).collect())
}
fn collect(root: &Path, exe: Option<&str>) -> Value {
    let sys=root.join("sys/class/nvme");
    let mut found=Vec::new();
    if let Ok(dir)=std::fs::read_dir(sys) {
        for entry in dir.flatten() {
            let name=entry.file_name().to_string_lossy().into_owned();
            if controller(&name) { found.push((name,entry.path())); }
        }
    }
    found.sort_by(|a,b|a.0.cmp(&b.0));
    found.truncate(MAX_CONTROLLERS);
    let mut controllers=Vec::new();
    for (name,base) in found {
        let state=rd(&base.join("state"));
        let fw=rd(&base.join("firmware_rev"));
        let model=rd(&base.join("model"));
        // Only canonical /dev/nvme<digits> character devices. Symlink paths,
        // missing devices and unusable states remain UNKNOWN.
        let path=PathBuf::from(format!("/dev/{name}"));
        let valid = path.symlink_metadata().ok().is_some_and(|m| m.file_type().is_char_device());
        let mut c=json!({"name":name,"state":state,"firmware":fw,"model":model,
            "smart_status":"unavailable", "error_status":"unavailable"});
        if let Some(bin)=exe.filter(|_|valid && state=="live") {
            let dev=path.to_string_lossy();
            match query(bin,"smart-log",&dev) {
                Ok(v) => {c["smart"]=smart_public(&v);c["smart_status"]=json!("read-only NVMe SMART success");},
                Err(e) => c["smart_status"]=json!(e),
            }
            match query(bin,"error-log",&dev) {
                Ok(v) => {
                    if let Some(entries) = error_entries(&v) {
                        c["error_log"]=json!({"entries":entries.len(),"items":entries});
                        c["error_status"]=json!("read-only Error Information Log success");
                    } else { c["error_status"]=json!("error log format unrecognized; number of entries unknown"); }
                },
                Err(e) => c["error_status"]=json!(e),
            }
        } else if exe.is_none() { c["smart_status"]=json!("nvme-cli not installed"); }
        else { c["smart_status"]=json!("controller not live or device unavailable"); }
        controllers.push(c);
    }
    json!({"ok":true,"controllers":controllers,"read_only":true,
       "limits":"Max 8 controllers; at most 16 error entries each; 4s per command; missing fields unknown"})
}
pub fn report() -> Value { collect(Path::new("/"), nvme_binary()) }

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn whitelisted_controller_paths() {
        assert!(controller("nvme0")); assert!(controller("nvme12"));
        for value in ["nvme0n1", "nvme../x", "nvme", "nvme0;reboot", "sda", "nvme1234"] { assert!(!controller(value)); }
    }
    #[test] fn smart_does_not_expose_identifiers() {
        let v=json!({"media_errors":4,"serial_number":"private","temperature":310,"avail_spare":97,"spare_thresh":10,
                     "percent_used":"12","data_units_written":"2000000"});
        let r=smart_public(&v);
        assert_eq!(r["media_errors"],4.0);assert!(r.get("serial_number").is_none());
        assert_eq!(r["temp_c"],37.0);assert_eq!(r["spare_pct"],97.0);assert_eq!(r["life_used_pct"],12.0);
        assert_eq!(r["written_tb"],1.02);
        let smartctl=smart_public(&json!({"available_spare":100,"percentage_used":3,"temperature":41}));
        assert_eq!(smartctl["spare_pct"],100.0);assert_eq!(smartctl["temp_c"],41.0);
    }
    #[test] fn absent_error_entries_are_not_claimed_zero_health() {
        assert_eq!(error_entries(&json!({})),None);
        let e=error_entries(&json!({"errors":[{"error_count":2,"status_field":"0x2002","sqid":0},{"error_count":0}]})).unwrap();
        assert_eq!(e.len(),1);
        assert_eq!(e[0]["status_field"],0x2002);
        assert_eq!(e[0]["error_count"],2);
        let r=collect(Path::new("/nonexistent-centurion-test"),None);
        assert_eq!(r["controllers"].as_array().unwrap().len(),0);
    }
}
