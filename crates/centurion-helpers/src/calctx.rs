//! Measurement context of a calibration session, and the power context a scene sets.
//!
//! A knob's effect depends on what the firmware lets the CPU do: EPP and boost act differently
//! under a 45 W Quiet limit than under 130 W Performance, a locked fan changes how fast the die
//! heats, an undervolt changes the clock a temperature buys. centurion-calibrate records this context
//! with every session (`Calibration::contexts`); the model learns a knob's effect per context
//! (context pseudo-knobs, `ctx.*`), and autotune reads its decisions in the context the goal will
//! run in.
//!
//! `--scene NAME` measures in a scene's power context: its power profile, firmware limits and
//! fan mode are written before the session (directly through the same root helpers the GUI uses)
//! and put back afterwards; the scene's Optimizations preset is not applied - those knobs are
//! what is being measured. CPU/GPU curves are recorded, never changed by a calibration.

use serde_json::{json, Map, Value};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// Last Curve Optimizer state ryzen-co-helper applied (tmpfs: a reboot resets the SMU too).
pub const CO_STATE: &str = "/run/centurion/co-state.json";
/// What `apply_scene_power` changed, for `restore` after an interrupted run.
pub const SCENE_JOURNAL: &str = "/run/centurion/calibrate-scene.json";
const HELPER_DIR: &str = match option_env!("CENTURION_HELPER_DIR") { Some(d) => d, None => "/usr/libexec/centurion" };

/// Firmware attributes recorded as context (read through sysfs; never woken, never written here).
const FW_ATTRS: &[&str] = &["ppt_pl1_spl", "ppt_pl2_sppt", "ppt_pl3_fppt", "cpu_temp", "gpu_nv_ac_offset", "gpu_temp"];
/// GPU knobs behind WMI: read only while the dGPU is usable.
const GPU_KEYS: &[(&str, &str)] = &[("gpu_nv_ctgp", "ctgp"), ("gpu_nv_ppab", "boost_up"), ("gpu_nv_cpu_boost", "boost_down")];

fn fw_dir(attr: &str) -> Option<PathBuf> {
    std::fs::read_dir("/sys/class/firmware-attributes").ok()?.flatten()
        .map(|d| d.path().join("attributes").join(attr)).find(|d| d.join("current_value").is_file())
}
fn fw_read(attr: &str) -> Option<i64> { std::fs::read_to_string(fw_dir(attr)?.join("current_value")).ok()?.trim().parse().ok() }

/// The platform-profile node the GUI and centurion-gamemode use (Custom-capable first).
pub fn profile_node() -> Option<String> {
    let dir = Path::new("/sys/class/platform-profile");
    let rd = |n: &str, f: &str| std::fs::read_to_string(dir.join(n).join(f)).ok().map(|s| s.trim().to_owned());
    let mut v: Vec<(bool, usize, String)> = std::fs::read_dir(dir).ok()?.flatten().filter_map(|e| {
        let node = e.file_name().to_string_lossy().into_owned();
        let (name, choices) = (rd(&node, "name")?, rd(&node, "choices")?);
        let choices: Vec<&str> = choices.split_whitespace().collect();
        let name = name.to_lowercase();
        let custom = choices.contains(&"custom") || ["lenovo", "gamezone", "legion", "ideapad"].iter().any(|h| name.contains(h));
        Some((custom, choices.len(), node))
    }).collect();
    v.sort_by(|a, b| b.0.cmp(&a.0).then(b.1.cmp(&a.1)).then(a.2.cmp(&b.2)));
    v.into_iter().next().map(|x| x.2)
}

pub fn current_profile() -> Option<String> {
    let p = match profile_node() {
        Some(n) => PathBuf::from(format!("/sys/class/platform-profile/{n}/profile")),
        None => PathBuf::from("/sys/firmware/acpi/platform_profile"),
    };
    std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned()).filter(|s| !s.is_empty())
}

/// The context of this moment (root for the WMI/EC reads; without root those parts are null).
/// `fan_locked`: the session holds the EC fan at full speed itself.
pub fn capture(scene: Option<&str>) -> Value {
    let mut limits = Map::new();
    for a in FW_ATTRS { if let Some(v) = fw_read(a) { limits.insert((*a).into(), json!(v)); } }
    let root = unsafe { libc::geteuid() } == 0;
    if root && crate::dgpu_usable() {
        for (attr, key) in GPU_KEYS { if let Ok(v) = crate::legion_wmi::read_feature(key) { limits.insert((*attr).into(), json!(v)); } }
    }
    let fan = if root { crate::legion_wmi::fan_fullspeed_get().ok().map(|on| if on { "full" } else { "auto" }) } else { None };
    // No record since boot on AMD = the SMU's own zero offsets (a reboot resets both).
    let co = crate::read_root_file(CO_STATE, 64 * 1024).and_then(|s| serde_json::from_str::<Value>(&s).ok()).map(|v| v["state"].clone())
        .or_else(|| (crate::tune::cpu_vendor() == crate::tune::Vendor::Amd).then(|| json!("reset")));
    let mut d = json!({
        "profile": current_profile(),
        "power": match crate::power::on_ac() { Some(true) => "ac", Some(false) => "battery", None => "?" },
        "limits": if limits.is_empty() { Value::Null } else { Value::Object(limits) },
        "fan": fan, "co": co,
    });
    if let Some(s) = scene { d["scene"] = json!(s); }
    d
}

/// One line for logs and `--show`.
pub fn describe(d: &Value) -> String {
    let mut p = vec![format!("profile {}", d["profile"].as_str().unwrap_or("?")), format!("on {}", d["power"].as_str().unwrap_or("?"))];
    if let Some(l) = d["limits"].as_object() {
        let g = |k: &str| l.get(k).and_then(Value::as_i64).map(|v| v.to_string()).unwrap_or_else(|| "-".into());
        p.push(format!("PL1/PL2/PL3 {}/{}/{} W", g("ppt_pl1_spl"), g("ppt_pl2_sppt"), g("ppt_pl3_fppt")));
        if l.contains_key("gpu_nv_ctgp") { p.push(format!("cTGP {} W + boost {} W", g("gpu_nv_ctgp"), g("gpu_nv_ppab"))); }
    }
    if let Some(f) = d["fan"].as_str() { p.push(format!("fan {f}")); }
    match &d["co"] { Value::Null => {}, v => p.push(format!("CPU curve {}", if v == "reset" { "0".to_owned() } else { v.to_string().chars().take(40).collect() })) }
    if let Some(s) = d["scene"].as_str() { p.push(format!("scene \"{s}\"")); }
    p.join(" · ")
}

/// Runs a root helper directly (we are root: no pkexec) with the calibration-owner mark, so the
/// helpers' calibration hold lets this process through.
pub fn run_helper(name: &str, req: &Value) -> Result<Value, String> {
    let exe = format!("{HELPER_DIR}/{name}");
    if !Path::new(&exe).is_file() { return Err(format!("{exe} not installed")); }
    let mut child = Command::new(&exe).env_clear().env("PATH", "/usr/bin:/bin:/usr/sbin:/sbin")
        .env("CENTURION_CALIBRATE_OWNER", std::env::var("CENTURION_CALIBRATE_OWNER").unwrap_or_else(|_| std::process::id().to_string()))
        .stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| format!("{name}: {e}"))?;
    child.stdin.take().ok_or("no stdin")?.write_all(req.to_string().as_bytes()).map_err(|e| format!("{name}: {e}"))?;
    let out = child.wait_with_output().map_err(|e| format!("{name}: {e}"))?;
    let text = String::from_utf8_lossy(&out.stdout);
    text.lines().last().and_then(|l| serde_json::from_str::<Value>(l).ok())
        .ok_or_else(|| format!("{name}: {}", String::from_utf8_lossy(&out.stderr).trim()))
}

fn ok(v: &Value) -> Result<(), String> { if v["ok"] == true { Ok(()) } else { Err(v["error"].as_str().unwrap_or("failed").to_owned()) } }

/// Home directory of a user (passwd).
pub fn home_of(user: &str) -> Option<String> {
    std::fs::read_to_string("/etc/passwd").ok()?.lines().find_map(|l| {
        let f: Vec<&str> = l.split(':').collect();
        (f.len() >= 6 && (f[0] == user || f[2] == user)).then(|| f[5].to_owned())
    })
}

/// Name of a user id (passwd).
pub fn user_of(uid: &str) -> Option<String> {
    std::fs::read_to_string("/etc/passwd").ok()?.lines().find_map(|l| {
        let f: Vec<&str> = l.split(':').collect();
        (f.len() >= 3 && f[2] == uid).then(|| f[0].to_owned())
    })
}

pub fn valid_scene_name(name: &str) -> bool {
    !name.is_empty() && name.len() <= 64 && name.chars().all(|c| c.is_ascii_alphanumeric() || " _-.".contains(c)) && !name.starts_with('.')
}

/// A user's scene file (scenes are per user: ~/.config/centurion/scenes).
pub fn scene_file_for(user: &str, name: &str) -> Result<PathBuf, String> {
    if !valid_scene_name(name) { return Err(format!("invalid scene name '{name}'")); }
    let home = home_of(user).ok_or_else(|| format!("no home directory for {user}"))?;
    let p = Path::new(&home).join(".config/centurion/scenes").join(format!("{name}.json"));
    if !p.is_file() { return Err(format!("scene \"{name}\" not found ({})", p.display())); }
    Ok(p)
}

/// The scene file of the user who ran sudo.
pub fn scene_file(name: &str) -> Result<PathBuf, String> {
    if !valid_scene_name(name) { return Err(format!("invalid scene name '{name}'")); }
    let user = std::env::var("SUDO_USER").ok().filter(|u| !u.is_empty() && u != "root")
        .ok_or("--scene needs the scene owner's account: run it with sudo from that account")?;
    scene_file_for(&user, name)
}

// ── calibration at the next boot ─────────────────────────────────────────────

pub const BOOT_FLAG: &str = "/var/lib/centurion/calibrate-on-boot.json";
pub const BOOT_TAKEN: &str = "/var/lib/centurion/calibrate-on-boot.json.taken";
pub const BOOT_STATUS: &str = "/run/centurion/calibrate-status.json";

/// A boot calibration is scheduled or under way (the boot preset waits for it).
pub fn boot_calibration_pending() -> bool { Path::new(BOOT_FLAG).exists() || Path::new(BOOT_TAKEN).exists() }

/// Schedules (or cancels) a calibration at the next boot. `user`: whose scene `scene` is.
pub fn schedule_boot(on: bool, sessions: u64, budget: f64, scene: Option<&str>, user: Option<&str>) -> Result<String, String> {
    if !on {
        let _ = std::fs::remove_file(BOOT_FLAG);
        return Ok("boot calibration cancelled".into());
    }
    let sessions = sessions.clamp(1, 12);
    let budget = budget.clamp(5.0, 60.0);
    if let Some(sc) = scene {
        let u = user.ok_or("a scene needs its owner's account")?;
        scene_file_for(u, sc)?;
    }
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let body = json!({"sessions": sessions, "budget": budget, "scene": scene, "user": user, "t": now});
    crate::secure_dir(crate::defaults::DIR)?;
    crate::write_root_file(BOOT_FLAG, body.to_string().as_bytes())?;
    Ok(format!("next boot: the boot preset and scenes are held, and {sessions} calibration session(s) of {budget:.0} min run once the machine is on AC and \
                nobody has touched it for 5 min (status: centurion-calibrate --status)"))
}

/// What a scene asks of the power context: (profile, firmware limits, fan full speed).
pub fn scene_power(scene: &Value) -> (Option<String>, Map<String, Value>, Option<bool>) {
    let profile = scene["platform_profile"].as_str().filter(|p| !p.is_empty() && p.len() <= 32).map(str::to_owned);
    let fw = scene["firmware"].as_object().cloned().unwrap_or_default();
    (profile, fw, scene["fan_fullspeed"].as_bool())
}

/// The originals a scene's power context replaced.
fn snapshot(fw: &Map<String, Value>) -> Value {
    let mut lim = Map::new();
    for a in fw.keys() {
        if let Some((_, key)) = GPU_KEYS.iter().find(|(x, _)| x == a) {
            if crate::dgpu_usable() { if let Ok(v) = crate::legion_wmi::read_feature(key) { lim.insert(a.clone(), json!(v)); } }
        } else if let Some(v) = fw_read(a) { lim.insert(a.clone(), json!(v)); }
    }
    json!({"profile": current_profile(), "firmware": lim, "fan": crate::legion_wmi::fan_fullspeed_get().ok()})
}

fn write_power(profile: Option<&str>, fw: &Map<String, Value>, fan: Option<bool>) -> Vec<String> {
    let mut errs = Vec::new();
    // Fan first (as a scene does): the fans never spin up for the new profile and then drop again.
    if let Some(on) = fan {
        if let Err(e) = run_helper("legion-profile-helper", &json!({"device": "fan_fullspeed", "value": if on { "1" } else { "0" }})).and_then(|v| ok(&v)) { errs.push(format!("fan: {e}")); }
    }
    if let Some(p) = profile {
        if let Err(e) = run_helper("legion-profile-helper", &json!({"profile": p, "handler": profile_node()})).and_then(|v| ok(&v)) { errs.push(format!("power profile {p}: {e}")); }
    }
    if !fw.is_empty() {
        if current_profile().as_deref() != Some("custom") {
            errs.push("firmware limits need the Custom power profile".into());
        } else {
            let (mut batch, mut wmi) = (Vec::new(), Map::new());
            for (attr, val) in fw {
                let Some(v) = val.as_i64() else { continue };
                if let Some((_, key)) = GPU_KEYS.iter().find(|(a, _)| a == attr) { wmi.insert((*key).into(), json!(v)); continue; }
                if let Some(d) = fw_dir(attr) { batch.push(json!({"path": d.join("current_value"), "value": v})); }
            }
            if !batch.is_empty() { if let Err(e) = run_helper("fwattr-helper", &Value::Array(batch)).and_then(|v| ok(&v)) { errs.push(format!("firmware limits: {e}")); } }
            if !wmi.is_empty() && crate::dgpu_usable() {
                if let Err(e) = run_helper("legion-gpu-helper", &json!({"op": "apply", "values": wmi})).and_then(|v| ok(&v)) { errs.push(format!("GPU limits: {e}")); }
            }
        }
    }
    // The EC may reset the fan flag on a profile change.
    if let (Some(on), Some(_)) = (fan, profile) {
        if crate::legion_wmi::fan_fullspeed_get().ok() != Some(on) {
            let _ = run_helper("legion-profile-helper", &json!({"device": "fan_fullspeed", "value": if on { "1" } else { "0" }}));
        }
    }
    errs
}

/// Writes a scene's power context, journaling the originals first. Returns the problems (the
/// session then runs in whatever context is live; it is recorded as such).
pub fn apply_scene_power(scene: &Value) -> Vec<String> {
    let (profile, fw, fan) = scene_power(scene);
    if profile.is_none() && fw.is_empty() && fan.is_none() { return vec!["the scene sets no power profile, firmware limits or fan mode".into()]; }
    let orig = snapshot(&fw);
    if crate::secure_dir("/run/centurion").is_ok() { let _ = crate::write_root_file(SCENE_JOURNAL, orig.to_string().as_bytes()); }
    write_power(profile.as_deref(), &fw, fan)
}

/// Puts back what `apply_scene_power` changed (no-op without a journal).
pub fn restore_scene_power() -> Vec<String> {
    let Some(orig) = crate::read_root_file(SCENE_JOURNAL, 64 * 1024).and_then(|s| serde_json::from_str::<Value>(&s).ok()) else { return Vec::new() };
    let fw = orig["firmware"].as_object().cloned().unwrap_or_default();
    // Limits are only writable in Custom: write them while Custom is still active, then the profile.
    let mut errs = if fw.is_empty() { Vec::new() } else { write_power(None, &fw, None) };
    errs.extend(write_power(orig["profile"].as_str(), &Map::new(), orig["fan"].as_bool()));
    if errs.is_empty() { let _ = std::fs::remove_file(SCENE_JOURNAL); }
    errs
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn scene_power_and_description() {
        let s = json!({"platform_profile": "custom", "firmware": {"ppt_pl1_spl": 80, "gpu_nv_ctgp": 150}, "fan_fullspeed": true, "tuning": {"profile": "x"}});
        let (p, fw, fan) = scene_power(&s);
        assert_eq!(p.as_deref(), Some("custom"));
        assert_eq!(fw.len(), 2);
        assert_eq!(fan, Some(true));
        let d = json!({"profile": "performance", "power": "ac", "limits": {"ppt_pl1_spl": 80, "ppt_pl2_sppt": 110, "ppt_pl3_fppt": 120}, "fan": "full", "co": "reset"});
        let t = describe(&d);
        assert!(t.contains("80/110/120") && t.contains("fan full") && t.contains("CPU curve 0"), "{t}");
        assert!(scene_file("../x").is_err() && scene_file("").is_err());
    }
}
