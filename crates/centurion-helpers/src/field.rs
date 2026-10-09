//! Game sessions as field evidence: frame times a real game delivered under a known configuration.
//!
//! The calibration's game loops are synthetic. To see whether a preset helps a real game,
//! centurion-gamemode records every game session (start, end, the game, the Optimizations values it
//! ran with) in ~/.local/state/centurion/game-sessions.jsonl, and - opt-in - runs an
//! A/B comparison: every other launch of a game skips the game preset (A = boot defaults, the
//! game scene's power context stays the same). `centurion-calibrate --field LOG...` matches frame-time
//! logs to those sessions and turns each game's A/B sessions into rows of the load model:
//!
//!   throughput = -ln(median frame time / the game's A median)
//!   latency    = -(ln(tail / A tail) + ln(pacing / A pacing)) / 2
//!
//! Logs: FLM's CSV (interval_ns), MangoHud's CSV (frametime, ms), or one number per line (ms).
//! A game is its own session in the model (its own offset), field rows count half a benchmark
//! run, and only the knobs the calibration designed are kept (a row loses weight for every
//! changed setting the model does not know).

use crate::calib::{PowerSrc, Phase, Row};
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Weight of one field session against a benchmark run.
pub const FIELD_WEIGHT: f64 = 0.5;
/// Frames needed (after the first 10 % - loading, shader compilation - is dropped).
const MIN_FRAMES: usize = 300;

/// Frame-time figures of one session (ms).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Frames { pub med: f64, pub tail: f64, pub pacing: f64, pub n: usize }

/// Frame times (ms) from a log. FLM CSV: `interval_ns` column; MangoHud CSV: `frametime` (ms);
/// otherwise the first numeric column, taken as ms. Non-positive and absurd values are dropped.
pub fn parse_log(text: &str) -> Option<Vec<f64>> {
    let mut lines = text.lines().map(str::trim).filter(|l| !l.is_empty() && !l.starts_with('#'));
    let mut out = Vec::new();
    let (mut col, mut scale) = (0usize, 1.0f64);
    let mut header_found = false;
    let mut pending: Vec<&str> = Vec::new();
    // MangoHud writes two preamble lines (system info) before its header: look at the first few lines.
    for _ in 0..4 {
        let Some(l) = lines.next() else { break };
        let cells: Vec<&str> = l.split(',').map(str::trim).collect();
        if let Some(i) = cells.iter().position(|c| c.eq_ignore_ascii_case("interval_ns")) { col = i; scale = 1e-6; header_found = true; break; }
        if let Some(i) = cells.iter().position(|c| c.eq_ignore_ascii_case("frametime")) { col = i; scale = 1.0; header_found = true; break; }
        pending.push(l);
    }
    let all: Vec<&str> = if header_found { lines.collect() } else { pending.into_iter().chain(lines).collect() };
    for l in all {
        let Some(c) = l.split(',').nth(col) else { continue };
        let Ok(v) = c.trim().parse::<f64>() else { continue };
        let ms = v * scale;
        if ms > 0.05 && ms < 2000.0 { out.push(ms); }
    }
    (!out.is_empty()).then_some(out)
}

/// Median, tail (mean of the worst 1 %, at least 5 frames) and pacing (mean frame-to-frame change).
pub fn frames(ft: &[f64]) -> Option<Frames> {
    let skip = ft.len() / 10;
    let v = &ft[skip..];
    if v.len() < MIN_FRAMES { return None; }
    let pacing = v.windows(2).map(|w| (w[1] - w[0]).abs()).sum::<f64>() / (v.len() - 1) as f64;
    let mut s = v.to_vec();
    let med = crate::calib::median(&mut s)?;
    let k = (s.len() / 100).max(5);
    let tail = s[s.len() - k..].iter().sum::<f64>() / k as f64;
    Some(Frames { med, tail, pacing: pacing.max(1e-3), n: v.len() })
}

/// The session log of a user (written by centurion-gamemode).
pub fn session_log(home: &Path) -> PathBuf { home.join(".local/state/centurion/game-sessions.jsonl") }

/// Sessions from the log: start and end lines merged by id. {id, start, end?, game, preset?, values, ab?}
pub fn sessions(text: &str) -> Vec<Value> {
    let mut by: BTreeMap<String, Value> = BTreeMap::new();
    for l in text.lines() {
        let Ok(v) = serde_json::from_str::<Value>(l) else { continue };
        let Some(id) = v["id"].as_str().map(str::to_owned) else { continue };
        let e = by.entry(id).or_insert_with(|| json!({}));
        if let (Some(o), Some(n)) = (e.as_object_mut(), v.as_object()) { for (k, x) in n { o.insert(k.clone(), x.clone()); } }
    }
    by.into_values().filter(|v| v["start"].is_u64()).collect()
}

/// The session a log belongs to: the log was last written while the game ran or shortly after
/// it ended (`mtime`). The latest session that fits wins.
pub fn match_session<'a>(sessions: &'a [Value], mtime: u64) -> Option<&'a Value> {
    sessions.iter().filter(|s| {
        let st = s["start"].as_u64().unwrap_or(u64::MAX);
        let end = s["end"].as_u64().unwrap_or(st + 6 * 3600);
        st <= mtime && mtime <= end + 300
    }).max_by_key(|s| s["start"].as_u64().unwrap_or(0))
}

/// The configuration a session ran with, as changes against the boot defaults (`boot`): an A
/// session (preset skipped) ran at the defaults - empty.
pub fn session_cfg(s: &Value, boot: &dyn Fn(&str) -> Option<String>) -> Vec<(String, String)> {
    if s["ab"] == "A" || s["preset"].is_null() { return Vec::new(); }
    let mut cfg: Vec<(String, String)> = s["values"].as_object().into_iter().flatten().filter_map(|(k, v)| {
        let v = match v { Value::String(x) => x.clone(), x => x.to_string() };
        (boot(k).as_deref() != Some(v.as_str())).then(|| (k.clone(), v))
    }).collect();
    cfg.sort();
    cfg
}

/// Model session id of a game (its own offset in the load model).
pub fn game_sess(game: &str) -> u64 { (1u64 << 40) | (crate::model::fnv(game.as_bytes()) & 0xFF_FFFF_FFFF) }

/// Rows of every game that has at least two reference (A) sessions and one other: each session
/// against the median of the game's A sessions. `recs` = stored field sessions
/// {game, start, cfg: [[k, v]], med, tail, pacing, n}.
pub fn rows(recs: &[Value], kernel: &str) -> Vec<Row> {
    let mut by: BTreeMap<String, Vec<&Value>> = BTreeMap::new();
    for r in recs { if let Some(g) = r["game"].as_str() { by.entry(g.to_owned()).or_default().push(r); } }
    let cfg_of = |r: &Value| -> Vec<(String, String)> { r["cfg"].as_array().into_iter().flatten()
        .filter_map(|x| Some((x[0].as_str()?.to_owned(), x[1].as_str()?.to_owned()))).collect() };
    let mut out = Vec::new();
    for (game, rs) in by {
        let refs: Vec<&&Value> = rs.iter().filter(|r| cfg_of(r).is_empty()).collect();
        if refs.len() < 2 || rs.len() < 3 { continue; }
        let med = |f: &str| { let mut v: Vec<f64> = refs.iter().filter_map(|r| r[f].as_f64()).filter(|x| *x > 0.0).collect(); crate::calib::median(&mut v) };
        let (Some(m0), Some(t0), Some(p0)) = (med("med"), med("tail"), med("pacing")) else { continue };
        let sess = game_sess(&game);
        let start0 = rs.iter().filter_map(|r| r["start"].as_u64()).min().unwrap_or(0);
        let span = rs.iter().filter_map(|r| r["start"].as_u64()).max().unwrap_or(0).saturating_sub(start0).max(1) as f64;
        for r in &rs {
            let (Some(m), Some(t), Some(p)) = (r["med"].as_f64(), r["tail"].as_f64(), r["pacing"].as_f64()) else { continue };
            if m <= 0.0 || t <= 0.0 || p <= 0.0 { continue; }
            let thr = -(m / m0).ln();
            let lat = -((t / t0).ln() + (p / p0).ln()) / 2.0;
            let st = r["start"].as_u64().unwrap_or(0);
            out.push(Row { phase: Phase::Load, sess, pos: (st - start0) as f64 / span, t: st, kernel: kernel.to_owned(), cfg: cfg_of(r),
                           y: [lat.clamp(-0.5, 0.5), thr.clamp(-0.5, 0.5), f64::NAN, f64::NAN], w: FIELD_WEIGHT,
                           bv: crate::calib::BENCH_VERSION, src: PowerSrc::Rapl, cx: String::new(), tc: f64::NAN, field: true });
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn logs_parse_in_all_three_shapes() {
        let flm = "frame,flip_ns,interval_ns,slot_mean_ns\n1,100,4000000,4100000\n2,200,4200000,4100000\n";
        assert_eq!(parse_log(flm).unwrap(), vec![4.0, 4.2]);
        let mango = "os,cpu,gpu\nLinux,Ryzen,RTX\nfps,frametime,cpu_load\n240,4.1,30\n238,4.2,31\n";
        assert_eq!(parse_log(mango).unwrap(), vec![4.1, 4.2]);
        assert_eq!(parse_log("8.3\n8.4\n#c\n\n0\n").unwrap(), vec![8.3, 8.4]);
        assert!(parse_log("a,b\nx,y\n").is_none());
    }

    #[test]
    fn frames_and_rows_against_the_reference_sessions() {
        let ft: Vec<f64> = (0..1000).map(|i| if i % 100 == 99 { 20.0 } else { 8.0 }).collect();
        let f = frames(&ft).unwrap();
        assert!((f.med - 8.0).abs() < 1e-9 && f.tail > 8.0 && f.pacing > 0.0 && f.n == 900);
        assert!(frames(&ft[..200]).is_none());
        let rec = |start: u64, cfg: Value, med: f64| json!({"game": "g", "start": start, "cfg": cfg, "med": med, "tail": med * 2.0, "pacing": 0.5});
        let recs = vec![rec(1, json!([]), 10.0), rec(2, json!([]), 10.0), rec(3, json!([["cpu.epp", "performance"]]), 9.0)];
        let rs = rows(&recs, "7.2");
        assert_eq!(rs.len(), 3);
        let b = rs.iter().find(|r| !r.cfg.is_empty()).unwrap();
        assert!((b.y[1] - (10.0f64 / 9.0).ln()).abs() < 1e-9 && b.y[2].is_nan() && b.field && b.phase == Phase::Load);
        assert!(rows(&recs[..2], "7.2").is_empty(), "needs a non-reference session");
    }

    #[test]
    fn sessions_merge_and_match() {
        let log = "{\"id\":\"a\",\"start\":100,\"game\":\"x\",\"preset\":\"P\",\"values\":{\"cpu.epp\":\"performance\",\"vm.swappiness\":60},\"ab\":\"B\"}\n\
                   {\"id\":\"a\",\"end\":900}\n{\"id\":\"b\",\"start\":1000,\"game\":\"x\",\"preset\":null,\"values\":{},\"ab\":\"A\"}\n";
        let s = sessions(log);
        assert_eq!(s.len(), 2);
        assert_eq!(match_session(&s, 950).unwrap()["id"], "a");
        assert_eq!(match_session(&s, 2000).unwrap()["id"], "b");
        assert!(match_session(&s, 50).is_none());
        let boot = |k: &str| Some(if k == "vm.swappiness" { "60".to_owned() } else { "balance_performance".to_owned() });
        assert_eq!(session_cfg(&s[0], &boot), vec![("cpu.epp".to_owned(), "performance".to_owned())]);
        assert!(session_cfg(&s[1], &boot).is_empty());
    }
}
