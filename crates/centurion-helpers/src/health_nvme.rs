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

const SMART_FIELDS: &[&str] = &[
    "critical_warning", "temperature", "available_spare", "percentage_used",
    "media_errors", "num_err_log_entries", "unsafe_shutdowns", "warning_temp_time",
    "critical_comp_time", "thermal_mgmt_temp1_transition_count",
    "thermal_mgmt_temp2_transition_count",
];
fn smart_public(v: &Value) -> Value {
    let mut out=Map::new();
    for &k in SMART_FIELDS {
        if let Some(x)=v.get(k).filter(|x| x.is_number() || x.is_string()) { out.insert(k.to_owned(), x.clone()); }
    }
    Value::Object(out)
}
fn count_errors(v: &Value) -> Option<usize> {
    let a=v.get("error_log_entries").or_else(||v.get("errors")).and_then(Value::as_array);
    a.map(|entries| entries.iter().take(16).filter(|x| {
        x.get("error_count").map(|n| n.as_u64().unwrap_or(0) > 0 ||
            n.as_str().is_some_and(|s| s.parse::<u64>().unwrap_or(0) > 0)).unwrap_or(false)
    }).count())
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
        // Only canonical /dev/nvme<digits> character devices. Symlink paths,
        // missing devices and unusable states remain UNKNOWN.
        let path=PathBuf::from(format!("/dev/{name}"));
        let valid = path.symlink_metadata().ok().is_some_and(|m| m.file_type().is_char_device());
        let mut c=json!({"name":name,"state":state,"firmware":fw,
            "smart_status":"unavailable", "error_status":"unavailable"});
        if let Some(bin)=exe.filter(|_|valid && state=="live") {
            let dev=path.to_string_lossy();
            match query(bin,"smart-log",&dev) {
                Ok(v) => {c["smart"]=smart_public(&v);c["smart_status"]=json!("read-only NVMe SMART success");},
                Err(e) => c["smart_status"]=json!(e),
            }
            match query(bin,"error-log",&dev) {
                Ok(v) => {
                    if let Some(count) = count_errors(&v) {
                        c["error_log"]=json!({"entries":count});
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
        let v=json!({"media_errors":4,"serial_number":"private","temperature":300});
        let r=smart_public(&v);
        assert_eq!(r["media_errors"],4);assert!(r.get("serial_number").is_none());
    }
    #[test] fn absent_error_entries_are_not_claimed_zero_health() {
        assert_eq!(count_errors(&json!({})),None);
        assert_eq!(count_errors(&json!({"errors":[{"error_count":2}]})),Some(1));
        let r=collect(Path::new("/nonexistent-centurion-test"),None);
        assert_eq!(r["controllers"].as_array().unwrap().len(),0);
    }
}
