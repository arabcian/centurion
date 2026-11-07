//! Internal-panel refresh rate for Scenes (runs as the user, never as root).
//!
//! The refresh rate belongs to the compositor, not to sysfs, so it goes through the desktop's own tool:
//!   KDE Plasma (Wayland and X11)  kscreen-doctor -j / output.NAME.mode.ID
//!   wlroots compositors           wlr-randr --json / --output NAME --mode WxH@RHz
//!   other X11 sessions            xrandr --query / --output NAME --mode WxH --rate R
//! GNOME on Wayland has no such command-line tool before gdctl; it reports "unsupported" and the scene's
//! Run command stays the way to do it there.
//!
//! Only the internal panel (eDP / LVDS / DSI) is touched, and only its refresh rate: the resolution stays
//! the current one and the closest offered rate at that resolution is picked.
//!
//! Screen brightness (Scenes, after the run command) is the internal panel's backlight in
//! /sys/class/backlight, stored as a percentage so a scene means the same on any panel. Still as the user:
//! the sysfs file when it is writable, otherwise systemd-logind / elogind's Session.SetBrightness (the call
//! desktops use themselves; allowed for the active local session), otherwise brightnessctl.

use serde_json::{json, Value};
use std::io::Read;
use std::path::Path;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Backend { KScreen, Wlr, Xrandr }

impl Backend {
    pub fn name(self) -> &'static str {
        match self { Backend::KScreen => "kscreen-doctor", Backend::Wlr => "wlr-randr", Backend::Xrandr => "xrandr" }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Mode { pub id: String, pub width: u32, pub height: u32, pub refresh: f64, pub current: bool }

#[derive(Clone, Debug, PartialEq)]
pub struct Output { pub name: String, pub enabled: bool, pub modes: Vec<Mode> }

pub fn is_internal(name: &str) -> bool {
    let n = name.to_ascii_lowercase();
    n.starts_with("edp") || n.starts_with("lvds") || n.starts_with("dsi")
}

fn in_path(prog: &str) -> bool {
    std::env::var_os("PATH").map_or(false, |p| std::env::split_paths(&p).any(|d| {
        let f = d.join(prog);
        std::fs::metadata(&f).map_or(false, |m| m.is_file())
    }))
}

fn env_has(k: &str) -> bool { std::env::var_os(k).map_or(false, |v| !v.is_empty()) }

/// The tool for this session, or None (no graphical session in the environment, or no supported tool).
pub fn detect() -> Option<Backend> {
    let desktop = std::env::var("XDG_CURRENT_DESKTOP").unwrap_or_default().to_ascii_uppercase();
    let wayland = env_has("WAYLAND_DISPLAY");
    if desktop.contains("KDE") && in_path("kscreen-doctor") { return Some(Backend::KScreen); }
    if wayland && !desktop.contains("GNOME") && in_path("wlr-randr") { return Some(Backend::Wlr); }
    if !wayland && env_has("DISPLAY") && in_path("xrandr") { return Some(Backend::Xrandr); }
    None
}

/// Runs a tool with a hard time limit (a compositor that does not answer must not hang a scene).
fn run(prog: &str, args: &[&str], limit: Duration) -> Result<String, String> {
    let mut child = Command::new(prog).args(args).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped())
        .spawn().map_err(|e| format!("{prog}: {e}"))?;
    // Drained while the child runs: output past the pipe buffer (kscreen-doctor -j with an external
    // monitor's mode list) would otherwise block the child until the time limit.
    let drain = |s: Option<Box<dyn Read + Send>>| std::thread::spawn(move || {
        let mut v = Vec::new();
        if let Some(mut s) = s { let _ = s.read_to_end(&mut v); }
        String::from_utf8_lossy(&v).into_owned()
    });
    let out_t = drain(child.stdout.take().map(|s| Box::new(s) as Box<dyn Read + Send>));
    let err_t = drain(child.stderr.take().map(|s| Box::new(s) as Box<dyn Read + Send>));
    let t0 = Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(st)) => {
                let out = out_t.join().unwrap_or_default();
                let err = err_t.join().unwrap_or_default();
                // kscreen-doctor prints its JSON / errors on stdout or stderr depending on the version.
                return if st.success() { Ok(out) } else { Err(format!("{prog}: {}", if err.trim().is_empty() { out.trim() } else { err.trim() })) };
            }
            Ok(None) if t0.elapsed() < limit => std::thread::sleep(Duration::from_millis(25)),
            Ok(None) => { let _ = child.kill(); let _ = child.wait(); return Err(format!("{prog}: no answer within {} s", limit.as_secs())); }
            Err(e) => return Err(format!("{prog}: {e}")),
        }
    }
}

/// kscreen-doctor -j: {"outputs": [{"name", "enabled", "currentModeId", "modes": [{"id", "refreshRate", "size": {w, h}}]}]}
pub fn parse_kscreen(text: &str) -> Vec<Output> {
    // Older versions print log lines before the JSON object.
    let start = text.find('{').unwrap_or(0);
    let Ok(v) = serde_json::from_str::<Value>(&text[start..]) else { return Vec::new() };
    v["outputs"].as_array().into_iter().flatten().filter(|o| o["connected"] != false).map(|o| {
        let cur = match &o["currentModeId"] { Value::String(s) => s.clone(), Value::Number(n) => n.to_string(), _ => String::new() };
        let modes = o["modes"].as_array().into_iter().flatten().filter_map(|m| {
            let id = match &m["id"] { Value::String(s) => s.clone(), Value::Number(n) => n.to_string(), _ => return None };
            Some(Mode {
                current: id == cur,
                width: m["size"]["width"].as_u64()? as u32, height: m["size"]["height"].as_u64()? as u32,
                refresh: m["refreshRate"].as_f64()?, id,
            })
        }).collect();
        Output { name: o["name"].as_str().unwrap_or("").to_owned(), enabled: o["enabled"] != false, modes }
    }).filter(|o| !o.name.is_empty()).collect()
}

/// wlr-randr --json: [{"name", "enabled", "modes": [{"width", "height", "refresh", "current"}]}]
pub fn parse_wlr(text: &str) -> Vec<Output> {
    let Ok(v) = serde_json::from_str::<Value>(text) else { return Vec::new() };
    v.as_array().into_iter().flatten().map(|o| Output {
        name: o["name"].as_str().unwrap_or("").to_owned(),
        enabled: o["enabled"] != false,
        modes: o["modes"].as_array().into_iter().flatten().filter_map(|m| {
            let (w, h, r) = (m["width"].as_u64()? as u32, m["height"].as_u64()? as u32, m["refresh"].as_f64()?);
            Some(Mode { id: format!("{w}x{h}@{r:.6}Hz"), width: w, height: h, refresh: r, current: m["current"] == true })
        }).collect(),
    }).filter(|o| !o.name.is_empty()).collect()
}

/// xrandr --query: "eDP-1 connected primary 2560x1600+0+0 …" followed by "   2560x1600  240.00*+  60.00".
pub fn parse_xrandr(text: &str) -> Vec<Output> {
    let mut outs: Vec<Output> = Vec::new();
    for line in text.lines() {
        if !line.starts_with(' ') && !line.starts_with('\t') {
            let mut f = line.split_whitespace();
            let (Some(name), Some(state)) = (f.next(), f.next()) else { continue };
            // Anything else (a disconnected output, the "Screen 0:" line) gets a nameless sentinel so its mode
            // lines are not attached to the output before it.
            let keep = state == "connected";
            outs.push(Output { name: if keep { name.to_owned() } else { String::new() }, enabled: keep && line.contains('+'), modes: Vec::new() });
            continue;
        }
        let Some(o) = outs.last_mut() else { continue };
        if o.name.is_empty() { continue; }
        let mut f = line.split_whitespace();
        let Some(res) = f.next() else { continue };
        let Some((w, h)) = res.split_once('x') else { continue };
        let (Ok(w), Ok(h)) = (w.parse::<u32>(), h.trim_end_matches('i').parse::<u32>()) else { continue };
        for r in f {
            let current = r.contains('*');
            let num: String = r.chars().filter(|c| c.is_ascii_digit() || *c == '.').collect();
            if let Ok(rate) = num.parse::<f64>() {
                o.modes.push(Mode { id: format!("{w}x{h}@{num}"), width: w, height: h, refresh: rate, current });
            }
        }
    }
    outs.retain(|o| !o.name.is_empty());
    outs
}

pub fn outputs(b: Backend) -> Result<Vec<Output>, String> {
    let limit = Duration::from_secs(6);
    Ok(match b {
        Backend::KScreen => parse_kscreen(&run("kscreen-doctor", &["-j"], limit)?),
        Backend::Wlr => parse_wlr(&run("wlr-randr", &["--json"], limit)?),
        Backend::Xrandr => parse_xrandr(&run("xrandr", &["--query"], limit)?),
    })
}

/// The enabled internal panel.
pub fn panel(outs: &[Output]) -> Option<&Output> { outs.iter().find(|o| is_internal(&o.name) && o.enabled) }

/// Refresh rates the panel offers at its current resolution, highest first (rounded to 0.01 Hz, unique).
pub fn panel_rates(o: &Output) -> (Option<(u32, u32)>, Vec<f64>, Option<f64>) {
    let cur = o.modes.iter().find(|m| m.current);
    let Some(c) = cur else { return (None, Vec::new(), None) };
    let mut r: Vec<f64> = o.modes.iter().filter(|m| m.width == c.width && m.height == c.height).map(|m| (m.refresh * 100.0).round() / 100.0).collect();
    r.sort_by(|a, b| b.partial_cmp(a).unwrap_or(std::cmp::Ordering::Equal));
    r.dedup_by(|a, b| (*a - *b).abs() < 0.05);
    (Some((c.width, c.height)), r, Some(c.refresh))
}

/// The mode at the current resolution whose refresh is closest to `hz`.
pub fn pick(o: &Output, hz: f64) -> Option<&Mode> {
    let c = o.modes.iter().find(|m| m.current)?;
    o.modes.iter().filter(|m| m.width == c.width && m.height == c.height)
        .min_by(|a, b| (a.refresh - hz).abs().partial_cmp(&(b.refresh - hz).abs()).unwrap_or(std::cmp::Ordering::Equal))
}

/// Sets the internal panel to the offered refresh rate closest to `hz` (resolution unchanged).
pub fn set_refresh(hz: f64) -> Result<String, String> {
    if !(20.0..=1000.0).contains(&hz) { return Err(format!("refresh {hz} Hz is out of range")); }
    let b = detect().ok_or("no supported display tool for this session (KDE kscreen-doctor, wlr-randr or X11 xrandr)")?;
    let outs = outputs(b)?;
    let o = panel(&outs).ok_or("no enabled internal panel (eDP) found")?;
    let m = pick(o, hz).ok_or("the panel reports no current mode")?.clone();
    if m.current { return Ok(format!("{} already at {:.0} Hz", o.name, m.refresh)); }
    let limit = Duration::from_secs(8);
    match b {
        Backend::KScreen => run("kscreen-doctor", &[&format!("output.{}.mode.{}", o.name, m.id)], limit)?,
        Backend::Wlr => run("wlr-randr", &["--output", &o.name, "--mode", &m.id], limit)?,
        Backend::Xrandr => {
            let (res, rate) = m.id.split_once('@').unwrap_or((&m.id, ""));
            run("xrandr", &["--output", &o.name, "--mode", res, "--rate", rate], limit)?
        }
    };
    Ok(format!("{} → {}x{} @ {:.0} Hz", o.name, m.width, m.height, m.refresh))
}

/// `centurion-display list` reply.
pub fn list_json() -> Value {
    let Some(b) = detect() else {
        return json!({"ok": false, "error": "no supported display tool for this session (KDE kscreen-doctor, wlr-randr or X11 xrandr)"});
    };
    match outputs(b) {
        Err(e) => json!({"ok": false, "backend": b.name(), "error": e}),
        Ok(outs) => match panel(&outs) {
            None => json!({"ok": false, "backend": b.name(), "error": "no enabled internal panel (eDP) found"}),
            Some(o) => {
                let (res, rates, cur) = panel_rates(o);
                json!({"ok": true, "backend": b.name(), "output": o.name, "resolution": res.map(|(w, h)| format!("{w}x{h}")),
                       "rates": rates, "current": cur})
            }
        },
    }
}

// ── internal-panel backlight ───────────────────────────────────────────────

pub const BACKLIGHT_DIR: &str = "/sys/class/backlight";

#[derive(Clone, Debug, PartialEq)]
pub struct Backlight { pub name: String, pub kind: String, pub max: u32, pub cur: u32, pub active: bool }

fn read_trim(p: &Path) -> Option<String> { std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned()) }

/// Every backlight device under `root` (normally /sys/class/backlight).
pub fn backlights_in(root: &Path) -> Vec<Backlight> {
    let mut v: Vec<Backlight> = std::fs::read_dir(root).into_iter().flatten().flatten().filter_map(|e| {
        let d = e.path();
        let max = read_trim(&d.join("max_brightness"))?.parse::<u32>().ok().filter(|m| *m > 0)?;
        let cur = read_trim(&d.join("brightness")).and_then(|s| s.parse::<u32>().ok()).unwrap_or(0);
        // A raw (GPU) backlight sits under its DRM connector; a disabled connector (the other GPU's eDP on a
        // MUX machine) is not the panel being shown.
        let active = read_trim(&d.join("device/enabled")).map_or(true, |s| s != "disabled");
        Some(Backlight { name: e.file_name().to_string_lossy().into_owned(), kind: read_trim(&d.join("type")).unwrap_or_default(), max, cur, active })
    }).collect();
    v.sort_by(|a, b| a.name.cmp(&b.name));
    v
}

/// The backlight that drives the panel: active ones first, then firmware > platform > raw (the order
/// systemd-backlight and the desktops use — the kernel registers a firmware/platform interface only when it
/// is the one in charge), then the finer scale.
pub fn pick_backlight(v: &[Backlight]) -> Option<&Backlight> {
    let rank = |k: &str| match k { "firmware" => 0, "platform" => 1, "raw" => 2, _ => 3 };
    v.iter().min_by_key(|b| (!b.active, rank(&b.kind), std::cmp::Reverse(b.max)))
}

pub fn percent_to_raw(pct: u32, max: u32) -> u32 { ((pct.min(100) as u64 * max as u64 + 50) / 100).max(1) as u32 }
pub fn raw_to_percent(raw: u32, max: u32) -> u32 { if max == 0 { 0 } else { ((raw.min(max) as u64 * 100 + max as u64 / 2) / max as u64) as u32 } }

/// Current panel brightness: (device, percent).
pub fn brightness() -> Result<(String, u32), String> {
    let all = backlights_in(Path::new(BACKLIGHT_DIR));
    let b = pick_backlight(&all).ok_or("no backlight device (/sys/class/backlight is empty)")?;
    Ok((b.name.clone(), raw_to_percent(b.cur, b.max)))
}

/// Sets the internal panel's backlight to `pct` percent (1-100; never 0, which turns some panels off).
pub fn set_brightness(pct: u32) -> Result<String, String> {
    if !(1..=100).contains(&pct) { return Err(format!("brightness {pct} % is out of range (1-100)")); }
    let all = backlights_in(Path::new(BACKLIGHT_DIR));
    let b = pick_backlight(&all).ok_or("no backlight device (/sys/class/backlight is empty)")?;
    let raw = percent_to_raw(pct, b.max);
    let done = || format!("{} → {pct} % ({raw}/{})", b.name, b.max);
    if b.cur == raw { return Ok(format!("{} already at {pct} %", b.name)); }

    // 1. Writable sysfs (a udev rule or group grants it on some systems).
    if std::fs::write(Path::new(BACKLIGHT_DIR).join(&b.name).join("brightness"), raw.to_string()).is_ok() { return Ok(done()); }

    // 2. logind / elogind: Session.SetBrightness("backlight", NAME, RAW) on the caller's session.
    let limit = Duration::from_secs(4);
    let mut errs: Vec<String> = Vec::new();
    for session in ["auto", "self"] {
        let path = format!("/org/freedesktop/login1/session/{session}");
        let r = if in_path("dbus-send") {
            run("dbus-send", &["--system", "--print-reply", "--dest=org.freedesktop.login1", &path,
                               "org.freedesktop.login1.Session.SetBrightness", "string:backlight",
                               &format!("string:{}", b.name), &format!("uint32:{raw}")], limit)
        } else if in_path("busctl") {
            run("busctl", &["call", "org.freedesktop.login1", &path, "org.freedesktop.login1.Session",
                            "SetBrightness", "ssu", "backlight", &b.name, &raw.to_string()], limit)
        } else { break };
        match r { Ok(_) => return Ok(done()), Err(e) => errs.push(e) }
    }

    // 3. brightnessctl (setuid or logind-backed on most distributions).
    if in_path("brightnessctl") {
        match run("brightnessctl", &["--quiet", "--device", &b.name, "set", &raw.to_string()], limit) {
            Ok(_) => return Ok(done()),
            Err(e) => errs.push(e),
        }
    }
    Err(if errs.is_empty() {
        format!("cannot write {} (no writable sysfs file, no dbus-send / busctl for logind, no brightnessctl)", b.name)
    } else { errs.join("; ") })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn kscreen() {
        let t = r#"{"outputs":[{"name":"eDP-1","connected":true,"enabled":true,"currentModeId":"2",
            "modes":[{"id":"1","refreshRate":60.0,"size":{"width":2560,"height":1600}},
                     {"id":"2","refreshRate":240.01,"size":{"width":2560,"height":1600}},
                     {"id":"3","refreshRate":60.0,"size":{"width":1920,"height":1200}}]},
            {"name":"HDMI-A-1","connected":false,"enabled":false,"modes":[]}]}"#;
        let o = parse_kscreen(t);
        assert_eq!(o.len(), 1);
        let p = panel(&o).unwrap();
        let (res, rates, cur) = panel_rates(p);
        assert_eq!(res, Some((2560, 1600)));
        assert_eq!(rates, vec![240.01, 60.0]);
        assert_eq!(cur, Some(240.01));
        assert_eq!(pick(p, 60.0).unwrap().id, "1");
        assert_eq!(pick(p, 165.0).unwrap().id, "2");
    }

    #[test]
    fn wlr() {
        let t = r#"[{"name":"eDP-1","enabled":true,"modes":[{"width":2560,"height":1600,"refresh":240.0,"current":true},
                   {"width":2560,"height":1600,"refresh":60.0,"current":false}]}]"#;
        let o = parse_wlr(t);
        assert_eq!(pick(&o[0], 59.0).unwrap().id, "2560x1600@60.000000Hz");
    }

    #[test]
    fn xrandr() {
        let t = "Screen 0: minimum 8 x 8\neDP-1 connected primary 2560x1600+0+0 (normal) 344mm x 215mm\n   2560x1600    240.00*+  60.00    48.00  \n   1920x1200    240.00  \nHDMI-1 disconnected (normal)\n   1920x1080     60.00\n";
        let o = parse_xrandr(t);
        assert_eq!(o.len(), 1);
        let (_, rates, cur) = panel_rates(&o[0]);
        assert_eq!(rates, vec![240.0, 60.0, 48.0]);
        assert_eq!(cur, Some(240.0));
        assert_eq!(pick(&o[0], 60.0).unwrap().id, "2560x1600@60.00");
    }

    #[test]
    fn internal() {
        assert!(is_internal("eDP-1") && is_internal("LVDS1") && is_internal("DSI-1") && !is_internal("DP-2"));
    }

    #[test]
    fn backlight_pick_and_scale() {
        let bl = |name: &str, kind: &str, max: u32, active: bool| Backlight { name: name.into(), kind: kind.into(), max, cur: 0, active };
        let v = vec![bl("intel_backlight", "raw", 96000, true), bl("nvidia_wmi_ec_backlight", "firmware", 100, true)];
        assert_eq!(pick_backlight(&v).unwrap().name, "nvidia_wmi_ec_backlight");
        let v = vec![bl("amdgpu_bl1", "raw", 255, false), bl("nvidia_0", "raw", 100, true)];
        assert_eq!(pick_backlight(&v).unwrap().name, "nvidia_0");
        assert!(pick_backlight(&[]).is_none());
        assert_eq!(percent_to_raw(50, 255), 128);
        assert_eq!(percent_to_raw(1, 10), 1);   // never 0
        assert_eq!(percent_to_raw(100, 96000), 96000);
        assert_eq!(raw_to_percent(128, 255), 50);
        assert_eq!(raw_to_percent(96000, 96000), 100);
    }

    #[test]
    fn backlight_sysfs() {
        let root = std::env::temp_dir().join(format!("centurion-bl-{}", std::process::id()));
        let d = root.join("intel_backlight");
        std::fs::create_dir_all(&d).unwrap();
        std::fs::write(d.join("max_brightness"), "480\n").unwrap();
        std::fs::write(d.join("brightness"), "240\n").unwrap();
        std::fs::write(d.join("type"), "raw\n").unwrap();
        let v = backlights_in(&root);
        assert_eq!(v, vec![Backlight { name: "intel_backlight".into(), kind: "raw".into(), max: 480, cur: 240, active: true }]);
        let _ = std::fs::remove_dir_all(&root);
    }
}
