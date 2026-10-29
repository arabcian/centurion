//! Health → Overview, the Home / tray badge, and the dGPU usage view.
//!
//! Every card is decided here from the files the other Health pages keep (health.log, the boot
//! record, battery.json, cooling.json, the sleep log, the Backup folders, dgpu.json). Read-only,
//! run as the user, asked for only when the Overview page is shown, once at start-up for the badge,
//! and after a new kernel fault. The GUI words the cards (its texts are translated); this module
//! returns levels and numbers only.
//!
//! Levels: -1 no data yet, 0 ok, 1 attention, 2 problem.
//!
//! stdin:  {"op":"overview", "battery": bool, "dgpu": bool, "backup_dirs": ["/abs/dir", …], "kernel_seen"?: epoch, "unclean_seen"?: epoch}
//! stdout: {"ok": true, "cards": [{"id", "level", …}], "worst": n}
//! stdin:  {"op":"dgpu_usage"}
//! stdout: {"ok": true, "today": {"awake_pct", "hours"}, "wakes": [{"t", "holders"}], "power": {…}}

use crate::cooling::{self, Level};
use crate::stability;
use serde_json::{json, Value};
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

pub const NODATA: i64 = -1;
pub const OK: i64 = 0;
pub const WARN: i64 = 1;
pub const BAD: i64 = 2;

const DAY: i64 = 86_400;
const WEEK: i64 = 7 * DAY;
const MONTH: i64 = 30 * DAY;
/// A log is read from its end; this much is more than a week of kernel faults.
const TAIL_BYTES: u64 = 2 << 20;
const JSON_MAX: u64 = 4 << 20;
/// Battery health (full ÷ design capacity) thresholds, in percent.
const BATTERY_OK_PCT: f64 = 80.0;
const BATTERY_WARN_PCT: f64 = 70.0;
/// Backup age thresholds, in days.
const BACKUP_OK_DAYS: i64 = 7;
const BACKUP_WARN_DAYS: i64 = 30;
/// Unexpected shutdowns in the last 30 days: one or two need a look, three or more is a problem.
const DIRTY_WARN: u64 = 1;
const DIRTY_BAD: u64 = 3;
/// Share of battery time the dGPU spent awake, in percent.
const DGPU_WARN_AWAKE_PCT: f64 = 20.0;
/// A sleep that drains faster than this (% per hour) is a warning: the Sleep page's own threshold.
const SLEEP_WARN_PCT_H: f64 = 2.0;
/// Shorter sleeps say nothing about drain (the Sleep page's rule).
const SLEEP_MIN_H: f64 = 0.25;
/// dGPU awake and asleep power are compared only once both sides have this much battery time.
pub const POWER_MIN_SEC: f64 = 1800.0;
/// Wake-ups kept in dgpu.json and shown on the page (newest first).
pub const WAKES_SHOWN: usize = 20;

// ── time ─────────────────────────────────────────────────────────────────────

/// Unix seconds and the local UTC offset in seconds.
pub fn local_clock() -> (i64, i64) {
    let now = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs() as i64);
    let t = now as libc::time_t;
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe { libc::localtime_r(&t, &mut tm) };
    (now, tm.tm_gmtoff as i64)
}

/// Local calendar date "YYYY-MM-DD" of a unix time.
pub fn ymd(epoch: i64, offset: i64) -> String {
    let (y, m, d) = civil_from_days((epoch + offset).div_euclid(DAY));
    format!("{y:04}-{m:02}-{d:02}")
}

/// Hinnant's days_from_civil: days since 1970-01-01.
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let mp = (m + 9) % 12;
    let doy = (153 * mp + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

fn civil_from_days(z: i64) -> (i64, i64, i64) {
    let z = z + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (if m <= 2 { yoe + era * 400 + 1 } else { yoe + era * 400 }, m, d)
}

/// "YYYY-MM-DD HH:MM:SS" (local wall clock, as the GUI logs write it) → unix seconds.
pub fn parse_local(ts: &str, offset: i64) -> Option<i64> {
    let n = |r: std::ops::Range<usize>| ts.get(r)?.parse::<i64>().ok();
    let (y, m, d) = (n(0..4)?, n(5..7)?, n(8..10)?);
    let (hh, mm, ss) = (n(11..13)?, n(14..16)?, n(17..19)?);
    if !(1..=12).contains(&m) || !(1..=31).contains(&d) { return None; }
    Some(days_from_civil(y, m, d) * DAY + hh * 3600 + mm * 60 + ss - offset)
}

// ── files ────────────────────────────────────────────────────────────────────

/// ~/.local/state/centurion (or $XDG_STATE_HOME/centurion): the user's own Health records.
pub fn state_dir() -> PathBuf {
    let base = std::env::var_os("XDG_STATE_HOME").filter(|v| !v.is_empty()).map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").filter(|v| !v.is_empty()).map(|h| PathBuf::from(h).join(".local/state")))
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    base.join("centurion")
}

/// The last `max` bytes of a file as whole lines; None when it cannot be read.
pub fn read_tail(path: &Path, max: u64) -> Option<String> {
    let mut f = std::fs::File::open(path).ok()?;
    let len = f.metadata().ok()?.len();
    let cut = len > max;
    if cut { f.seek(SeekFrom::Start(len - max)).ok()?; }
    let mut buf = Vec::new();
    f.take(max).read_to_end(&mut buf).ok()?;
    let mut text = String::from_utf8_lossy(&buf).into_owned();
    if cut {
        if let Some(i) = text.find('\n') { text.drain(..=i); }
    }
    Some(text)
}

/// A whole JSON file (up to JSON_MAX); None when missing, too large or not JSON.
pub fn read_json(path: &Path) -> Option<Value> {
    let meta = std::fs::metadata(path).ok()?;
    if meta.len() > JSON_MAX { return None; }
    serde_json::from_str(&std::fs::read_to_string(path).ok()?).ok()
}

// ── kernel faults (health.log) ───────────────────────────────────────────────

#[derive(Default, Debug, PartialEq)]
pub struct KernelCounts { pub crit24: u64, pub err24: u64, pub crit7: u64, pub err7: u64 }

/// The lines of health.log stamped after `seen` (epoch seconds, from the Health tab's "Mark all seen";
/// 0 = never marked). Lines whose stamp cannot be read are kept.
pub fn unseen_log(log: &str, seen: i64, offset: i64) -> String {
    if seen <= 0 { return log.to_owned(); }
    log.lines().filter(|line| {
        let f: Vec<&str> = line.split_whitespace().collect();
        f.len() < 2 || parse_local(&format!("{} {}", f[0], f[1]), offset).map_or(true, |t| t > seen)
    }).collect::<Vec<_>>().join("\n")
}

/// Critical and error lines of health.log in the last day and week. Line shape (written by the
/// Health tab): "YYYY-MM-DD HH:MM:SS  boot=… seq=…  LEVEL KIND  title  |  text".
pub fn kernel_counts(log: &str, now: i64, offset: i64) -> KernelCounts {
    let mut c = KernelCounts::default();
    for line in log.lines() {
        let f: Vec<&str> = line.split_whitespace().collect();
        if f.len() < 5 { continue; }
        let Some(t) = parse_local(&format!("{} {}", f[0], f[1]), offset) else { continue };
        let age = (now - t).max(0);
        if age > WEEK { continue; }
        match f[4] {
            "CRITICAL" => { c.crit7 += 1; if age <= DAY { c.crit24 += 1; } }
            "ERROR" => { c.err7 += 1; if age <= DAY { c.err24 += 1; } }
            _ => {}
        }
    }
    c
}

// ── unexpected shutdowns (exposure.json) ─────────────────────────────────────

#[derive(Default, Debug, PartialEq)]
pub struct Dirty { pub boots: u64, pub unclean: u64, pub reviewed: u64, pub newest: i64 }

/// Boots that started in the last 30 days and how many of them did not end cleanly. `seen` is the
/// start of the newest unclean boot the user marked reviewed (0 = none): unclean boots up to it are
/// counted in `reviewed`, not `unclean`. A later crash starts later, so it is never hidden.
/// `newest`: start of the newest unclean boot (what "Mark reviewed" stores).
pub fn dirty_boots(exposure: &Value, now: i64, seen: i64) -> Dirty {
    let mut d = Dirty::default();
    for b in exposure["boots"].as_array().into_iter().flatten() {
        let start = b["start"].as_i64().unwrap_or(0);
        if !(0..=MONTH).contains(&(now - start)) { continue; }
        d.boots += 1;
        if b["clean"].as_bool() != Some(false) { continue; }
        if start <= seen { d.reviewed += 1; } else { d.unclean += 1; }
        d.newest = d.newest.max(start);
    }
    d
}

// ── battery (battery.json) ───────────────────────────────────────────────────

/// (health %, full Wh, date) from the newest capacity sample that knows its design capacity.
pub fn battery_health(store: &Value) -> Option<(f64, f64, String)> {
    let e = store["capacity"].as_array()?.iter().rev().find(|e| e["design"].as_f64().unwrap_or(0.0) > 0.0)?;
    let (full, design) = (e["full"].as_f64()?, e["design"].as_f64()?);
    Some((100.0 * full / design, full, e["d"].as_str().unwrap_or("").to_owned()))
}

// ── cooling (cooling.json) ───────────────────────────────────────────────────

/// (time of the newest judged run, its verdict) — the same assessment the Cooling page shows.
pub fn cooling_latest(store: &Value) -> Option<(i64, Level)> {
    let runs = cooling::runs(store);
    for r in runs.iter().rev() {
        let Some(key) = r["key"].as_str() else { continue };
        let limit = r["ctx"]["limits"]["cpu_temp"].as_f64().unwrap_or(cooling::DEFAULT_LIMIT_C);
        if let Some((last, a)) = cooling::latest_assessment(&runs, key, limit) {
            return Some((last["t"].as_i64().unwrap_or(0), a.level));
        }
    }
    None
}

// ── sleeps (sleep.log) ───────────────────────────────────────────────────────

#[derive(Default, Debug, PartialEq)]
pub struct SleepStats { pub sleeps: u64, pub pct: f64, pub hours: f64, pub dgpu_errors: u64 }

/// Sleeps of the last week. Drain counts sleeps that started on battery and lasted SLEEP_MIN_H or more.
pub fn sleep_stats(log: &str, now: i64) -> SleepStats {
    let mut s = SleepStats::default();
    for line in log.lines() {
        let Ok(o) = serde_json::from_str::<Value>(line) else { continue };
        let start = o["start"].as_i64().unwrap_or(0);
        if !(0..=WEEK).contains(&(now - start)) { continue; }
        s.sleeps += 1;
        if o["dgpu_after"] == "error" { s.dgpu_errors += 1; }
        let h = (o["end"].as_i64().unwrap_or(start) - start) as f64 / 3600.0;
        let (e0, e1, full) = (o["energy_before"].as_f64().unwrap_or(0.0), o["energy_after"].as_f64().unwrap_or(0.0), o["full"].as_f64().unwrap_or(0.0));
        if o["status_before"] == "Discharging" && full > 0.0 && e0 >= e1 && h >= SLEEP_MIN_H {
            s.pct += 100.0 * (e0 - e1) / full;
            s.hours += h;
        }
    }
    s
}

// ── backups ──────────────────────────────────────────────────────────────────

/// Seconds since the newest regular file directly inside any of the folders (None: none found).
pub fn newest_age(dirs: &[PathBuf], now: i64) -> Option<i64> {
    let mut newest: Option<i64> = None;
    for d in dirs {
        let Ok(rd) = std::fs::read_dir(d) else { continue };
        for e in rd.flatten() {
            let Ok(m) = e.metadata() else { continue };
            if !m.is_file() { continue; }
            let Some(t) = m.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()) else { continue };
            let t = t.as_secs() as i64;
            newest = Some(newest.map_or(t, |n| n.max(t)));
        }
    }
    newest.map(|t| (now - t).max(0))
}

// ── dGPU usage (dgpu.json, written by the GUI's usage tracker) ───────────────

/// Today's battery time and awake share, the last wake-ups, and the awake-vs-asleep power difference.
pub fn dgpu_usage(state: &Value, today: &str) -> Value {
    let bat = &state["days"][today]["bat"];
    let (awake, asleep) = (bat["awake_ms"].as_f64().unwrap_or(0.0), bat["asleep_ms"].as_f64().unwrap_or(0.0));
    let total = awake + asleep;
    let pct = (total > 0.0).then(|| 100.0 * awake / total);
    let wakes: Vec<Value> = state["wakes"].as_array().map(|a| a.iter().rev().take(WAKES_SHOWN).cloned().collect()).unwrap_or_default();

    let side = |k: &str| {
        let e = &state["power"][k];
        (e["sec"].as_f64().unwrap_or(0.0), e["wh"].as_f64().unwrap_or(0.0))
    };
    let (a_sec, a_wh) = side("awake");
    let (s_sec, s_wh) = side("asleep");
    let watts = |sec: f64, wh: f64| wh / (sec / 3600.0);
    let enough = a_sec >= POWER_MIN_SEC && s_sec >= POWER_MIN_SEC;
    let awake_w = (a_sec > 0.0).then(|| watts(a_sec, a_wh));
    let asleep_w = (s_sec > 0.0).then(|| watts(s_sec, s_wh));
    let delta_w = if enough { Some(watts(a_sec, a_wh) - watts(s_sec, s_wh)) } else { None };
    let level = match pct { None => NODATA, Some(p) if p > DGPU_WARN_AWAKE_PCT => WARN, Some(_) => OK };
    json!({
        "level": level,
        "today": {"awake_pct": pct, "hours": total / 3.6e6},
        "wakes": wakes,
        "power": {"awake_w": awake_w, "asleep_w": asleep_w, "delta_w": delta_w, "awake_h": a_sec / 3600.0, "asleep_h": s_sec / 3600.0},
    })
}

// ── cards ────────────────────────────────────────────────────────────────────

/// Everything the cards need, read from disk by `read_inputs` (or built directly in tests).
#[derive(Default)]
pub struct Inputs {
    pub now: i64,
    pub offset: i64,
    pub today: String,
    pub health_log: String,
    pub exposure: Option<Value>,
    /// Start of the newest unclean boot marked reviewed on the Overview (0 = none).
    pub unclean_seen: i64,
    pub battery: Option<Value>,
    pub cooling: Option<Value>,
    pub sleep_log: String,
    pub dgpu_state: Option<Value>,
    pub backup_age: Option<i64>,
    pub has_battery: bool,
    pub has_dgpu: bool,
}

fn card(id: &str, level: i64, fields: Value) -> Value {
    let mut v = json!({"id": id, "level": level});
    if let (Some(dst), Some(src)) = (v.as_object_mut(), fields.as_object()) {
        for (k, x) in src { dst.insert(k.clone(), x.clone()); }
    }
    v
}

fn battery_level(pct: f64) -> i64 {
    if pct >= BATTERY_OK_PCT { OK } else if pct >= BATTERY_WARN_PCT { WARN } else { BAD }
}

/// One card per subsystem that has something to say. Battery and dGPU cards exist only when that
/// hardware is present; the disk card (SMART) is not built yet and is therefore absent.
pub fn cards(i: &Inputs) -> Vec<Value> {
    let mut out = Vec::new();

    let k = kernel_counts(&i.health_log, i.now, i.offset);
    let lvl = if k.crit24 > 0 { BAD } else if k.crit7 + k.err7 > 0 { WARN } else { OK };
    out.push(card("kernel", lvl, json!({"crit24": k.crit24, "err24": k.err24, "crit7": k.crit7, "err7": k.err7})));

    let dirty = i.exposure.as_ref().map(|e| dirty_boots(e, i.now, i.unclean_seen)).filter(|d| d.boots > 0);
    match dirty {
        Some(d) => {
            let lvl = if d.unclean >= DIRTY_BAD { BAD } else if d.unclean >= DIRTY_WARN { WARN } else { OK };
            out.push(card("unclean", lvl, json!({"boots": d.boots, "unclean": d.unclean, "reviewed": d.reviewed, "newest": d.newest})));
        }
        None => out.push(card("unclean", NODATA, json!({}))),
    }

    if i.has_battery {
        match i.battery.as_ref().and_then(battery_health) {
            Some((pct, full, date)) => out.push(card("battery", battery_level(pct), json!({"health": pct, "full_wh": full, "date": date}))),
            None => out.push(card("battery", NODATA, json!({}))),
        }
    }

    match i.cooling.as_ref().and_then(cooling_latest) {
        Some((t, level)) => {
            let (lvl, verdict) = match level {
                Level::Better | Level::Ok => (OK, "ok"),
                Level::Watch => (WARN, "watch"),
                Level::Warn => (BAD, "warn"),
                Level::Inconclusive => (NODATA, "inconclusive"),
            };
            out.push(card("cooling", lvl, json!({"verdict": verdict, "t": t})));
        }
        None => out.push(card("cooling", NODATA, json!({}))),
    }

    match i.backup_age {
        Some(age) => {
            let days = age / DAY;
            let lvl = if days <= BACKUP_OK_DAYS { OK } else if days <= BACKUP_WARN_DAYS { WARN } else { BAD };
            out.push(card("backup", lvl, json!({"age_days": days})));
        }
        None => out.push(card("backup", NODATA, json!({}))),
    }

    if i.has_dgpu {
        let u = dgpu_usage(i.dgpu_state.as_ref().unwrap_or(&Value::Null), &i.today);
        match u["today"]["awake_pct"].as_f64() {
            Some(pct) => out.push(card("dgpu", u["level"].as_i64().unwrap_or(NODATA), json!({"awake_pct": pct, "hours": u["today"]["hours"]}))),
            None => out.push(card("dgpu", NODATA, json!({}))),
        }
    }

    let s = sleep_stats(&i.sleep_log, i.now);
    let per_hour = (s.hours > 0.0).then(|| s.pct / s.hours);
    let lvl = if s.dgpu_errors > 0 { BAD } else if s.sleeps == 0 { NODATA } else if per_hour.is_some_and(|p| p > SLEEP_WARN_PCT_H) { WARN } else { OK };
    out.push(card("sleep", lvl, json!({"sleeps": s.sleeps, "per_hour": per_hour, "dgpu_errors": s.dgpu_errors})));

    out
}

/// Worst level of the cards that have data (NODATA when none does).
pub fn worst(cards: &[Value]) -> i64 {
    cards.iter().filter_map(|c| c["level"].as_i64()).filter(|l| *l >= 0).max().unwrap_or(NODATA)
}

/// Reads every input from its file, as the user, and builds the cards.
pub fn read_inputs(req: &Value) -> Inputs {
    let (now, offset) = local_clock();
    let st = state_dir();
    let mut health = String::new();
    for name in ["health.log.1", "health.log"] {
        if let Some(t) = read_tail(&st.join(name), TAIL_BYTES) { health.push_str(&t); health.push('\n'); }
    }
    let health = unseen_log(&health, req["kernel_seen"].as_i64().unwrap_or(0), offset);
    let backup_dirs: Vec<PathBuf> = req["backup_dirs"].as_array().into_iter().flatten()
        .filter_map(|v| v.as_str()).map(PathBuf::from).filter(|p| p.is_absolute()).collect();
    Inputs {
        now,
        offset,
        today: ymd(now, offset),
        health_log: health,
        exposure: read_json(Path::new(stability::EXPOSURE)),
        unclean_seen: req["unclean_seen"].as_i64().unwrap_or(0),
        battery: read_json(&st.join("battery.json")),
        cooling: read_json(&st.join("cooling.json")),
        sleep_log: read_tail(Path::new("/var/log/centurion/sleep.log"), TAIL_BYTES).unwrap_or_default(),
        dgpu_state: read_json(&st.join("dgpu.json")),
        backup_age: newest_age(&backup_dirs, now),
        has_battery: req["battery"].as_bool().unwrap_or(false),
        has_dgpu: req["dgpu"].as_bool().unwrap_or(false),
    }
}

/// Op "overview".
pub fn request(req: &Value) -> Value {
    let cards = cards(&read_inputs(req));
    let worst = worst(&cards);
    json!({"ok": true, "cards": cards, "worst": worst})
}

/// Op "dgpu_usage".
pub fn dgpu_request(_req: &Value) -> Value {
    let (now, offset) = local_clock();
    let state = read_json(&state_dir().join("dgpu.json")).unwrap_or(Value::Null);
    let mut v = dgpu_usage(&state, &ymd(now, offset));
    v["ok"] = json!(true);
    v
}

#[cfg(test)]
mod tests {
    use super::*;

    // 2000-03-01 00:00:00 UTC.
    const MAR1_2000: i64 = 951_868_800;

    /// "YYYY-MM-DD HH:MM:SS" (UTC) of a unix time, in the format the Health log uses.
    fn stamp(t: i64) -> String {
        let (y, m, d) = civil_from_days(t.div_euclid(DAY));
        let rem = t.rem_euclid(DAY);
        format!("{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02}", rem / 3600, rem / 60 % 60, rem % 60)
    }

    #[test]
    fn calendar_round_trips() {
        assert_eq!(parse_local("2000-03-01 00:00:00", 0), Some(MAR1_2000));
        assert_eq!(parse_local("2000-03-01 03:00:00", 3 * 3600), Some(MAR1_2000));
        assert_eq!(ymd(MAR1_2000, 0), "2000-03-01");
        assert_eq!(ymd(MAR1_2000 - 1, 0), "2000-02-29");
        assert_eq!(ymd(MAR1_2000 - 1, 3 * 3600), "2000-03-01");
        assert_eq!(parse_local("garbage", 0), None);
        assert_eq!(parse_local("2000-13-01 00:00:00", 0), None);
        assert_eq!(stamp(MAR1_2000 + 3661), "2000-03-01 01:01:01");
    }

    fn line(ts: &str, level: &str) -> String {
        format!("{ts}  boot=abcdef12 seq=7  {level:<8} GSP     GSP timeout  |  text")
    }

    #[test]
    fn kernel_counts_split_day_and_week() {
        let now = MAR1_2000 + 10 * DAY;
        let log = [
            line(&stamp(now - 3600), "CRITICAL"),
            line(&stamp(now - 2 * 3600), "ERROR"),
            line(&stamp(now - 3 * DAY), "ERROR"),
            line(&stamp(now - 3 * DAY), "WARN"),
            line(&stamp(now - 10 * DAY), "CRITICAL"),
            "not a log line".to_owned(),
        ]
        .join("\n");
        assert_eq!(kernel_counts(&log, now, 0), KernelCounts { crit24: 1, err24: 1, crit7: 1, err7: 2 });
    }

    #[test]
    fn marking_seen_removes_older_lines_from_the_cards() {
        let now = MAR1_2000 + 10 * DAY;
        let log = [line(&stamp(now - 3600), "CRITICAL"), line(&stamp(now - 600), "ERROR"), "not a log line".to_owned()].join("\n");
        assert_eq!(unseen_log(&log, 0, 0), log, "never marked: nothing is hidden");
        // Marked seen half an hour ago: the critical line of an hour ago is seen, the error of ten minutes ago is not.
        let left = unseen_log(&log, now - 1800, 0);
        assert_eq!(kernel_counts(&left, now, 0), KernelCounts { crit24: 0, err24: 1, crit7: 0, err7: 1 });
        assert!(left.contains("not a log line"));
    }

    #[test]
    fn dirty_boots_last_thirty_days() {
        let now = MAR1_2000 + 100 * DAY;
        let e = json!({"boots": [
            {"boot": "a", "start": now - 40 * DAY, "clean": false},
            {"boot": "b", "start": now - 5 * DAY, "clean": false},
            {"boot": "c", "start": now - 4 * DAY, "clean": true},
            {"boot": "d", "start": now - DAY},
        ]});
        assert_eq!(dirty_boots(&e, now, 0), Dirty { boots: 3, unclean: 1, reviewed: 0, newest: now - 5 * DAY });
    }

    #[test]
    fn reviewed_unclean_boots_stop_counting_until_the_next_one() {
        let now = MAR1_2000 + 100 * DAY;
        let e = json!({"boots": [
            {"boot": "a", "start": now - 6 * DAY, "clean": false},
            {"boot": "b", "start": now - 5 * DAY, "clean": false},
            {"boot": "c", "start": now - DAY},
        ]});
        // Marked reviewed with the newest unclean start: both are reviewed, nothing is left.
        assert_eq!(dirty_boots(&e, now, now - 5 * DAY), Dirty { boots: 3, unclean: 0, reviewed: 2, newest: now - 5 * DAY });
        // The running boot (started before the mark) crashes later: it counts again.
        let mut e2 = e.clone();
        e2["boots"][2]["clean"] = json!(false);
        assert_eq!(dirty_boots(&e2, now, now - 5 * DAY).unclean, 1);
    }

    #[test]
    fn battery_health_uses_the_newest_sample_with_a_design_value() {
        let s = json!({"capacity": [
            {"d": "2026-01-01", "full": 50.0, "design": 60.0},
            {"d": "2026-02-01", "full": 45.0},
        ]});
        let (pct, full, date) = battery_health(&s).unwrap();
        assert!((pct - 83.333).abs() < 0.01);
        assert_eq!((full, date.as_str()), (50.0, "2026-01-01"));
        assert!(battery_health(&json!({"capacity": []})).is_none());
    }

    fn cooling_run(t: i64, steady: f64, baseline: bool) -> Value {
        json!({"t": t, "key": "k", "ctx": {"limits": {"cpu_temp": 95}}, "start_temp": 40.0, "steady_temp": steady,
               "max_temp": steady + 1.0, "fan_rpm": 3000.0, "mhz": 4000.0, "noisy": false, "secs": 600, "baseline": baseline})
    }

    #[test]
    fn cooling_verdict_compares_with_the_baseline() {
        assert!(cooling_latest(&json!({"runs": []})).is_none());
        assert!(cooling_latest(&json!({"runs": [cooling_run(1, 70.0, true)]})).is_none());
        let ok = json!({"runs": [cooling_run(1, 70.0, true), cooling_run(2, 71.0, false)]});
        assert!(matches!(cooling_latest(&ok), Some((2, Level::Ok))));
        let warn = json!({"runs": [cooling_run(1, 70.0, true), cooling_run(2, 90.0, false)]});
        assert!(matches!(cooling_latest(&warn), Some((2, Level::Warn))));
    }

    #[test]
    fn sleep_stats_count_drain_on_battery_only() {
        let now = MAR1_2000 + 50 * DAY;
        let log = [
            // Long sleep on battery: 10 % over 10 h.
            json!({"start": now - 3600, "end": now - 3600 + 36000, "status_before": "Discharging", "energy_before": 50.0, "energy_after": 40.0, "full": 100.0}).to_string(),
            // Too short to judge drain, but still a sleep.
            json!({"start": now - 7200, "end": now - 7200 + 600, "status_before": "Discharging", "energy_before": 50.0, "energy_after": 49.0, "full": 100.0}).to_string(),
            // On AC, dGPU did not come back.
            json!({"start": now - 9000, "end": now - 9000 + 36000, "status_before": "Charging", "energy_before": 50.0, "energy_after": 10.0, "full": 100.0, "dgpu_after": "error"}).to_string(),
            // Older than a week: ignored.
            json!({"start": now - 30 * DAY, "end": now - 30 * DAY + 3600, "status_before": "Discharging", "energy_before": 50.0, "energy_after": 1.0, "full": 100.0}).to_string(),
            "{broken".to_owned(),
        ]
        .join("\n");
        assert_eq!(sleep_stats(&log, now), SleepStats { sleeps: 3, pct: 10.0, hours: 10.0, dgpu_errors: 1 });
    }

    #[test]
    fn dgpu_usage_share_wakes_and_power_difference() {
        let state = json!({
            "days": {"2026-10-29": {"bat": {"awake_ms": 3_600_000.0, "asleep_ms": 9_000_000.0}}},
            "wakes": [{"t": 1, "holders": "a"}, {"t": 2, "holders": "b"}],
            "power": {"awake": {"sec": 3600.0, "wh": 12.0}, "asleep": {"sec": 3600.0, "wh": 8.0}},
        });
        let u = dgpu_usage(&state, "2026-10-29");
        assert_eq!(u["today"]["awake_pct"], 100.0 * 3.6e6 / 1.26e7);
        assert_eq!(u["wakes"][0]["t"], 2, "newest first");
        assert_eq!(u["power"]["delta_w"], 4.0);
        // Too little time on one side: no comparison.
        let thin = json!({"power": {"awake": {"sec": 3600.0, "wh": 12.0}, "asleep": {"sec": 600.0, "wh": 1.0}}});
        assert!(dgpu_usage(&thin, "2026-10-29")["power"]["delta_w"].is_null());
        // No data: nothing to show.
        assert!(dgpu_usage(&Value::Null, "2026-10-29")["today"]["awake_pct"].is_null());
    }

    #[test]
    fn newest_age_reads_the_newest_file_and_skips_missing_folders() {
        let dir = std::env::temp_dir().join(format!("centurion-overview-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("a.tar.gz"), b"x").unwrap();
        let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs() as i64;
        let age = newest_age(&[PathBuf::from("/nonexistent-centurion-dir"), dir.clone()], now).unwrap();
        assert!((0..60).contains(&age), "{age}");
        assert!(newest_age(&[PathBuf::from("/nonexistent-centurion-dir")], now).is_none());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn cards_follow_the_hardware_and_the_levels() {
        let now = MAR1_2000 + 100 * DAY;
        let quiet = Inputs { now, today: ymd(now, 0), ..Default::default() };
        let c = cards(&quiet);
        let ids: Vec<&str> = c.iter().map(|x| x["id"].as_str().unwrap()).collect();
        assert_eq!(ids, ["kernel", "unclean", "cooling", "backup", "sleep"], "no battery or dGPU card without that hardware");
        assert_eq!(c[0]["level"], OK);
        assert_eq!(c[1]["level"], NODATA);
        assert_eq!(worst(&c), OK);

        let busy = Inputs {
            now,
            today: ymd(now, 0),
            health_log: line(&stamp(now - 60), "CRITICAL"),
            battery: Some(json!({"capacity": [{"d": "x", "full": 60.0, "design": 100.0}]})),
            backup_age: Some(40 * DAY),
            has_battery: true,
            has_dgpu: true,
            dgpu_state: Some(json!({"days": {ymd(now, 0): {"bat": {"awake_ms": 9.0e6, "asleep_ms": 1.0e6}}}})),
            ..Default::default()
        };
        let c = cards(&busy);
        let level = |id: &str| c.iter().find(|x| x["id"] == id).unwrap()["level"].as_i64().unwrap();
        assert_eq!(level("kernel"), BAD);
        assert_eq!(level("battery"), BAD);
        assert_eq!(level("backup"), BAD);
        assert_eq!(level("dgpu"), WARN);
        assert_eq!(worst(&c), BAD);
        assert!(c.iter().any(|x| x["id"] == "dgpu" && x["awake_pct"].as_f64().unwrap() > 85.0));
    }

    #[test]
    fn sleep_card_warns_on_fast_drain_and_on_a_failed_dgpu() {
        let now = MAR1_2000 + 100 * DAY;
        let fast = json!({"start": now - 3600, "end": now - 3600 + 36000, "status_before": "Discharging", "energy_before": 50.0, "energy_after": 20.0, "full": 100.0}).to_string();
        let level = |log: String| cards(&Inputs { now, sleep_log: log, ..Default::default() }).last().unwrap()["level"].as_i64().unwrap();
        assert_eq!(level(fast.clone()), WARN);
        assert_eq!(level(String::new()), NODATA);
        assert_eq!(level(format!("{fast}\n{}", json!({"start": now - 60, "end": now, "dgpu_after": "error"}))), BAD);
    }
}
