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

use serde_json::{json, Value};
use std::io::Read;
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
}
