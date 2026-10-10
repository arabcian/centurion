//! centurion-gamemode — Lutris / Steam front end for the Optimizations presets.
//! Replaces lutris-game-tune-wrapper without a setuid binary: it runs as the
//! user and reaches root only through `pkexec tune-helper` (same polkit rule
//! as the other helpers). The game itself never runs with elevated rights.
//!
//!   centurion-gamemode PRE  [preset]            apply preset, refcounted game mode
//!   centurion-gamemode POST                     the last POST restores the originals
//!   centurion-gamemode RUN  [preset] [--] cmd…  nice/autogroup boost + CCD affinity, then exec
//!   centurion-gamemode WRAP [preset] [--] cmd…  PRE + RUN + POST around one command (Steam: %command%)
//!   centurion-gamemode APPLY preset             apply as a manual (non-refcounted) change
//!   centurion-gamemode UNDERVOLT                apply the "GAMING" CPU/GPU curve presets now
//!   centurion-gamemode SCENE name               apply a saved scene now (all components)
//!   centurion-gamemode RESTORE                  restore everything now
//!   centurion-gamemode STATUS                   live values and game-mode state
//!
//! Without a preset name, the game preset chosen in the GUI is used
//! (~/.config/centurion/tune.json).
//!
//! Game launch settings (Optimizations → Game launch, tune.json):
//!   "game_scene"    a saved scene the first PRE / WRAP switches to; the last
//!                   POST returns to the scene active before the game (or to
//!                   the AC / battery scene when automatic switching is on).
//!                   The scene's Optimizations part is skipped: the game preset
//!                   owns tuning while a game runs.
//!   "undervolt_cpu" / "undervolt_gpu"
//!                   with a game scene: whether its CPU / GPU curve is applied;
//!                   without one: apply the "GAMING" curve profiles (CPU first,
//!                   GPU 2 s later; a missing profile is skipped with a note).
//!   "field_ab"      true (every game) or a list of game names: A/B comparison -
//!                   launches of a game alternate B (game preset) and A (boot
//!                   defaults: no preset, no launch boost; same game scene).
//!
//! Every first game of a session is recorded for centurion-calibrate --field in
//! ~/.local/state/centurion/game-sessions.jsonl (start, end, game,
//! preset and its values, A/B turn). The game's name comes from CENTURION_GAME,
//! GAME_NAME (Lutris), SteamAppId, else the wrapped program's file name.

/// stderr logging that never panics: eprintln! aborts the process on EPIPE
/// (a launcher that stopped reading), which could cut POST off before it had
/// restored anything.
macro_rules! log {
    ($($t:tt)*) => {{ use std::io::Write as _; let _ = writeln!(std::io::stderr(), $($t)*); }};
}

use centurion_helpers::{lighting, tune};
use serde_json::{json, Value};
use std::io::Write;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicI32, Ordering};

const HELPER_DIR: &str = match option_env!("CENTURION_HELPER_DIR") { Some(d) => d, None => "/usr/libexec/centurion" };
const PKEXEC: &[&str] = &["/usr/bin/pkexec", "/bin/pkexec"];
const MAX_PRESET_BYTES: u64 = 256 * 1024;

/// Exact, case-sensitive profile name looked up in both curve tools.
const UNDERVOLT_PROFILE: &str = "GAMING";
/// RUN: how long to look for a PRE that has not taken the start lock yet.
const START_DETECT: std::time::Duration = std::time::Duration::from_secs(3);
/// RUN: upper bound for PRE's start sequence (scene, undervolt, preset). Only a live PRE holds the
/// start lock (flock goes with the process), and PRE itself is bounded by HELPER_TIMEOUT per helper,
/// so this is a safety net, not a guess at how long a start takes (90 s cut slow starts short).
const START_WAIT: std::time::Duration = std::time::Duration::from_secs(600);
/// RUN: once game mode is active, how long to wait for a preset's SMT-off to show up. The helper has
/// returned by then, so a still-active SMT means the knob was not applied (refused, not offered).
const SMT_WAIT: std::time::Duration = std::time::Duration::from_secs(3);
/// pkexec still waiting for polkit (real uid still ours, helper not started): cancelled after this.
const PKEXEC_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(60);
/// An authorized helper runs as root and can no longer be cancelled; it is working (CPU hot-plug, a
/// dGPU waking from D3cold, a game loading next to it) and changes the machine whether we wait or
/// not. Giving up on it at PKEXEC_TIMEOUT made PRE leave the game scene while the helper went on and
/// opened the game session: the game then ran on the pre-game scene.
const HELPER_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(180);
/// Marks the error of a helper that was still running when HELPER_TIMEOUT ran out.
const STILL_RUNNING: &str = "still running";
const UNDERVOLT_GAP: std::time::Duration = std::time::Duration::from_secs(2);
/// Same directory nvcurve-root-helper applies from.
const NVCURVE_PROFILES: &str = "/etc/nvcurve/profiles";

/// Everything this tool asks of root goes to tune-profile-helper (approved presets by name, restore, game
/// bookkeeping); only APPROVE talks to tune-helper.
fn helper() -> String { format!("{HELPER_DIR}/tune-profile-helper") }

const STORE_DIR: &str = "/etc/centurion/presets";

/// The approved (root-owned) copy of a preset's values. tune-profile-helper applies from it by name.
fn store_values(name: &str) -> Option<Value> {
    let v: Value = serde_json::from_str(&std::fs::read_to_string(format!("{STORE_DIR}/{name}.json")).ok()?).ok()?;
    v["values"].is_object().then(|| v["values"].clone())
}

fn xdg_config() -> PathBuf {
    std::env::var_os("XDG_CONFIG_HOME").map(PathBuf::from).filter(|p| p.is_absolute())
        .or_else(|| std::env::var_os("HOME").map(|h| Path::new(&h).join(".config")))
        .unwrap_or_else(|| PathBuf::from("/nonexistent"))
}
fn config_dir() -> PathBuf { xdg_config().join("centurion") }
/// QStandardPaths::GenericConfigLocation + the Ryzen tab's profile folder.
fn ryzen_profiles_dir() -> PathBuf { xdg_config().join("ryzen-curve-optimizer/profiles") }
fn presets_dir() -> PathBuf { config_dir().join("tune-presets") }

fn valid_name(n: &str) -> bool {
    n.chars().next().map_or(false, |c| c.is_alphanumeric()) && n.len() <= 64
        && n.chars().all(|c| c.is_alphanumeric() || " _-.".contains(c)) && !n.contains("..")
}

fn read_json(p: &Path) -> Option<Value> {
    let m = std::fs::metadata(p).ok()?;
    if !m.is_file() || m.len() > MAX_PRESET_BYTES { return None; }
    serde_json::from_str(&std::fs::read_to_string(p).ok()?).ok()
}

fn default_preset() -> Option<String> {
    read_json(&config_dir().join("tune.json"))?["game_preset"].as_str().filter(|n| valid_name(n)).map(str::to_owned)
}

fn load_preset(name: Option<&str>) -> Result<(String, Value), String> {
    let name = match name {
        Some(n) => n.to_owned(),
        None => default_preset().ok_or("no preset given and no game preset chosen in Centurion → Optimizations")?,
    };
    if !valid_name(&name) { return Err(format!("invalid preset name '{name}'")); }
    let p = presets_dir().join(format!("{name}.json"));
    let mut v = read_json(&p).ok_or_else(|| format!("cannot read preset {}", p.display()))?;
    if !v.is_object() { return Err(format!("preset '{name}' is not a JSON object")); }
    // Only the root-owned copy counts as the preset's values; the user's file may say anything.
    v["values"] = store_values(&name).ok_or_else(|| format!(
        "preset '{name}' is not approved — run: centurion-gamemode APPROVE '{name}' (or save it in the Optimizations tab)"))?;
    Ok((name, v))
}

fn preset_exists(name: &str) -> bool { valid_name(name) && presets_dir().join(format!("{name}.json")).is_file() }

fn pkexec(req: &Value) -> Result<Value, String> { pkexec_helper(&helper(), req) }

fn pkexec_helper(helper: &str, req: &Value) -> Result<Value, String> {
    let short = helper.rsplit('/').next().unwrap_or(helper);
    let pk = PKEXEC.iter().find(|p| Path::new(p).is_file()).ok_or("pkexec not found (install sys-auth/polkit)")?;
    let mut child = Command::new(pk).arg(helper)
        .stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped())
        .spawn().map_err(|e| format!("pkexec: {e}"))?;
    child.stdin.take().unwrap().write_all(req.to_string().as_bytes()).map_err(|e| format!("pkexec stdin: {e}"))?;
    // Game hooks run without a terminal and often without a polkit agent in
    // reach: never let a stuck authorization block the game launch forever.
    let pid = child.id() as libc::pid_t;
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || { let _ = tx.send(child.wait_with_output()); });
    let t0 = std::time::Instant::now();
    let out = loop {
        match rx.recv_timeout(std::time::Duration::from_millis(500)) {
            Ok(r) => break r.map_err(|e| format!("pkexec: {e}"))?,
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return Err(format!("pkexec: lost {short}")),
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
                let waited = t0.elapsed();
                if awaiting_authorization(pid) {
                    // pkexec keeps the caller's real uid until polkit says yes: it can still be signalled.
                    if waited >= PKEXEC_TIMEOUT {
                        unsafe { libc::kill(pid, libc::SIGTERM); }
                        return Err(format!("{short}: not authorized within {} s — skipped", PKEXEC_TIMEOUT.as_secs()));
                    }
                } else if waited >= HELPER_TIMEOUT {
                    return Err(format!("{short}: {STILL_RUNNING} after {} s — its result is unknown", HELPER_TIMEOUT.as_secs()));
                }
            }
        }
    };
    let text = String::from_utf8_lossy(&out.stdout);
    match text.lines().last().and_then(|l| serde_json::from_str::<Value>(l).ok()) {
        Some(v) => Ok(v),
        None => Err(match out.status.code() {
            Some(126) => "authorization dismissed".into(),
            Some(127) => format!("polkit did not authorize {short}: {}", String::from_utf8_lossy(&out.stderr).trim()),
            _ => format!("{short} failed: {}", String::from_utf8_lossy(&out.stderr).trim()),
        }),
    }
}

/// `pid` is still pkexec under our real uid (asking polkit), not the helper it execs as root.
fn awaiting_authorization(pid: libc::pid_t) -> bool {
    let Ok(s) = std::fs::read_to_string(format!("/proc/{pid}/status")) else { return false };
    let field = |k: &str| s.lines().find_map(|l| l.strip_prefix(k)).map(str::trim);
    field("Name:") == Some("pkexec")
        && field("Uid:").and_then(|u| u.split_whitespace().next()).and_then(|u| u.parse::<u32>().ok()) == Some(unsafe { libc::getuid() })
}

/// Prints helper messages and per-key failures to stderr; returns `ok`.
fn report(tag: &str, v: &Value) -> bool {
    let ok = v["ok"].as_bool().unwrap_or(false);
    if let Some(m) = v["message"].as_str() { log!("centurion-gamemode {tag}: {m}"); }
    for r in v["results"].as_array().into_iter().flatten() {
        if let Some(e) = r["error"].as_str() { log!("  {}: {e}", r["key"].as_str().unwrap_or("?")); }
        for e in r["errors"].as_array().into_iter().flatten() {
            log!("  {}: {}", e["key"].as_str().unwrap_or("?"), e["error"].as_str().unwrap_or(""));
        }
    }
    if !ok { log!("centurion-gamemode {tag}: {}", v["error"].as_str().unwrap_or("failed")); }
    ok
}

fn apply(name: Option<&str>, mode: &str) -> Result<bool, String> {
    // The request names the preset; tune-profile-helper takes the values from the approved store.
    let (name, _) = load_preset(name)?;
    let mut req = json!({"op": "apply_preset", "mode": mode, "preset": name, "soft_park": mode == "game"});
    if mode == "game" { req["owner_pid"] = json!(OWNER.load(Ordering::SeqCst)); }
    let v = pkexec(&req)?;
    if let Some(r) = v["parked"].as_str() {
        log!("centurion-gamemode PRE: CCD '{r}' parked for the game without hot-unplug (game kept off it, IRQs and kernel work moved onto it)");
    }
    Ok(report(if mode == "game" { "PRE" } else { "APPLY" }, &v))
}

/// POST: release game mode; when that was the last game, leave the game scene.
fn post() -> Result<bool, String> {
    // Same lock as the start sequence: a game started right after this one closed must not enter its
    // scene (and record `before_game`) while this POST is still switching back.
    let _start_lock = user_lock("gamemode-start.lock");
    let st = read_scene_state();
    // The field record's session ends with the last game (the one that leaves game mode).
    let end_session = || {
        let id = update_scene_state(|st| { let id = st["game_session"].as_str().map(str::to_owned); st["game_session"] = Value::Null; id });
        if let Some(id) = id { log_session(&json!({"id": id, "end": now_s()})); }
    };
    // The A launch's POST: the only game running, or its own launcher. The POST of a second game started
    // during an A launch used to take this path too and leave the scene under the A game.
    let owner = OWNER.load(Ordering::SeqCst) as i64;
    if st["ab_a"] == true && (game_refcount() == 0 || st["ab_owner"].is_null() || st["ab_owner"]["pid"].as_i64() == Some(owner)) {
        // An A launch opened no game session: nothing to release, only the game scene to leave.
        update_scene_state(|st| { st["ab_a"] = Value::Null; st["ab_owner"] = Value::Null; });
        // A game started during it keeps the scene until its own POST.
        if game_refcount() == 0 { end_session(); leave_game_scene(); }
        return Ok(true);
    }
    let v = pkexec(&json!({"op": "release", "owner_pid": owner}))?;
    let ok = report("POST", &v);
    // Also when the session was already gone (launcher pruned by an earlier helper call, GUI not
    // running to clean up): no live session left and a game scene still set means it is ours to leave.
    // A running A launch has no session to count, so it is checked on its own.
    if (v["restored"] == true || v["state"]["refcount"] == 0) && !ab_launch_live(&read_scene_state()) { end_session(); leave_game_scene(); }
    Ok(ok)
}

/// First PRE / WRAP of a session: the game scene (or the legacy "GAMING"
/// undervolt), then the game-mode preset. Returns whether the helper took the
/// game-mode reference (so POST must run) and the exit code.
fn game_start(name: Option<&str>, game: &str) -> (bool, i32) {
    // Two games launched at the same moment must not both see "first game"
    // and both switch scenes: serialise the start sequence per user.
    let _start_lock = user_lock("gamemode-start.lock");
    let cfg = read_json(&config_dir().join("tune.json")).unwrap_or(Value::Null);
    let st0 = read_scene_state();
    let first = game_refcount() == 0 && !ab_launch_live(&st0);
    // An A launch whose launcher died without POST: its flag no longer blocks "first game".
    if first && st0["ab_a"] == true { update_scene_state(|st| { st["ab_a"] = Value::Null; st["ab_owner"] = Value::Null; }); }
    // Field record (centurion-calibrate --field): which game ran with which values; with A/B on, every
    // other launch of a game runs at the boot defaults (A) under the same game scene.
    let ab = if first { ab_turn(&cfg, game) } else { None };
    if first {
        // The preset this launch really applies: without a name, apply() takes the GUI's game preset. Recording
        // null there made every such B launch look like an A (boot defaults) session to --field.
        let eff = name.map(str::to_owned).or_else(default_preset);
        let values = eff.as_deref().and_then(store_values).unwrap_or(Value::Null);
        let id = format!("{}-{}", now_s(), std::process::id());
        log_session(&json!({"id": id, "start": now_s(), "game": game, "preset": if ab == Some("A") { Value::Null } else { json!(eff) },
                            "values": if ab == Some("A") { json!({}) } else { values }, "ab": ab}));
        update_scene_state(|st| st["game_session"] = json!(id));
    }
    let paused = scenes_paused();
    if paused && cfg["game_scene"].as_str().is_some() { log!("centurion-gamemode: scenes are paused — game scene skipped"); }
    match cfg["game_scene"].as_str().filter(|n| valid_name(n) && !paused) {
        Some(scene) if first => enter_game_scene(scene, cfg["undervolt_cpu"] == true, cfg["undervolt_gpu"] == true),
        Some(_) => log!("centurion-gamemode: another game is running — its scene stays"),
        // Paused scenes with a game scene configured: nothing would undo the undervolt afterwards.
        None if first && !(paused && cfg["game_scene"].as_str().is_some()) => { undervolt(false); }
        None => {}
    }
    if ab == Some("A") {
        log!("centurion-gamemode: A/B comparison - this launch of {game} runs at the boot defaults (no game preset)");
        // No tune-helper session tracks an A launch: its launcher is recorded here instead, so the GUI
        // (gameSessions) and the next PRE see it as a running game — and as gone once the launcher is.
        let pid = OWNER.load(Ordering::SeqCst);
        let ab_owner = (pid > 1).then(|| centurion_helpers::proc_start_time(pid)).flatten()
            .map_or(Value::Null, |t| json!({"pid": pid, "start": t}));
        update_scene_state(|st| { st["ab_a"] = json!(true); st["ab_owner"] = ab_owner; });
        return (true, 0);
    }
    match apply(name, "game") {
        Ok(ok) => (true, (!ok) as i32),
        Err(e) if e.contains(STILL_RUNNING) => {
            // The helper may still open the session: the scene stays, POST (or the GUI's orphan
            // clean-up, if no session ever appears) leaves it. `true` so WRAP still runs POST.
            log!("centurion-gamemode: {e} — the game scene stays");
            (true, 1)
        }
        Err(e) => {
            log!("centurion-gamemode: {e}");
            // No game session exists, so nothing will ever call POST / release: do not leave the
            // game scene (Custom profile + its limits) behind with the machine believing a game runs.
            if first && cfg["game_scene"].as_str().is_some() && !paused {
                log!("centurion-gamemode: game mode could not start — leaving the game scene again");
                leave_game_scene();
            }
            (false, 1)
        }
    }
}

/// The A/B "A" launch recorded in scene.json is still running (its launcher lives; untracked = yes).
fn ab_launch_live(st: &Value) -> bool {
    st["ab_a"] == true && centurion_helpers::session_alive(st["ab_owner"]["pid"].as_i64(), st["ab_owner"]["start"].as_u64())
}

fn pre(name: Option<&str>) -> i32 { game_start(name, &game_name(None)).1 }

fn now_s() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) }

fn state_dir() -> PathBuf {
    std::env::var_os("XDG_STATE_HOME").map(PathBuf::from).filter(|p| p.is_absolute())
        .or_else(|| std::env::var_os("HOME").map(|h| Path::new(&h).join(".local/state")))
        .unwrap_or_else(|| PathBuf::from("/nonexistent")).join("centurion")
}

/// The game's name: the launcher's (Lutris exports GAME_NAME to its scripts), CENTURION_GAME, else the
/// wrapped program's file name (past wine / env / shell wrappers).
fn game_name(cmd: Option<&[String]>) -> String {
    for v in ["CENTURION_GAME", "GAME_NAME", "LUTRIS_GAME_NAME", "SteamAppId"] {
        if let Ok(x) = std::env::var(v) { let x = x.trim().to_owned(); if !x.is_empty() && x.len() <= 128 { return x; } }
    }
    let wrappers = ["wine", "wine64", "env", "sh", "bash", "proton", "gamemoderun", "mangohud", "prime-run", "umu-run", "taskset", "nice"];
    cmd.into_iter().flatten().filter(|a| !a.starts_with('-') && !a.contains('=')).map(|a| Path::new(a).file_name().map_or(a.clone(), |f| f.to_string_lossy().into_owned()))
        .find(|b| !wrappers.contains(&b.as_str())).unwrap_or_else(|| "unknown".into())
}

/// A/B turn of a game when the comparison is on (tune.json "field_ab": true for every game, or a
/// list of game names): launches alternate B (game preset), A (boot defaults), B, A, ...
fn ab_turn(cfg: &Value, game: &str) -> Option<&'static str> {
    let on = match &cfg["field_ab"] { Value::Bool(b) => *b, Value::Array(a) => a.iter().any(|g| g.as_str() == Some(game)), _ => false };
    if !on { return None; }
    let f = state_dir().join("ab.json");
    let mut st = read_json(&f).filter(Value::is_object).unwrap_or_else(|| json!({}));
    let n = st[game].as_u64().unwrap_or(0);
    st[game] = json!(n + 1);
    let _ = std::fs::create_dir_all(state_dir());
    let _ = std::fs::write(&f, st.to_string());
    Some(if n % 2 == 0 { "B" } else { "A" })
}

fn log_session(v: &Value) {
    let _ = std::fs::create_dir_all(state_dir());
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(state_dir().join("game-sessions.jsonl")) {
        let _ = writeln!(f, "{v}");
    }
}

// ── scenes ────────────────────────────────────────────────────────────────
// Same files and semantics as the GUI's Scenes tab (scenes.cpp). The shared
// runtime state lets the GUI and this tool agree on the active scene.

fn scenes_dir() -> PathBuf { config_dir().join("scenes") }

fn runtime_dir() -> PathBuf {
    std::env::var_os("XDG_RUNTIME_DIR").map(PathBuf::from).filter(|p| p.is_absolute())
        .unwrap_or_else(|| PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() })))
        .join("centurion")
}
fn scene_state_file() -> PathBuf { runtime_dir().join("scene.json") }
fn read_scene_state() -> Value { read_json(&scene_state_file()).filter(Value::is_object).unwrap_or_else(|| json!({})) }
fn write_scene_state(v: &Value) {
    let dir = runtime_dir();
    if std::fs::create_dir_all(&dir).is_err() { return; }
    let tmp = dir.join(format!("scene.json.{}.tmp", std::process::id()));
    if std::fs::write(&tmp, v.to_string()).is_ok() { let _ = std::fs::rename(&tmp, scene_state_file()); }
}
/// Read-modify-write of scene.json under scene.json.lock (the GUI takes the same lock): two writers
/// can no longer drop each other's fields. Never held across a helper call.
fn update_scene_state<R>(f: impl FnOnce(&mut Value) -> R) -> R {
    let _l = user_lock("scene.json.lock");
    let mut st = read_scene_state();
    let r = f(&mut st);
    write_scene_state(&st);
    r
}

/// Running game sessions, from tune-helper's world-readable state (sessions
/// whose launcher has died are not counted).
fn game_refcount() -> i64 {
    read_json(Path::new("/run/centurion/tune/state.json")).map_or(0, |v| centurion_helpers::live_game_sessions(&v))
}

/// Exclusive per-user lock in $XDG_RUNTIME_DIR, held until the file drops.
fn user_lock(name: &str) -> Option<std::fs::File> {
    use std::os::unix::io::AsRawFd;
    let dir = runtime_dir();
    std::fs::create_dir_all(&dir).ok()?;
    let f = std::fs::OpenOptions::new().create(true).write(true).open(dir.join(name)).ok()?;
    loop {
        if unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_EX) } == 0 { return Some(f); }
        if std::io::Error::last_os_error().kind() != std::io::ErrorKind::Interrupted { return None; }
    }
}

// ── game-session owner ──────────────────────────────────────────────────────
//
// tune-helper ends a game session on its own when the owner process is gone,
// so a launcher that was killed (or a POST hook that never ran) no longer
// leaves game mode stuck until reboot. WRAP owns its session itself; PRE/POST
// are short-lived hooks, so the owner is the launcher above them (the first
// ancestor that is not a shell or a small exec wrapper).

static OWNER: AtomicI32 = AtomicI32::new(0);

/// comm is cut to 15 bytes (TASK_COMM_LEN - 1): this tool itself reads back as "centurion-gamem".
const WRAPPER_COMMS: &[&str] = &["sh", "bash", "dash", "zsh", "fish", "ksh", "mksh", "busybox", "env", "timeout",
                                 "nice", "ionice", "stdbuf", "xargs", "setsid", "flock", "centurion-gamem"];

/// (comm, ppid) of `pid` from /proc/<pid>/stat.
fn proc_comm_ppid(pid: i32) -> Option<(String, i32)> {
    let s = std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    let (l, r) = (s.find('(')?, s.rfind(')')?);
    let ppid = s[r + 1..].split_whitespace().nth(1)?.parse().ok()?;
    Some((s[l + 1..r].to_owned(), ppid))
}

/// Interpreters whose scripts must count as wrappers too.
const SHELL_EXES: &[&str] = &["sh", "bash", "dash", "zsh", "fish", "ksh", "mksh", "busybox"];

/// A script started through its shebang (./start.sh, the usual Linux game launcher) has the
/// *script's file name* as comm ("start.sh"), so comm alone never matches "bash". The real
/// interpreter is in /proc/<pid>/exe. Without this check the game's own start.sh became the
/// session owner; when it exited (it often just spawns the game) the session was dropped,
/// the GUI saw "no game running" and left the game scene while the game was still up.
fn is_wrapper(pid: i32, comm: &str) -> bool {
    WRAPPER_COMMS.contains(&comm)
        || std::fs::read_link(format!("/proc/{pid}/exe")).ok()
            .and_then(|p| p.file_name().map(|n| n.to_string_lossy().into_owned()))
            .map_or(false, |n| SHELL_EXES.contains(&n.as_str()))
        || is_lutris_wrapper(comm, &std::fs::read(format!("/proc/{pid}/cmdline")).unwrap_or_default())
}

/// Lutris starts the pre-launch script (unless "Wait for pre-launch script completion" is set) and
/// the post-exit script through their own `lutris-wrapper`: a python process that exits as soon as
/// the hook and everything the hook left behind have exited. It is not a shell, so PRE took it for
/// the launcher and made it the session owner. It is also a child subreaper: tune-helper's detached
/// pressure guard (up to 2 min) is re-parented to it and kept it alive that long. So about two
/// minutes into the game the "owner" exited, the session counted as dead, the GUI left the game
/// scene and restored the game tuning under the running game, and automatic AC / battery switching
/// took over again. The owner must be Lutris itself (the wrapper's parent).
/// comm: "lutris-wrapper:" (setproctitle) or "lutris-wrapper" (shebang); without setproctitle and
/// started as `python3 …/lutris-wrapper` only the command line names it.
fn is_lutris_wrapper(comm: &str, cmdline: &[u8]) -> bool {
    comm.starts_with("lutris-wrapper")
        || cmdline.split(|b| *b == 0).take(2).any(|a| {
            let a = String::from_utf8_lossy(a);
            a.starts_with("lutris-wrapper") || a.rsplit('/').next() == Some("lutris-wrapper")
        })
}

fn launcher_pid() -> i32 {
    let mut pid = unsafe { libc::getppid() };
    for _ in 0..16 {
        if pid <= 1 { return 0; }
        let Some((comm, ppid)) = proc_comm_ppid(pid) else { return 0 };
        if comm == "systemd" || comm == "init" { return 0; }  // no launcher above us: untracked
        if !is_wrapper(pid, &comm) { return pid; }
        pid = ppid;
    }
    0
}

fn enter_game_scene(scene: &str, cpu: bool, gpu: bool) {
    // A scene that was deleted (tune.json still names it) must not become the "active" one.
    if read_json(&scenes_dir().join(format!("{scene}.json"))).is_none() {
        log!("centurion-gamemode: game scene \"{scene}\" not found ({}) — skipped", scenes_dir().display());
        return;
    }
    update_scene_state(|st| {
        // A game scene that is still set (its game ended without leaving it) keeps the scene recorded
        // before it: `active` is that game scene, not where the machine should return to.
        if st.get("game_scene").map_or(true, Value::is_null) {
            st["before_game"] = st.get("active").cloned().unwrap_or(Value::Null);
        }
        st["game_scene"] = json!(scene);
        // Active from the first step on: the power profile is the first thing the scene changes, so a
        // PRE that is cut short (hook timeout, launcher killed) used to leave the machine on Custom
        // while the state still named the old scene — the GUI neither knew nor reported the switch.
        st["active"] = json!(scene);
    });
    let ok = apply_scene(scene, SceneParts { cpu, gpu, tuning: false });  // parts that failed were reported
    if !ok { log!("centurion-gamemode: game scene \"{scene}\" was applied only partly — see the lines above"); }
}

fn scenes_paused() -> bool {
    read_json(&config_dir().join("scenes.json")).map_or(false, |v| v["paused"] == true)
}

fn leave_game_scene() {
    let paused = scenes_paused();
    // Claim the switch back under the lock: the GUI's orphan clean-up does the same, so only one of the
    // two leaves the scene.
    let before = update_scene_state(|st| {
        if st.get("game_scene").map_or(true, Value::is_null) { return None; }
        let before = st["before_game"].as_str().map(str::to_owned);
        st["game_scene"] = Value::Null;
        st["before_game"] = Value::Null;
        Some(before)
    });
    let Some(before) = before else { return };
    if paused {
        log!("centurion-gamemode: last game closed — scenes are paused, no scene change");
        return;
    }
    let auto = read_json(&config_dir().join("scenes.json")).unwrap_or(Value::Null);
    let target = if auto["auto"] == true {
        let key = if on_ac_settled().unwrap_or(true) { "on_ac" } else { "on_battery" };
        auto[key].as_str().filter(|n| valid_name(n)).map(str::to_owned)
    } else {
        before.filter(|n| valid_name(n))
    };
    match target {
        Some(t) => {
            log!("centurion-gamemode: last game closed — back to scene \"{t}\"");
            apply_scene(&t, SceneParts { cpu: true, gpu: true, tuning: true });
            update_scene_state(|st| st["active"] = json!(t));
        }
        None => log!("centurion-gamemode: last game closed — no earlier scene to return to (the game scene stays)"),
    }
}

use centurion_helpers::power::on_ac_settled;

#[derive(Clone, Copy)]
struct SceneParts { cpu: bool, gpu: bool, tuning: bool }

// Firmware attributes the kernel rejects through sysfs, written by the GPU
// helper over WMI instead (same table as fwattrtab.cpp).
const WMI_KNOBS: &[(&str, &str)] = &[("gpu_nv_ctgp", "ctgp"), ("gpu_nv_ppab", "boost_up"), ("gpu_nv_cpu_boost", "boost_down")];

/// The handler the GUI uses (platformprofile.cpp primaryHandler): Custom-capable first, then the
/// richer choice list, then the lowest node. Alphabetical order alone picked e.g. an amd-pmf handler
/// on machines that have two, so profiles were written to (and read from) the wrong one.
fn platform_profile_node() -> Option<String> {
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

/// Port of the GUI's nvidiaUsable() (scenes.cpp): the NVIDIA dGPU is on the bus with a driver bound
/// and the driver has actually initialised it. In iGPU-only mode / after a firmware power cut every
/// NVIDIA or WMI-GPU call can hang in the kernel (the helper sits in D state until pkexec's timeout),
/// so those steps are skipped instead. Directory listing only: no GPU wake-up.
fn nvidia_usable() -> bool {
    let rd = |p: &Path, f: &str| std::fs::read_to_string(p.join(f)).map(|s| s.trim().to_owned()).unwrap_or_default();
    let on_bus = std::fs::read_dir("/sys/bus/pci/devices").into_iter().flatten().flatten().map(|e| e.path()).any(|p| {
        rd(&p, "vendor") == "0x10de" && rd(&p, "class").starts_with("0x03") && p.join("driver").exists()
            && rd(&p, "power/runtime_status") != "error"
    });
    on_bus && std::fs::read_dir("/proc/driver/nvidia/gpus").map_or(false, |mut d| d.next().is_some())
}
const GPU_OFF: &str = "skipped (NVIDIA dGPU is off)";

fn current_platform_profile() -> Option<String> {
    let p = match platform_profile_node() {
        Some(n) => PathBuf::from(format!("/sys/class/platform-profile/{n}/profile")),
        None => PathBuf::from("/sys/firmware/acpi/platform_profile"),
    };
    std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned())
}

fn scene_choice(v: &Value) -> Option<Result<String, ()>> {
    if v["reset"] == true { return Some(Err(())); }
    v["profile"].as_str().filter(|n| valid_name(n)).map(|n| Ok(n.to_owned()))
}

/// Applies a saved scene in dependency order, printing one line per part.
/// A failing part is reported and the rest still runs. Returns overall ok.
fn apply_scene(name: &str, parts: SceneParts) -> bool {
    if !valid_name(name) { log!("centurion-gamemode: invalid scene name '{name}'"); return false; }
    let Some(s) = read_json(&scenes_dir().join(format!("{name}.json"))) else {
        log!("centurion-gamemode: scene \"{name}\" not found ({})", scenes_dir().display());
        return false;
    };
    let mut ok = true;
    let mut line = |what: &str, r: Result<String, String>| match r {
        Ok(m) => log!("centurion-gamemode: scene {name}: {what}: {m}"),
        Err(e) => { ok = false; log!("centurion-gamemode: scene {name}: ✗ {what}: {e}"); }
    };

    // 0. EC fan boost (Full Speed flag) FIRST: switching it off before the profile change means
    //    the fans never spin up for the new profile and then drop again.
    let fan_want = s["fan_fullspeed"].as_bool();
    if let Some(on) = fan_want {
        let v = pkexec_helper(&format!("{HELPER_DIR}/legion-profile-helper"),
                              &json!({"device": "fan_fullspeed", "value": if on { "1" } else { "0" }}))
            .unwrap_or_else(|e| json!({"ok": false, "error": e}));
        line("Fan boost", if v["ok"] == true { Ok(if on { "turbo".into() } else { "auto".into() }) }
                          else { Err(v["error"].as_str().unwrap_or("failed").to_owned()) });
    }

    // 1. power profile (firmware limits below need Custom)
    if let Some(p) = s["platform_profile"].as_str().filter(|p| !p.is_empty() && p.len() <= 32) {
        let r = pkexec_helper(&format!("{HELPER_DIR}/legion-profile-helper"),
                              &json!({"profile": p, "handler": platform_profile_node()}))
            .and_then(|v| if v["ok"] == true { Ok(v["effective"].as_str().unwrap_or(p).to_owned()) }
                          else { Err(v["error"].as_str().unwrap_or("failed").to_owned()) });
        line("power profile", r);
    }

    // The EC may reset the flag when the profile changes: re-assert only if it no longer matches.
    if let (Some(on), Some(_)) = (fan_want, s["platform_profile"].as_str().filter(|p| !p.is_empty())) {
        let h = format!("{HELPER_DIR}/legion-profile-helper");
        if let Ok(v) = pkexec_helper(&h, &json!({"fan_fullspeed": "get"})) {
            if v["ok"] == true && v["on"].as_bool() != Some(on) {
                let r = pkexec_helper(&h, &json!({"device": "fan_fullspeed", "value": if on { "1" } else { "0" }}))
                    .and_then(|v| if v["ok"] == true { Ok("re-applied after the profile change".to_owned()) } else { Err("failed".to_owned()) });
                line("Fan boost", r);
            }
        }
    }

    // 2. firmware limits
    if let Some(fw) = s["firmware"].as_object().filter(|m| !m.is_empty()) {
        let r = (|| -> Result<String, String> {
            if current_platform_profile().as_deref() != Some("custom") {
                return Err("needs the Custom power profile (set it in this scene)".into());
            }
            let (mut batch, mut wmi, mut unknown) = (Vec::new(), serde_json::Map::new(), 0);
            for (attr, val) in fw {
                let Some(v) = val.as_i64() else { unknown += 1; continue };
                if let Some((_, key)) = WMI_KNOBS.iter().find(|(a, _)| a == attr) { wmi.insert((*key).into(), json!(v)); continue; }
                let dir = std::fs::read_dir("/sys/class/firmware-attributes").into_iter().flatten().flatten()
                    .map(|d| d.path().join("attributes").join(attr)).find(|d| d.join("current_value").is_file());
                let Some(dir) = dir else { unknown += 1; continue };
                let num = |f: &str| std::fs::read_to_string(dir.join(f)).ok().and_then(|x| x.trim().parse::<i64>().ok());
                if num("max_value").unwrap_or(0) <= num("min_value").unwrap_or(0) { unknown += 1; continue; }
                batch.push(json!({"path": dir.join("current_value"), "value": v}));
            }
            let note = if unknown > 0 { format!(" ({unknown} not present here)") } else { String::new() };
            if !batch.is_empty() {
                let v = pkexec_helper(&format!("{HELPER_DIR}/fwattr-helper"), &Value::Array(batch.clone()))?;
                if v["ok"] != true {
                    let bad: Vec<String> = v["results"].as_array().into_iter().flatten()
                        .filter(|x| x["ok"] != true).filter_map(|x| x["error"].as_str().map(str::to_owned)).collect();
                    return Err(if bad.is_empty() { v["error"].as_str().unwrap_or("failed").into() } else { bad.join("; ") });
                }
            }
            if !wmi.is_empty() && !nvidia_usable() {
                return Ok(format!("{} value(s); {} GPU value(s) skipped (NVIDIA dGPU is off){note}", batch.len(), wmi.len()));
            }
            if !wmi.is_empty() {
                let v = pkexec_helper(&format!("{HELPER_DIR}/legion-gpu-helper"), &json!({"op": "apply", "values": wmi}))?;
                if v["ok"] != true { return Err(format!("GPU (WMI): {}", v["error"].as_str().unwrap_or("failed"))); }
            }
            Ok(format!("{} value(s){}{note}", batch.len(),
                       if wmi.is_empty() { String::new() } else { format!(" + {} GPU (WMI)", wmi.len()) }))
        })();
        line("firmware limits", r);
    }

    // 3. CPU curve, 4. GPU curve — CPU first, GPU a moment later, as for undervolt
    let mut did_cpu = false;
    if parts.cpu {
        if let Some(c) = scene_choice(&s["cpu_curve"]) {
            did_cpu = true;
            let r = match (tune::cpu_vendor(), c) {
                (tune::Vendor::Intel, Err(())) => pkexec_helper(&format!("{HELPER_DIR}/intel-uv-helper"), &json!({"op": "reset", "hold": true}))
                    .and_then(|v| if v["ok"] == true { Ok("reset (0 mV)".into()) } else { Err(v["error"].as_str().unwrap_or("failed").into()) }),
                (tune::Vendor::Intel, Ok(n)) => apply_intel(&config_dir().join(format!("intel-uv-profiles/{n}.json")), true).map(|_| format!("\"{n}\"")),
                (tune::Vendor::Amd, Err(())) => pkexec_helper(&format!("{HELPER_DIR}/ryzen-co-helper"), &json!({"op": "reset"}))
                    .and_then(|v| if v["ok"] == true { Ok("reset (0)".into()) } else { Err(v["error"].as_str().unwrap_or("failed").into()) }),
                (tune::Vendor::Amd, Ok(n)) => apply_ryzen(&ryzen_profiles_dir().join(format!("{n}.json"))).map(|_| format!("\"{n}\"")),
                _ => Err("no CPU curve backend for this CPU".into()),
            };
            line("CPU curve", r);
        }
    }
    if parts.gpu {
        if let Some(c) = scene_choice(&s["gpu_curve"]) {
            let usable = nvidia_usable();
            if did_cpu && usable { std::thread::sleep(UNDERVOLT_GAP); }
            let r = match c {
                _ if !usable => Ok(GPU_OFF.into()),
                Err(()) => pkexec_helper(&format!("{HELPER_DIR}/nvcurve-root-helper"), &json!({"op": "reset_gpu_curve"}))
                    .and_then(|v| if v["ok"] == true { Ok("reset".into()) } else { Err(v["error"].as_str().unwrap_or("failed").into()) }),
                Ok(n) => apply_nvidia_named(&n).map(|_| format!("\"{n}\"")),
            };
            line("GPU curve", r);
        }
    }

    // 5. Optimizations preset — switched, not stacked ("replace"); a preset
    //    is applied by name from the approved store (tune-profile-helper).
    if parts.tuning {
        if let Some(c) = scene_choice(&s["tuning"]) {
            let r = (|| -> Result<String, String> {
                let preset = match &c { Err(()) => Value::Null, Ok(n) => json!(n) };
                let v = pkexec(&json!({"op": "apply_preset", "mode": "manual", "replace": true, "preset": preset}))?;
                if v["game_active"] == true { return Ok("left alone (game session active)".into()); }
                if v["ok"] != true { return Err(v["error"].as_str().unwrap_or("failed").into()); }
                Ok(match c { Err(()) => "originals restored".into(), Ok(n) => format!("\"{n}\"") })
            })();
            line("Optimizations", r);
        }
    }

    // 5a. Custom-mode fan curve: only while the Custom profile is active (the EC follows the table there only).
    if let Some(lv) = s["fan_table"].as_array().filter(|a| a.len() == 10 && a.iter().all(|x| x.as_u64().map_or(false, |n| (1..=10).contains(&n)))) {
        if current_platform_profile().as_deref() != Some("custom") {
            line("Fan curve", Ok("skipped (not the Custom power profile)".into()));
        } else {
            let v = pkexec_helper(&format!("{HELPER_DIR}/legion-profile-helper"), &json!({"fan_table": "set", "levels": lv}))
                .unwrap_or_else(|e| json!({"ok": false, "error": e}));
            line("Fan curve", if v["ok"] == true { Ok("table written".into()) } else { Err(v["error"].as_str().unwrap_or("failed").to_owned()) });
        }
    }

    // 6. keyboard lighting — in-process as the user (udev uaccess on the
    //    hidraw node); lighting-helper through pkexec only if that is denied.
    // A machine without the keyboard skips the step (as the GUI does); it is not a failed scene.
    let light = lighting::scene_request(&s["lighting"]);
    if light.is_some() && !centurion_spectrum::present() {
        line("lighting", Ok("no Spectrum keyboard here; skipped".into()));
    } else if let Some(req) = light {
        let mut v = lighting::handle(&req);
        if v["denied"] == true {
            v = pkexec_helper(&format!("{HELPER_DIR}/lighting-helper"), &req)
                .unwrap_or_else(|e| json!({"ok": false, "error": e}));
        }
        line("lighting", if v["ok"] == true {
            Ok(format!("profile {} · brightness {}", v["profile"], v["brightness"]))
        } else {
            Err(v["error"].as_str().unwrap_or("failed").to_owned())
        });
    }

    // 7. user command — as the user, no shell, detached
    if let Some(cmd) = s["command"].as_str().map(str::trim).filter(|c| !c.is_empty()) {
        // Same quoting rules as the GUI (QProcess::splitCommand).
        let argv = split_command(cmd);
        if argv.is_empty() { return ok; }
        let mut c = Command::new(&argv[0]);
        c.args(&argv[1..]).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
        unsafe { c.pre_exec(|| { libc::setsid(); Ok(()) }); }
        line("command", c.spawn().map(|_| argv[0].clone()).map_err(|e| format!("{}: {e}", argv[0])));
    }
    ok
}

// ── undervolt ("GAMING" curve presets) ──────────────────────────────────────

/// Ryzen Curve Optimizer profile → ryzen-co-helper, the same order the GUI
/// uses: all-core offset first, per-core offsets only after it succeeded.
fn apply_ryzen(path: &Path) -> Result<(), String> {
    let p = read_json(path).ok_or_else(|| format!("cannot read {}", path.display()))?;
    let helper = format!("{HELPER_DIR}/ryzen-co-helper");
    let call = |op: &str, params: Value| -> Result<(), String> {
        let v = pkexec_helper(&helper, &json!({"op": op, "params": params}))?;
        for r in v["results"].as_array().into_iter().flatten().filter(|r| r["ok"] != true) {
            log!("  CCD{}/S{}: {}", r["ccd"], r["core"], r["message"].as_str().unwrap_or("failed"));
        }
        if v["ok"] == true { Ok(()) } else {
            Err(v["error"].as_str().or(v["message"].as_str()).unwrap_or("failed").to_owned())
        }
    };
    // Slots on a CCD that is not there (profile from another topology) are skipped, like the GUI does.
    // Counted like the GUI (a parked CCD still has its SMU slots): with CCD1 parked its per-core
    // offsets used to be dropped here while the GUI's scene apply sent them.
    let ccds = tune::ccd_count() as i64;
    let entries: Vec<Value> = p["cores"].as_array().into_iter().flatten()
        .filter(|c| c["disabled"] != true && c["coper"].is_i64() && c["ccx"].as_i64().unwrap_or(0) == 0)
        .filter_map(|c| {
            let ccd = c["ccd"].as_i64()?;
            let core = c["slot"].as_i64().or_else(|| c["core"].as_i64())?;
            (ccds == 0 || ccd < ccds).then(|| json!({"ccd": ccd, "ccx": 0, "core": core, "coper": c["coper"]}))
        })
        .collect();
    let coall = p["coall"].as_i64();
    if coall.is_none() && entries.is_empty() { return Err("profile contains no offsets".into()); }
    if let Some(v) = coall { call("set_coall", json!({"value": v}))?; }
    if !entries.is_empty() { call("set_coper_batch", json!({"entries": entries}))?; }
    Ok(())
}

/// Intel undervolt profile (Intel Undervolt tab format) → intel-uv-helper.
/// `hold` (scenes): the helper also records the profile so centurion-intel-uv-daemon keeps *it* alive
/// instead of writing its own boot profile's limits over it at the next interval.
fn apply_intel(path: &Path, hold: bool) -> Result<(), String> {
    let p = read_json(path).ok_or_else(|| format!("cannot read {}", path.display()))?;
    let v = pkexec_helper(&format!("{HELPER_DIR}/intel-uv-helper"), &json!({"op": "apply", "profile": p, "hold": hold}))?;
    for r in v["results"].as_array().into_iter().flatten().filter(|r| r["ok"] != true) {
        log!("  {}: {}", r["what"].as_str().unwrap_or("?"), r["message"].as_str().unwrap_or("failed"));
    }
    if v["ok"] == true { Ok(()) } else { Err(v["error"].as_str().unwrap_or("some values failed").to_owned()) }
}

fn apply_nvidia() -> Result<(), String> { apply_nvidia_named(UNDERVOLT_PROFILE) }

fn apply_nvidia_named(name: &str) -> Result<(), String> {
    let v = pkexec_helper(&format!("{HELPER_DIR}/nvcurve-root-helper"),
        &json!({"op": "apply_named_profile", "name": name}))?;
    if v["ok"] == true { Ok(()) } else { Err(v["error"].as_str().unwrap_or("failed").to_owned()) }
}

/// Applies the "GAMING" curve presets enabled in tune.json: CPU first, then
/// GPU UNDERVOLT_GAP later. `forced` (the UNDERVOLT verb) ignores the switches.
fn undervolt(forced: bool) -> bool {
    let cfg = read_json(&config_dir().join("tune.json")).unwrap_or(Value::Null);
    let want_cpu = forced || cfg["undervolt_cpu"] == true;
    let want_gpu = forced || cfg["undervolt_gpu"] == true;
    let ryzen = ryzen_profiles_dir().join(format!("{UNDERVOLT_PROFILE}.json"));
    let nvidia = Path::new(NVCURVE_PROFILES).join(format!("{UNDERVOLT_PROFILE}.json"));

    let mut steps: Vec<(&str, Box<dyn Fn() -> Result<(), String>>)> = Vec::new();
    let intel = xdg_config().join(format!("centurion/intel-uv-profiles/{UNDERVOLT_PROFILE}.json"));
    if want_cpu && tune::cpu_vendor() == tune::Vendor::Intel {
        if intel.is_file() { let r = intel.clone(); steps.push(("CPU", Box::new(move || apply_intel(&r, false)))); }
        else { log!("centurion-gamemode: undervolt CPU skipped — no Intel undervolt profile \"{UNDERVOLT_PROFILE}\" ({})", intel.display()); }
    } else if want_cpu && tune::cpu_vendor() != tune::Vendor::Amd {
        log!("centurion-gamemode: undervolt CPU skipped — no CPU undervolt backend for this vendor");
    } else if want_cpu {
        if ryzen.is_file() { let r = ryzen.clone(); steps.push(("CPU", Box::new(move || apply_ryzen(&r)))); }
        else { log!("centurion-gamemode: undervolt CPU skipped — no Ryzen profile \"{UNDERVOLT_PROFILE}\" ({})", ryzen.display()); }
    }
    if want_gpu && !nvidia_usable() {
        log!("centurion-gamemode: undervolt GPU {GPU_OFF}");
    } else if want_gpu {
        if nvidia.is_file() { steps.push(("GPU", Box::new(apply_nvidia))); }
        else { log!("centurion-gamemode: undervolt GPU skipped — no NVIDIA profile \"{UNDERVOLT_PROFILE}\" ({})", nvidia.display()); }
    }
    let mut all_ok = true;
    for (i, (what, step)) in steps.iter().enumerate() {
        if i > 0 { std::thread::sleep(UNDERVOLT_GAP); }
        match step() {
            Ok(()) => log!("centurion-gamemode: undervolt {what}: \"{UNDERVOLT_PROFILE}\" applied"),
            Err(e) => { all_ok = false; log!("centurion-gamemode: undervolt {what} failed: {e}"); }
        }
    }
    all_ok && (!forced || !steps.is_empty())
}

// ── CCD park in game mode ─────────────────────────────────────────────────
// A hot-unplugged CCD leaves a hole in the CPU numbers (9955HX3D with CCD1
// parked: online 0-7,16-23). Wine takes the online *count* as its CPU count
// and maps logical CPU i to host CPU i (system affinity mask 0-15), while its
// processor info lists 0-7,16-23: every per-core thread pin of a game hits
// an offline or out-of-mask CPU, and games that check it do not start.
// nvidia-powerd dies on the missing CPUs too. Game mode therefore empties the
// CCD instead of taking it offline: the game is pinned to the other CCD(s),
// IRQs and unbound kernel work are moved onto the parked one. The
// Optimizations tab's manual Apply still hot-unplugs.

/// Turns a game preset's `cpu.ccd_park` into the soft form; returns the role.
/// A CCD that is already offline (parked by hand) is left as it is.
fn soft_park(preset: &mut Value) -> Option<String> {
    tune::soft_park_values(preset["values"].as_object_mut()?)
}

/// Proton's WINE_CPU_TOPOLOGY for this process's CPU set: logical CPU i ->
/// host CPU, SMT siblings as adjacent pairs ("Ns:a,b,…"). None when Wine's
/// own view (host 0..online-1) is already right. Proton rejects host ids
/// >= the online count, so above a hole only the ids below it are mapped.
fn wine_topology() -> Option<String> {
    let n = unsafe { libc::sysconf(libc::_SC_NPROCESSORS_ONLN) };
    if n < 1 { return None; }
    let n = n as usize;
    let mut usable = Vec::new();
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        if libc::sched_getaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &mut set) != 0 { return None; }
        for c in 0..n { if libc::CPU_ISSET(c, &set) { usable.push(c); } }
    }
    if usable.is_empty() || usable.len() == n { return None; }
    let siblings = |c: usize| std::fs::read_to_string(format!("/sys/devices/system/cpu/cpu{c}/topology/thread_siblings_list"))
        .map(|s| tune::cpu_list(s.trim())).unwrap_or_default();
    let mut order: Vec<usize> = Vec::new();
    let mut pairs = true;
    for &c in &usable {
        if order.contains(&c) { continue; }
        let s: Vec<usize> = siblings(c).into_iter().filter(|x| usable.contains(x)).collect();
        if s.len() != 2 { pairs = false; break; }
        order.extend(s);
    }
    let list = |v: &[usize]| v.iter().map(usize::to_string).collect::<Vec<_>>().join(",");
    Some(if pairs { format!("{}s:{}", order.len() / 2, list(&order)) } else { format!("{}:{}", usable.len(), list(&usable)) })
}

// ── launch boost ──────────────────────────────────────────────────────────

fn set_affinity(cpus: &[usize]) -> std::io::Result<()> {
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        for &c in cpus { if c < libc::CPU_SETSIZE as usize { libc::CPU_SET(c, &mut set); } }
        if libc::sched_setaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &set) != 0 {
            return Err(std::io::Error::last_os_error());
        }
    }
    Ok(())
}

/// Applies the preset's `run` block to this process; the game inherits it.
/// `parked`: soft-parked CCD role, the game is kept off it when the preset
/// pins nothing itself.
fn prepare_run(preset: &Value, parked: Option<&str>) {
    let run = &preset["run"];
    let nice = run["nice"].as_i64().unwrap_or(0);
    if (-20..=-1).contains(&nice) {
        match pkexec(&json!({"op": "boost", "nice": nice, "autogroup": run["autogroup"].as_bool().unwrap_or(true)})) {
            Ok(v) if v["ok"] == true => {}
            Ok(v) => log!("centurion-gamemode: nice boost failed: {}", v["error"].as_str().unwrap_or("?")),
            Err(e) => log!("centurion-gamemode: nice boost failed: {e}"),
        }
    }
    // Soft park: the game gets the other CCD(s) to itself through a cgroup
    // partition (everything else is moved off them); pinning below stays as
    // the fallback when the partition is not available.
    if let Some(pr) = parked {
        match pkexec(&json!({"op": "isolate_join", "park": pr})) {
            Ok(v) if v["ok"] == true => log!("centurion-gamemode: game CPU partition {} (rest of the system moved off it)", v["cpus"].as_str().unwrap_or("?")),
            Ok(v) => log!("centurion-gamemode: game CPU partition not used: {}", v["error"].as_str().unwrap_or("?")),
            Err(e) => log!("centurion-gamemode: game CPU partition not used: {e}"),
        }
    }
    let role = run["affinity"].as_str().unwrap_or("none");
    if role != "none" {
        match tune::resolve_ccd(&tune::ccx_groups(), role) {
            Some(g) => match set_affinity(&g.cpus) {
                Ok(()) if role == "pcore" || role == "ecore" =>
                    log!("centurion-gamemode: pinned to {} ({})", if role == "pcore" { "P-cores" } else { "E-cores" }, tune::fmt_cpu_list(&g.cpus)),
                Ok(()) => log!("centurion-gamemode: pinned to CCD{} ({}, {} MiB L3)", g.index, tune::fmt_cpu_list(&g.cpus), g.l3_kib / 1024),
                Err(e) => log!("centurion-gamemode: sched_setaffinity: {e}"),
            },
            None => log!("centurion-gamemode: affinity '{role}' does not apply to this CPU, skipped"),
        }
    } else if let Some(pr) = parked {
        let groups = tune::ccx_groups();
        if let Some(pg) = tune::resolve_ccd(&groups, pr) {
            let rest: Vec<usize> = groups.iter().flat_map(|g| g.cpus.iter().copied()).filter(|c| !pg.cpus.contains(c)).collect();
            match set_affinity(&rest) {
                Ok(()) => log!("centurion-gamemode: pinned off the parked CCD ({})", tune::fmt_cpu_list(&rest)),
                Err(e) => log!("centurion-gamemode: sched_setaffinity: {e}"),
            }
        }
    }
    // Timer slack of the game's process tree (inherited over fork and exec). The default of 50 µs lets
    // the kernel deliver every sleep and wait time-out of a normal thread that much late; 1 ns = on time.
    if let Some(ns) = run["timerslack_ns"].as_u64().filter(|n| (1..=50_000).contains(n)) {
        if unsafe { libc::prctl(libc::PR_SET_TIMERSLACK, ns as libc::c_ulong) } == 0 { log!("centurion-gamemode: timer slack {ns} ns"); }
        else { log!("centurion-gamemode: timer slack: {}", std::io::Error::last_os_error()); }
    }
    // Wine/Proton must see exactly the CPUs the game may use (see soft_park).
    if std::env::var_os("WINE_CPU_TOPOLOGY").is_none() {
        if let Some(t) = wine_topology() {
            std::env::set_var("WINE_CPU_TOPOLOGY", &t);
            log!("centurion-gamemode: WINE_CPU_TOPOLOGY={t}");
        }
    }
}

/// [preset] [--] cmd… ; a first word that is not a saved preset is the command.
fn split_cmd(args: &[String]) -> (Option<String>, Vec<String>) {
    let mut i = 0;
    let mut name = None;
    if let Some(a) = args.first() {
        if a != "--" && preset_exists(a) { name = Some(a.clone()); i = 1; }
    }
    if args.get(i).map(String::as_str) == Some("--") { i += 1; }
    (name, args[i..].to_vec())
}

/// Port of QProcess::splitCommand: whitespace separates, double quotes group,
/// and three consecutive quotes give one literal quote. The GUI starts scene
/// commands with the Qt function, so both must split identically.
fn split_command(cmd: &str) -> Vec<String> {
    let (mut args, mut tmp) = (Vec::new(), String::new());
    let (mut quotes, mut in_quote) = (0, false);
    for c in cmd.chars() {
        if c == '"' {
            quotes += 1;
            if quotes == 3 { quotes = 0; tmp.push(c); }
            continue;
        }
        if quotes > 0 {
            if quotes == 1 { in_quote = !in_quote; }
            quotes = 0;
        }
        if !in_quote && c.is_whitespace() {
            if !tmp.is_empty() { args.push(std::mem::take(&mut tmp)); }
        } else {
            tmp.push(c);
        }
    }
    if !tmp.is_empty() { args.push(tmp); }
    args
}

static CHILD: AtomicI32 = AtomicI32::new(0);
extern "C" fn forward(sig: libc::c_int) {
    let pid = CHILD.load(Ordering::SeqCst);
    if pid > 0 { unsafe { libc::kill(pid, sig) }; }
}

fn wrap(args: &[String]) -> i32 {
    OWNER.store(std::process::id() as i32, Ordering::SeqCst);
    let (name, cmd) = split_cmd(args);
    if cmd.is_empty() { log!("centurion-gamemode WRAP: no command"); return 2; }
    let (pname, mut preset) = match load_preset(name.as_deref()) { Ok(p) => p, Err(e) => { log!("centurion-gamemode: {e}"); return 2 } };
    let parked = soft_park(&mut preset);
    // The refcount is taken as soon as the helper was reached, even if a knob failed.
    let (entered, _) = game_start(Some(&pname), &game_name(Some(&cmd)));
    // An A launch of the A/B comparison runs exactly at the boot state: no launch boost either.
    if read_scene_state()["ab_a"] != true { prepare_run(&preset, parked.as_deref()); }
    // Forward termination so POST still runs when the launcher stops us.
    // `as *const ()` first: casting a function item straight to an integer type is
    // deprecated (function pointers aren't guaranteed integer-representable), even
    // though it's always fine in practice on the platforms this runs on.
    // Signals are held back until the child's pid is known: one that arrived
    // in between used to be dropped (nothing to forward to yet). The child
    // itself starts with an empty mask (std resets it before exec).
    let mut held: libc::sigset_t = unsafe { std::mem::zeroed() };
    unsafe {
        libc::sigemptyset(&mut held);
        for s in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP] { libc::sigaddset(&mut held, s); }
        libc::sigprocmask(libc::SIG_BLOCK, &held, std::ptr::null_mut());
    }
    for s in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP] { unsafe { libc::signal(s, forward as *const () as libc::sighandler_t) }; }
    let spawned = Command::new(&cmd[0]).args(&cmd[1..]).spawn();
    if let Ok(c) = &spawned { CHILD.store(c.id() as i32, Ordering::SeqCst); }
    unsafe { libc::sigprocmask(libc::SIG_UNBLOCK, &held, std::ptr::null_mut()); }
    let code = match spawned {
        Ok(mut c) => {
            loop {
                match c.wait() {
                    Ok(st) => break st.code().unwrap_or(1),
                    Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                    Err(_) => break 1,
                }
            }
        }
        Err(e) => { log!("centurion-gamemode: {}: {e}", cmd[0]); 127 }
    };
    if entered { if let Err(e) = post() { log!("centurion-gamemode: {e}"); } }
    code
}

/// Lutris runs the pre-launch script (PRE) and the command prefix (RUN) in
/// parallel unless "Wait for pre-launch script completion" is set. RUN must
/// not start the game while PRE is still working: the game scene / "GAMING"
/// undervolt (CPU CO, then the NVIDIA curve reset + write + read-back) would
/// otherwise run while the game is bringing the dGPU up, and a preset that
/// hot-plugs CPUs (SMT off, CCD park) must finish before RUN reads the CCD
/// topology. PRE holds gamemode-start.lock for its whole start sequence, so
/// RUN waits until that lock is free and game mode is active.
fn start_lock_held() -> bool {
    use std::os::unix::io::AsRawFd;
    let Ok(f) = std::fs::OpenOptions::new().write(true).open(runtime_dir().join("gamemode-start.lock")) else { return false };
    let busy = unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0;
    busy  // our own lock (if taken) is released when `f` drops
}

fn wait_for_start(preset: &Value) {
    let t0 = std::time::Instant::now();
    let hotplug = tune::HOTPLUG_KEYS.iter().any(|k| !preset["values"][*k].is_null());
    let smt_off = preset["values"]["cpu.smt"] == "off";
    let mut active_since: Option<std::time::Instant> = None;
    loop {
        let held = start_lock_held();
        // An A launch (boot defaults) applies no preset: nothing to wait for once PRE is done.
        if !held && ab_launch_live(&read_scene_state()) { return; }
        // tune-helper's world-readable state file: no helper process (full sysfs describe) every 200 ms while the game starts.
        let active = !held && read_json(Path::new("/run/centurion/tune/state.json"))
            .map_or(false, |st| st["source"] == "game" && centurion_helpers::live_game_sessions(&st) > 0);
        let smt = !smt_off || read_json_str("/sys/devices/system/cpu/smt/active").as_deref() == Some("0");
        if active && smt { break; }
        if active {
            if active_since.get_or_insert_with(std::time::Instant::now).elapsed() >= SMT_WAIT {
                log!("centurion-gamemode RUN: the preset asks for SMT off but SMT is still active — starting with the current topology");
                break;
            }
        } else {
            active_since = None;
        }
        let waited = t0.elapsed();
        // No PRE hook at all: nothing will ever take the lock.
        if !held && !active && waited >= START_DETECT {
            if hotplug { log!("centurion-gamemode RUN: game mode not applied (is PRE set, and did it succeed?) — pinning with the current topology"); }
            return;
        }
        if waited >= START_WAIT {
            log!("centurion-gamemode RUN: PRE still busy after {} s — starting the game anyway", START_WAIT.as_secs());
            return;
        }
        std::thread::sleep(std::time::Duration::from_millis(200));
    }
    // CPU offlining finishes before tune-helper returns, but give cacheinfo a beat to settle.
    if hotplug { std::thread::sleep(std::time::Duration::from_millis(200)); }
}

fn read_json_str(p: &str) -> Option<String> { std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned()) }


fn run_exec(args: &[String]) -> i32 {
    let (name, cmd) = split_cmd(args);
    if cmd.is_empty() { log!("centurion-gamemode RUN: no command"); return 2; }
    match load_preset(name.as_deref()) {
        Ok((_, mut p)) => {
            let parked = soft_park(&mut p);
            wait_for_start(&p);
            if read_scene_state()["ab_a"] != true { prepare_run(&p, parked.as_deref()) }
        }
        Err(e) => log!("centurion-gamemode: {e} — starting without boost"),
    }
    let err = Command::new(&cmd[0]).args(&cmd[1..]).exec();
    log!("centurion-gamemode: {}: {err}", cmd[0]);
    127
}

fn status() -> i32 {
    let out = Command::new(helper()).stdin(Stdio::piped()).stdout(Stdio::piped()).spawn()
        .and_then(|mut c| { c.stdin.take().unwrap().write_all(br#"{"op":"describe"}"#)?; c.wait_with_output() });
    let Some(v) = out.ok().and_then(|o| serde_json::from_slice::<Value>(&o.stdout).ok()) else {
        log!("centurion-gamemode: cannot run {}", helper());
        return 1;
    };
    let st = &v["state"];
    println!("tuning   : {} (source {}, preset {}, {} game(s), {} file(s) saved)",
        if st["active"] == true { "ACTIVE" } else { "off" }, st["source"].as_str().unwrap_or("-"),
        st["preset"].as_str().unwrap_or("-"), st["refcount"], st["saved_files"]);
    println!("default  : {}", default_preset().unwrap_or_else(|| "-".into()));
    let iso = &v["isolation"];
    if iso["active"] == true {
        println!("partition: {} ({}, {} process(es))", iso["cpus"].as_str().unwrap_or(""), iso["partition"].as_str().unwrap_or(""), iso["procs"]);
    }
    if let Some(b) = v["boot"].as_object() { println!("at boot  : {}", b.get("preset").and_then(Value::as_str).unwrap_or("(unnamed)")); }
    for c in v["topology"]["ccds"].as_array().into_iter().flatten() {
        println!("CCD{}     : cpus {}  L3 {} MB  max {} MHz", c["index"], c["cpus"].as_str().unwrap_or(""),
            c["l3_kib"].as_u64().unwrap_or(0) / 1024, c["max_khz"].as_u64().unwrap_or(0) / 1000);
    }
    let mut group = "";
    for t in v["tunables"].as_array().into_iter().flatten() {
        let g = t["group"].as_str().unwrap_or("");
        if g != group { println!("\n[{g}]"); group = g; }
        let cur = t["current"].as_str().unwrap_or(if t["available"] == true { "(root only)" } else { "n/a" });
        println!("  {:<30} {}", t["key"].as_str().unwrap_or(""), cur);
    }
    0
}

/// APPROVE [preset…]: stores the values of user presets (all of them by default) in the root-owned store the
/// helper applies from. Needs the tune-helper authorization. Migrates presets saved before the store existed.
fn approve(names: &[String]) -> i32 {
    let all: Vec<String> = if names.is_empty() {
        std::fs::read_dir(presets_dir()).into_iter().flatten().flatten()
            .filter_map(|e| e.file_name().to_str().and_then(|s| s.strip_suffix(".json")).map(str::to_owned)).collect()
    } else { names.to_vec() };
    let mut presets = serde_json::Map::new();
    for n in all.into_iter().filter(|n| valid_name(n)) {
        if let Some(v) = read_json(&presets_dir().join(format!("{n}.json"))) {
            if v["values"].is_object() { presets.insert(n, v["values"].clone()); }
        }
    }
    if presets.is_empty() { log!("centurion-gamemode APPROVE: no preset to approve"); return 1; }
    let n = presets.len();
    match pkexec_helper(&format!("{HELPER_DIR}/tune-helper"), &json!({"op": "preset_save", "presets": presets})) {
        Ok(v) => { let ok = report("APPROVE", &v); if ok { log!("centurion-gamemode APPROVE: {n} preset(s) approved"); } (!ok) as i32 }
        Err(e) => { log!("centurion-gamemode: {e}"); 1 }
    }
}

fn usage() -> i32 {
    log!("usage: centurion-gamemode PRE [preset] | POST | RUN [preset] [--] cmd… | WRAP [preset] [--] cmd…\n\
               \x20                   | APPLY preset | UNDERVOLT | SCENE name | RESTORE | STATUS | APPROVE [preset…]");
    2
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Err(e) = centurion_helpers::machine::check() {
        log!("centurion-gamemode: {e}");
        // RUN / WRAP still start the game — only without any tuning.
        if let Some(mode) = args.first().map(|s| s.to_ascii_uppercase()).filter(|m| m == "RUN" || m == "WRAP") {
            let (_, cmd) = split_cmd(&args[1..]);
            if cmd.is_empty() { log!("centurion-gamemode {mode}: no command"); std::process::exit(2); }
            let err = Command::new(&cmd[0]).args(&cmd[1..]).exec();
            log!("centurion-gamemode: {}: {err}", cmd[0]);
            std::process::exit(127);
        }
        std::process::exit(1);
    }
    let fail = |e: String| { log!("centurion-gamemode: {e}"); 1 };
    let code = match args.first().map(|s| s.to_ascii_uppercase()).as_deref() {
        Some("PRE") => { OWNER.store(launcher_pid(), Ordering::SeqCst); pre(args.get(1).map(String::as_str)) }
        Some("POST") => { OWNER.store(launcher_pid(), Ordering::SeqCst); post().map_or_else(fail, |ok| (!ok) as i32) }
        Some("APPLY") if args.len() == 2 => apply(Some(&args[1]), "manual").map_or_else(fail, |ok| (!ok) as i32),
        Some("APPROVE") => approve(&args[1..]),
        Some("RESTORE") => pkexec(&json!({"op": "restore"})).map_or_else(fail, |v| (!report("RESTORE", &v)) as i32),
        Some("UNDERVOLT") => (!undervolt(true)) as i32,
        Some("SCENE") if args.len() == 2 => {
            // Only a scene that exists becomes the active one.
            if !valid_name(&args[1]) || read_json(&scenes_dir().join(format!("{}.json", args[1]))).is_none() {
                log!("centurion-gamemode: scene \"{}\" not found ({})", args[1], scenes_dir().display());
                std::process::exit(1);
            }
            let ok = apply_scene(&args[1], SceneParts { cpu: true, gpu: true, tuning: true });
            update_scene_state(|st| st["active"] = json!(args[1]));
            (!ok) as i32
        }
        Some("STATUS") => status(),
        Some("RUN") => run_exec(&args[1..]),
        Some("WRAP") => wrap(&args[1..]),
        _ => usage(),
    };
    std::process::exit(code);
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn names() {
        assert!(valid_name("Gaming X3D"));
        assert!(valid_name("low-latency_2"));
        assert!(!valid_name("../x"));
        assert!(!valid_name("/usr/bin/wine"));
        assert!(!valid_name(""));
        assert!(!valid_name("a..b"));
    }
    #[test]
    fn lutris_wrapper() {
        assert!(is_lutris_wrapper("lutris-wrapper:", b"lutris-wrapper: /usr/bin/centurion-gamemode"));
        assert!(is_lutris_wrapper("lutris-wrapper", b"/usr/bin/python3\0/usr/share/lutris/bin/lutris-wrapper\0x\0"));
        assert!(is_lutris_wrapper("python3.13", b"/usr/bin/python3.13\0/usr/share/lutris/bin/lutris-wrapper\0/usr/bin/centurion-gamemode\01\00\0"));
        assert!(!is_lutris_wrapper("lutris", b"/usr/bin/python3.13\0/usr/bin/lutris\0"));
        assert!(!is_lutris_wrapper("steam", b"/home/u/.steam/steam\0"));
    }
    #[test]
    fn split_like_qt() {
        assert_eq!(split_command(r#"kscreen-doctor output.eDP-1.mode.2560x1600@240"#),
                   vec!["kscreen-doctor", "output.eDP-1.mode.2560x1600@240"]);
        assert_eq!(split_command(r#"notify-send "Game mode" 'x'"#), vec!["notify-send", "Game mode", "'x'"]);
        assert_eq!(split_command(r#"a """b""" c"#), vec!["a", "\"b\"", "c"]);
        assert!(split_command("   ").is_empty());
    }
    #[test]
    fn split() {
        let a: Vec<String> = ["--", "wine", "game.exe"].iter().map(|s| s.to_string()).collect();
        assert_eq!(split_cmd(&a), (None, vec!["wine".to_string(), "game.exe".to_string()]));
    }
}
