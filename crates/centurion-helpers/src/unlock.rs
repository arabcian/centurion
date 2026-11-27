//! Unlock PIN: the gate in front of the root writes that put a new value on the hardware or the kernel.
//!
//! polkit lets a wheel/sudo admin at the machine run the tuning helpers without a password
//! (packaging/polkit/49-centurion.rules). Any program running as that user could do the same, so the helpers
//! that write new values (tune-helper, intel-uv-helper, ryzen-co-helper, amdgpu-helper, legion-gpu-helper,
//! legion-profile-helper, nvcurve-root-helper, fwattr-helper) refuse those writes unless the request comes
//! from a Centurion window the user unlocked with a 4-digit PIN.
//!
//!   * The PIN is set with the administrator password (tune-admin-helper "pin_set", polkit auth_admin).
//!     Stored as a salted, iterated SHA-256 in PIN_FILE (root, 0600): unprivileged code never sees it.
//!   * "unlock" (tune-profile-helper) checks it and records the process that ran pkexec — the GUI — with its
//!     start time in UNLOCK_FILE. That record is the grant: it lasts until the GUI exits or locks.
//!   * `require` (the gated helpers) accepts a request only if a process on its parent chain is the recorded
//!     one. Another program cannot borrow the grant: it is not a descendant of the unlocked GUI.
//!   * Window closed: the GUI calls "lock" with "scenes": true. The grant then covers only requests marked
//!     "scene": true (the GUI's scene engine keeps working from the tray); anything typed needs the PIN again.
//!   * Wrong PINs: from the 5th failure on, each further try waits 1, 2, 4 … minutes; after MAX_FAILURES the PIN
//!     is disabled until it is set again with the administrator password. The counter survives a reboot.
//!   * Scene steps ("scene": true) only write values the user approved. Scene and CPU-profile files live in the
//!     user's home, so another program could edit one and wait for it to be applied automatically. Root keeps a
//!     per-user list of approved payload digests (APPROVED_FILE): a step sent with "approve": true from the
//!     unlocked window (the GUI does that only for an apply the user started: Scenes tab, tray) adds its digest;
//!     every other scene step (power source, login, resume, D-Bus, shortcut, or any step while the window is
//!     closed) is accepted only if its digest is already in the list.
//!   * Not through pkexec (boot services, the daemon, root-run centurion-calibrate, sudo): nothing to gate.

use serde_json::{json, Value};
use std::io::{Read, Write};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};

pub const PIN_FILE: &str = "/etc/centurion/pin.json";
const PIN_DIR: &str = "/etc/centurion";
const STATE_FILE: &str = "/var/lib/centurion/pin-state.json";
const STATE_DIR: &str = "/var/lib/centurion";
const UNLOCK_FILE: &str = "/run/centurion/unlock.json";
const RUN_DIR: &str = "/run/centurion";
const LOCK_NAME: &str = "unlock";
/// Approved scene-step payload digests, per uid (root, 0600).
const APPROVED_FILE: &str = "/etc/centurion/scene-approved.json";
/// Newest digests kept per user (each scene has a handful of steps; edits add new ones).
const MAX_APPROVED: usize = 512;
const ROUNDS: u32 = 20_000;
/// Free tries before the waits start.
const FREE_FAILURES: u32 = 4;
/// Wrong PINs in a row after which the PIN is disabled (10 of 10 000 PINs: a 0.1 % chance to guess).
pub const MAX_FAILURES: u32 = 10;
/// How far up the parent chain the unlocked GUI may be (GUI → pkexec'd helper is 1; GUI → script → helper 2 …).
const MAX_DEPTH: usize = 8;

fn now() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs()) }

pub fn pin_ok(pin: &str) -> bool { pin.len() == 4 && pin.bytes().all(|b| b.is_ascii_digit()) }

fn hex(b: &[u8]) -> String { b.iter().map(|x| format!("{x:02x}")).collect() }

/// Salted, iterated SHA-256 of the PIN.
pub fn digest(salt: &[u8], pin: &str, rounds: u32) -> [u8; 32] {
    let mut h = crate::bootlogo::sha256(&[salt, pin.as_bytes()].concat());
    for _ in 1..rounds { h = crate::bootlogo::sha256(&[&h[..], salt].concat()); }
    h
}

/// Constant-time comparison (the stored digest is root-only, but there is no reason to leak timing).
fn same(a: &[u8], b: &[u8]) -> bool { a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0 }

fn random(n: usize) -> Result<Vec<u8>, String> {
    let mut v = vec![0u8; n];
    std::fs::File::open("/dev/urandom").and_then(|mut f| f.read_exact(&mut v)).map_err(|e| format!("/dev/urandom: {e}"))?;
    Ok(v)
}

/// Atomic root-owned write with mode 0600 (write_root_file makes 0644).
fn write_private(p: &str, body: &[u8]) -> Result<(), String> {
    let tmp = format!("{p}.{}.tmp", std::process::id());
    let _ = std::fs::remove_file(&tmp);
    let mut f = std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(&tmp).map_err(|e| format!("{tmp}: {e}"))?;
    let res = f.write_all(body).and_then(|_| f.set_permissions(std::fs::Permissions::from_mode(0o600))).and_then(|_| f.sync_all());
    drop(f);
    if let Err(e) = res.and_then(|_| std::fs::rename(&tmp, p)) {
        let _ = std::fs::remove_file(&tmp);
        return Err(format!("{p}: {e}"));
    }
    Ok(())
}

/// Reads a root-owned 0600 file (no symlink).
fn read_private(p: &str) -> Option<Value> {
    let limit = if p == APPROVED_FILE { 8 * 1024 * 1024 } else { 4096 };
    let f = std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(p).ok()?;
    let m = f.metadata().ok()?;
    if !m.is_file() || m.uid() != 0 || m.mode() & 0o077 != 0 || m.len() > limit { return None; }
    let mut s = String::new();
    (&f).take(limit + 1).read_to_string(&mut s).ok()?;
    if s.len() as u64 > limit { return None; }
    serde_json::from_str(&s).ok()
}

#[derive(Default, Debug, PartialEq)]
pub struct Failures { pub count: u32, pub until: u64 }

impl Failures {
    fn load() -> Failures {
        let v = read_private(STATE_FILE).unwrap_or(Value::Null);
        Failures { count: v["failures"].as_u64().unwrap_or(0) as u32, until: v["until"].as_u64().unwrap_or(0) }
    }
    fn save(&self) -> Result<(), String> {
        crate::secure_dir(STATE_DIR)?;
        write_private(STATE_FILE, json!({"failures": self.count, "until": self.until}).to_string().as_bytes())
    }
    /// After one more wrong PIN at `t`: the count and the earliest time of the next try.
    pub fn failed(&self, t: u64) -> Failures {
        let count = self.count + 1;
        let wait = if count > FREE_FAILURES { 60u64 << (count - FREE_FAILURES - 1).min(6) } else { 0 };
        Failures { count, until: t + wait }
    }
    pub fn disabled(&self) -> bool { self.count >= MAX_FAILURES }
}

/// The process that ran pkexec (our parent) and the user pkexec authenticated. None when not run through pkexec.
fn pkexec_caller() -> Option<(i32, u32)> {
    let uid = std::env::var("PKEXEC_UID").ok()?.parse::<u32>().ok()?;
    Some((unsafe { libc::getppid() }, uid))
}

fn proc_ruid(pid: i32) -> Option<u32> {
    let s = std::fs::read_to_string(format!("/proc/{pid}/status")).ok()?;
    s.lines().find_map(|l| l.strip_prefix("Uid:"))?.split_whitespace().next()?.parse().ok()
}

fn proc_ppid(pid: i32) -> Option<i32> {
    let s = std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    s[s.rfind(')')? + 1..].split_whitespace().nth(1)?.parse().ok()
}

fn grant() -> Option<Value> { read_private(UNLOCK_FILE) }

/// True if the recorded process (pid + start time) is `pid` or one of its first MAX_DEPTH ancestors.
fn on_chain(pid: i32, granted: i32, start: u64) -> bool {
    let mut p = pid;
    for _ in 0..=MAX_DEPTH {
        if p <= 1 { return false; }
        if p == granted { return crate::proc_start_time(p) == Some(start); }
        match proc_ppid(p) { Some(n) => p = n, None => return false }
    }
    false
}

fn locked_reply(why: &str) -> Value {
    json!({"ok": false, "locked": true, "pin_set": std::path::Path::new(PIN_FILE).exists(), "error": why})
}

/// Digest of a scene step's payload: the request without the routing flags ("scene", "approve", "hold"),
/// serialised with sorted keys (serde_json's default map), as SHA-256 hex.
pub fn payload_digest(req: &Value) -> String {
    let mut v = req.clone();
    if let Some(o) = v.as_object_mut() { for k in ["scene", "approve", "hold"] { o.remove(k); } }
    hex(&crate::bootlogo::sha256(v.to_string().as_bytes()))
}

#[derive(Debug, PartialEq)]
enum Verdict { Allow, Approve(String) }

/// The decision itself, without the system: `grant` is UNLOCK_FILE's content, `chain(pid, start)` tells whether that
/// process is the caller or one of its ancestors, `approved(digest)` whether a scene step was approved before.
fn decide(pin_set: bool, grant: Option<&Value>, uid: u32, chain: impl Fn(i32, u64) -> bool, req: &Value,
          approved: impl Fn(&str) -> bool) -> Result<Verdict, &'static str> {
    if !pin_set { return Err("no unlock PIN is set: set one in Centurion (the lock button), then unlock to write new values"); }
    // The root-owned approval belongs to the authenticated uid and survives GUI restarts.
    // A manual apply can request approval too: replay an existing entry before checking the PIN grant.
    if req["scene"] == true && approved(&payload_digest(req)) { return Ok(Verdict::Allow); }
    let Some(g) = grant else { return Err("Centurion is locked: unlock it with your PIN (the lock button) to write new values") };
    let (Some(gpid), Some(gstart), Some(guid)) = (g["pid"].as_i64(), g["start"].as_u64(), g["uid"].as_u64()) else {
        return Err("Centurion is locked: unlock it with your PIN");
    };
    if guid != u64::from(uid) || !chain(gpid as i32, gstart) {
        return Err("only the Centurion window that was unlocked with the PIN can write new values");
    }
    let window_open = g["scenes_only"] != true;
    if req["scene"] != true {
        return if window_open { Ok(Verdict::Allow) }
               else { Err("Centurion is locked (the window was closed): unlock it with your PIN to write new values") };
    }
    let d = payload_digest(req);
    if req["approve"] == true && window_open { return Ok(Verdict::Approve(d)); }
    Err("this scene step has values that were not approved yet: apply the scene (or profile) once from the unlocked Centurion window")
}

fn approved_list(uid: u32) -> Vec<String> {
    read_private(APPROVED_FILE).and_then(|v| v[uid.to_string()].as_array().cloned()).unwrap_or_default()
        .into_iter().filter_map(|x| x.as_str().map(str::to_owned)).collect()
}

fn approve(uid: u32, d: String) -> Result<(), String> {
    let _l = crate::helper_lock(LOCK_NAME)?;
    let mut all = match read_private(APPROVED_FILE).filter(Value::is_object) {
        Some(store) => store,
        None if !std::path::Path::new(APPROVED_FILE).try_exists().map_err(|e| e.to_string())? => json!({}),
        None => return Err("existing scene approvals could not be read".into()),
    };
    let mut list: Vec<String> = all[uid.to_string()].as_array().into_iter().flatten()
        .filter_map(|value| value.as_str().map(str::to_owned)).collect();
    list.retain(|x| *x != d);
    list.push(d);
    if list.len() > MAX_APPROVED { list.drain(..list.len() - MAX_APPROVED); }
    all[uid.to_string()] = json!(list);
    crate::secure_dir(PIN_DIR)?;
    write_private(APPROVED_FILE, all.to_string().as_bytes())
}

/// The gate. Ok when not run through pkexec, or when the caller descends from the unlocked GUI (scene steps: with
/// values the user approved). Err carries the reply to send back.
pub fn require(req: &Value) -> Result<(), Value> {
    let Some((ppid, uid)) = pkexec_caller() else { return Ok(()) };
    let pin_set = std::path::Path::new(PIN_FILE).exists();
    let list = if req["scene"] == true { approved_list(uid) } else { Vec::new() };
    match decide(pin_set, grant().as_ref(), uid, |pid, start| on_chain(ppid, pid, start), req, |d| list.iter().any(|x| x == d)) {
        Ok(Verdict::Allow) => Ok(()),
        Ok(Verdict::Approve(d)) => approve(uid, d).map_err(|e| json!({"ok": false, "error": format!("could not record the approval: {e}")})),
        Err(why) => Err(locked_reply(why)),
    }
}

/// Shorthand for the helpers: Some(reply) when the request must be refused.
pub fn refuse(req: &Value) -> Option<Value> { require(req).err() }

/// {"op":"pin_status"}: whether a PIN is set, whether the caller holds the grant, the wait after wrong PINs.
pub fn status() -> Value {
    let f = Failures::load();
    let unlocked = match (pkexec_caller(), grant()) {
        (Some((ppid, uid)), Some(g)) => g["uid"].as_u64() == Some(u64::from(uid))
            && on_chain(ppid, g["pid"].as_i64().unwrap_or(0) as i32, g["start"].as_u64().unwrap_or(0)),
        _ => false,
    };
    let g = grant().unwrap_or(Value::Null);
    json!({"ok": true, "pin_set": std::path::Path::new(PIN_FILE).exists(), "unlocked": unlocked,
           "scenes_only": unlocked && g["scenes_only"] == true, "failures": f.count, "max_failures": MAX_FAILURES,
           "disabled": f.disabled(), "wait_seconds": f.until.saturating_sub(now())})
}

/// {"op":"unlock","pin":"1234"} through pkexec: the process that ran pkexec gets the grant.
pub fn unlock(pin: &str) -> Value {
    let Some((ppid, uid)) = pkexec_caller() else { return json!({"ok": false, "error": "unlock is only available through pkexec"}) };
    if uid == 0 || proc_ruid(ppid) != Some(uid) { return json!({"ok": false, "error": "the caller does not belong to the authenticated user"}); }
    let Some(start) = crate::proc_start_time(ppid) else { return json!({"ok": false, "error": "caller process not found"}) };
    let _l = match crate::helper_lock(LOCK_NAME) { Ok(l) => l, Err(e) => return json!({"ok": false, "error": e}) };
    let f = Failures::load();
    if f.disabled() {
        return json!({"ok": false, "disabled": true, "error": format!("{MAX_FAILURES} wrong PINs: the PIN is disabled. Set a new one (administrator password).")});
    }
    let t = now();
    if f.until > t { return json!({"ok": false, "wait_seconds": f.until - t, "error": format!("too many wrong PINs: try again in {} s", f.until - t)}); }
    let Some(stored) = read_private(PIN_FILE) else { return json!({"ok": false, "pin_set": false, "error": "no unlock PIN is set"}) };
    let salt = stored["salt"].as_str().map(unhex).unwrap_or_default();
    let want = stored["hash"].as_str().map(unhex).unwrap_or_default();
    let rounds = stored["rounds"].as_u64().unwrap_or(u64::from(ROUNDS)).clamp(1, 1_000_000) as u32;
    if !pin_ok(pin) || salt.is_empty() || !same(&digest(&salt, pin, rounds), &want) {
        let n = f.failed(t);
        let _ = n.save();
        crate::wlog::log("unlock", &format!("wrong PIN ({} in a row)", n.count));
        let left = MAX_FAILURES.saturating_sub(n.count);
        return json!({"ok": false, "wrong_pin": true, "tries_left": left, "wait_seconds": n.until.saturating_sub(t),
                      "disabled": n.disabled(), "error": if n.disabled() { "wrong PIN: the PIN is now disabled".to_owned() } else { format!("wrong PIN ({left} tries left)") }});
    }
    if f.count > 0 { let _ = Failures::default().save(); }
    if let Err(e) = crate::secure_dir(RUN_DIR).and_then(|_| write_private(UNLOCK_FILE, json!({"pid": ppid, "start": start, "uid": uid, "t": t}).to_string().as_bytes())) {
        return json!({"ok": false, "error": e});
    }
    crate::wlog::log("unlock", &format!("unlocked for pid {ppid}"));
    json!({"ok": true, "unlocked": true})
}

/// {"op":"lock","scenes":bool}: the caller gives up its grant ("scenes": keep it for scene requests only).
/// Anyone may lock; a grant held by another process is left alone unless it is stale.
pub fn lock(scenes: bool) -> Value {
    let _l = crate::helper_lock(LOCK_NAME).ok();
    let Some(g) = grant() else { return json!({"ok": true, "locked": true}) };
    let (gpid, gstart) = (g["pid"].as_i64().unwrap_or(0) as i32, g["start"].as_u64().unwrap_or(0));
    let mine = pkexec_caller().is_some_and(|(ppid, _)| on_chain(ppid, gpid, gstart));
    let stale = crate::proc_start_time(gpid) != Some(gstart);
    if stale || (mine && !scenes) {
        let _ = std::fs::remove_file(UNLOCK_FILE);
    } else if mine {
        let mut g = g;
        g["scenes_only"] = json!(true);
        if let Err(e) = write_private(UNLOCK_FILE, g.to_string().as_bytes()) { return json!({"ok": false, "error": e}); }
    }
    json!({"ok": true, "locked": true, "scenes_only": mine && scenes && !stale})
}

/// {"op":"pin_set","pin":"1234"} (tune-admin-helper, the administrator password every time): stores a new PIN,
/// clears the wrong-PIN counter and every grant.
pub fn set_pin(pin: &str) -> Value {
    if !pin_ok(pin) { return json!({"ok": false, "error": "the PIN must be exactly 4 digits"}); }
    let _l = match crate::helper_lock(LOCK_NAME) { Ok(l) => l, Err(e) => return json!({"ok": false, "error": e}) };
    let salt = match random(16) { Ok(s) => s, Err(e) => return json!({"ok": false, "error": e}) };
    let body = json!({"salt": hex(&salt), "hash": hex(&digest(&salt, pin, ROUNDS)), "rounds": ROUNDS});
    if let Err(e) = crate::secure_dir(PIN_DIR).and_then(|_| write_private(PIN_FILE, body.to_string().as_bytes())) {
        return json!({"ok": false, "error": e});
    }
    let _ = Failures::default().save();
    let _ = std::fs::remove_file(UNLOCK_FILE);
    crate::wlog::log("unlock", "PIN set");
    json!({"ok": true, "message": "PIN set: unlock Centurion with it to write new values"})
}

fn unhex(s: &str) -> Vec<u8> {
    if s.len() % 2 != 0 { return Vec::new(); }
    (0..s.len()).step_by(2).map_while(|i| u8::from_str_radix(&s[i..i + 2], 16).ok()).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pin_shape() {
        assert!(pin_ok("0000") && pin_ok("9381"));
        for bad in ["", "123", "12345", "12a4", " 123", "١٢٣٤"] { assert!(!pin_ok(bad), "{bad}"); }
    }

    #[test]
    fn digest_depends_on_salt_pin_and_rounds() {
        let a = digest(b"salt", "1234", 10);
        assert_eq!(a, digest(b"salt", "1234", 10));
        assert_ne!(a, digest(b"salT", "1234", 10));
        assert_ne!(a, digest(b"salt", "1235", 10));
        assert_ne!(a, digest(b"salt", "1234", 11));
        assert_eq!(unhex(&hex(&a)), a.to_vec());
        assert!(same(&a, &a) && !same(&a, &a[..31]));
    }

    #[test]
    fn waits_grow_and_the_pin_is_disabled() {
        let mut f = Failures::default();
        for _ in 0..FREE_FAILURES { f = f.failed(1000); assert_eq!(f.until, 1000); }
        f = f.failed(1000);
        assert_eq!(f.until, 1060);
        f = f.failed(2000);
        assert_eq!(f.until, 2120);
        while !f.disabled() { f = f.failed(5000); }
        assert_eq!(f.count, MAX_FAILURES);
    }

    #[test]
    fn grant_decisions() {
        let g = json!({"pid": 4242, "start": 99, "uid": 1000});
        let gui = |pid: i32, start: u64| pid == 4242 && start == 99;
        let other = |_: i32, _: u64| false;
        let none = |_: &str| false;
        let apply = json!({"op": "apply"});
        let ok = |r: Result<Verdict, &str>| r == Ok(Verdict::Allow);
        assert!(ok(decide(true, Some(&g), 1000, gui, &apply, none)));
        assert!(decide(false, Some(&g), 1000, gui, &apply, none).is_err(), "no PIN set");
        assert!(decide(true, None, 1000, gui, &apply, none).is_err(), "not unlocked");
        assert!(decide(true, Some(&g), 1000, other, &apply, none).is_err(), "another program, same user");
        assert!(decide(true, Some(&g), 1001, gui, &apply, none).is_err(), "another user");
        assert!(decide(true, Some(&json!({"pid": 4242})), 1000, gui, &apply, none).is_err(), "malformed grant");
        let closed = json!({"pid": 4242, "start": 99, "uid": 1000, "scenes_only": true});
        assert!(decide(true, Some(&closed), 1000, gui, &apply, none).is_err(), "window closed: typed values refused");
    }

    #[test]
    fn scene_steps_need_approved_values() {
        let open = json!({"pid": 4242, "start": 99, "uid": 1000});
        let closed = json!({"pid": 4242, "start": 99, "uid": 1000, "scenes_only": true});
        let gui = |pid: i32, start: u64| pid == 4242 && start == 99;
        let step = json!({"op": "apply", "profile": {"voltage": {"core": -80}}, "scene": true});
        let user_started = json!({"op": "apply", "profile": {"voltage": {"core": -80}}, "scene": true, "approve": true, "hold": false});
        let edited = json!({"op": "apply", "profile": {"voltage": {"core": -190}}, "scene": true});
        let d = payload_digest(&step);
        let known = |x: &str| x == d;
        let none = |_: &str| false;
        // The user applies it from the unlocked window: approved (same digest whatever the routing flags).
        assert_eq!(decide(true, Some(&open), 1000, gui, &user_started, none), Ok(Verdict::Approve(d.clone())));
        // Later, automatically or with the window closed: accepted because it was approved.
        assert_eq!(decide(true, Some(&closed), 1000, gui, &step, known), Ok(Verdict::Allow));
        assert_eq!(decide(true, Some(&open), 1000, gui, &step, known), Ok(Verdict::Allow));
        assert_eq!(decide(true, None, 1000, gui, &step, known), Ok(Verdict::Allow));
        assert_eq!(decide(true, None, 1000, gui, &user_started, known), Ok(Verdict::Allow));
        assert!(decide(true, None, 1000, gui, &edited, known).is_err());
        assert!(decide(true, None, 1001, gui, &step, none).is_err());
        // Someone edited the file: new values, not approved, refused even while the window is open.
        assert!(decide(true, Some(&closed), 1000, gui, &edited, known).is_err());
        assert!(decide(true, Some(&open), 1000, gui, &edited, known).is_err());
        // "approve" counts only while the window is open.
        let mut sneaky = edited.clone();
        sneaky["approve"] = json!(true);
        assert!(decide(true, Some(&closed), 1000, gui, &sneaky, known).is_err());
        // Another program cannot use any of it.
        assert!(decide(true, Some(&open), 1000, |_, _| false, &user_started, none).is_err());
        // Key order does not change the digest.
        let a: Value = serde_json::from_str(r#"{"op":"x","a":1,"b":[1,2]}"#).unwrap();
        let b: Value = serde_json::from_str(r#"{"b":[1,2],"a":1,"op":"x","scene":true}"#).unwrap();
        assert_eq!(payload_digest(&a), payload_digest(&b));
    }

    #[test]
    fn not_through_pkexec_is_not_gated() {
        // The test process has no PKEXEC_UID: a root service or sudo run is not asked for the PIN.
        if std::env::var("PKEXEC_UID").is_err() { assert!(require(&json!({"op": "apply"})).is_ok()); }
    }

    #[test]
    fn own_chain() {
        let me = std::process::id() as i32;
        let start = crate::proc_start_time(me).unwrap();
        assert!(on_chain(me, me, start));
        assert!(!on_chain(me, me, start + 1));
        let parent = unsafe { libc::getppid() };
        if let Some(ps) = crate::proc_start_time(parent) { assert!(on_chain(me, parent, ps)); }
    }
}
