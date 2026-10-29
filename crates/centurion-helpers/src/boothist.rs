//! Boot history for Health → Boot history: one row per boot Centurion has seen (exposure.json),
//! with how it ended, its kernel events, the curve states in effect (oc-journal.jsonl) and the
//! pstore records of the crash that ended it.
//!
//! Pure assembly (`build`, testable with fixtures); `load` only reads the root-owned files.
//! A boot is "clean" (closed by centurion-boot-guard at shutdown), "unclean" (the next guarded
//! boot found it never closed), "running" (the current boot) or "unknown" (left open: the guard did
//! not run in it, so nothing can be said). Kernel events are recorded only at a clean shutdown, so
//! an unclean boot has none; its last kernel-log lines come from the GUI's saved kernel.log.

use serde_json::{json, Map, Value};
use std::collections::BTreeMap;

const JOURNAL_MAX: u64 = 512 * 1024;
/// A pstore record this far from any unclean boot's lifetime is not attributed to it.
const LINK_SLACK_S: u64 = 24 * 3600;
const MAX_STATES: usize = 8;

fn now() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) }

pub fn load() -> Value {
    let journal = crate::read_root_file(crate::ocjournal::FILE, JOURNAL_MAX).unwrap_or_default();
    build(&crate::stability::load(), &journal, &crate::bootguard::boot_id(), now(), &crate::pstore::list())
}

/// Index (into `spans`: (start, next boot's start) of each unclean boot) of the boot a record written at
/// `t` belongs to: the one whose lifetime contains it, else the nearest within a day.
fn link_dirty(spans: &[(u64, u64)], t: u64) -> Option<usize> {
    let dist = |&(s, n): &(u64, u64)| if t < s { s - t } else if t >= n { t - n + 1 } else { 0 };
    spans.iter().enumerate().map(|(i, sp)| (dist(sp), i)).min().filter(|(d, _)| *d <= LINK_SLACK_S).map(|(_, i)| i)
}

/// Curve states per kind in the order they were applied during one boot: {"cpu_co": "profile:a → reset"}.
fn curve_states(journal: &[(String, String, String)], boot: &str) -> Value {
    let mut per: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
    for (b, kind, state) in journal {
        if b != boot { continue; }
        let v = per.entry(kind).or_default();
        if v.last() != Some(&state.as_str()) && v.len() < MAX_STATES { v.push(state); }
    }
    Value::Object(per.into_iter().map(|(k, v)| (k.to_owned(), json!(v.join(" → ")))).collect::<Map<_, _>>())
}

/// {"ok", "current", "boots": [newest first], "previous_unclean": {"boot","end"}|null, "pstore": {"available", "entries"}}.
/// boot: {"boot", "start", "end"?, "duration"?, "state", "events": {kind:level: n}, "event_total", "curves": {kind: states}, "pstore": [entry indexes]}.
/// Every pstore entry gains "boot" (the unclean boot it was attributed to) when there is one.
pub fn build(exposure: &Value, journal: &str, current: &str, now: u64, pstore: &Value) -> Value {
    let jl: Vec<(String, String, String)> = journal.lines().filter_map(|l| {
        let v: Value = serde_json::from_str(l).ok()?;
        Some((v["boot"].as_str()?.to_owned(), v["kind"].as_str()?.to_owned(), v["state"].as_str()?.to_owned()))
    }).collect();
    let mut src: Vec<&Value> = exposure["boots"].as_array().map(|a| a.iter().filter(|b| b["boot"].is_string()).collect()).unwrap_or_default();
    src.sort_by_key(|b| b["start"].as_u64().unwrap_or(0));

    let state_of = |b: &Value| match b["clean"].as_bool() {
        Some(true) => "clean",
        Some(false) => "unclean",
        None if b["boot"].as_str() == Some(current) => "running",
        None => "unknown",
    };
    let mut boots: Vec<Value> = Vec::new();
    let mut spans: Vec<(u64, u64)> = Vec::new();   // unclean boots only
    let mut span_boot: Vec<usize> = Vec::new();    // their index in `boots`
    for (i, b) in src.iter().enumerate() {
        let id = b["boot"].as_str().unwrap_or("");
        let (start, state) = (b["start"].as_u64().unwrap_or(0), state_of(b));
        let end = b["end"].as_u64();
        let events = b["events"].as_object().cloned().unwrap_or_default();
        let total: u64 = events.values().filter_map(Value::as_u64).sum();
        let mut o = json!({"boot": id, "start": start, "state": state, "events": events, "event_total": total,
                           "curves": curve_states(&jl, id), "pstore": []});
        if let Some(e) = end { o["end"] = json!(e); o["duration"] = json!(e.saturating_sub(start)); }
        else if state == "running" { o["duration"] = json!(now.saturating_sub(start)); }
        if state == "unclean" {
            spans.push((start, src.get(i + 1).and_then(|n| n["start"].as_u64()).unwrap_or(u64::MAX)));
            span_boot.push(boots.len());
        }
        boots.push(o);
    }

    let mut entries: Vec<Value> = pstore["entries"].as_array().cloned().unwrap_or_default();
    for (ei, e) in entries.iter_mut().enumerate() {
        let Some(si) = e["t"].as_u64().and_then(|t| link_dirty(&spans, t)) else { continue };
        let bi = span_boot[si];
        e["boot"] = boots[bi]["boot"].clone();
        boots[bi]["pstore"].as_array_mut().unwrap().push(json!(ei));
    }

    // The boot before the current one: the one an "unexpected end" banner is about.
    let prev = match boots.iter().position(|b| b["boot"] == json!(current)) { Some(i) => i.checked_sub(1), None => boots.len().checked_sub(1) };
    let previous_unclean = prev.map(|i| &boots[i]).filter(|b| b["state"] == "unclean")
        .map(|b| json!({"boot": b["boot"], "end": b["end"]}));

    boots.reverse();
    json!({"ok": true, "current": current, "boots": boots, "previous_unclean": previous_unclean,
           "pstore": {"available": pstore["available"].as_bool().unwrap_or(false), "entries": entries}})
}

#[cfg(test)]
mod tests {
    use super::*;

    fn exposure() -> Value {
        json!({"boots": [
            {"boot": "b1", "start": 1000, "end": 5000, "clean": true, "events": {"aer:warn": 3, "xid:critical": 1}, "segs": []},
            {"boot": "b2", "start": 10000, "end": 12000, "clean": false, "segs": []},
            {"boot": "b3", "start": 20000, "segs": []},
            {"boot": "b4", "start": 30000, "end": 31000, "clean": false, "segs": []},
            {"boot": "b5", "start": 40000, "segs": []},
        ]})
    }
    const JOURNAL: &str = concat!(
        "{\"t\":1100,\"boot\":\"b1\",\"kind\":\"cpu_co\",\"state\":\"profile:mild\"}\n",
        "{\"t\":1200,\"boot\":\"b1\",\"kind\":\"cpu_co\",\"state\":\"profile:mild\"}\n",
        "{\"t\":1300,\"boot\":\"b1\",\"kind\":\"cpu_co\",\"state\":\"reset\"}\n",
        "{\"t\":1400,\"boot\":\"b1\",\"kind\":\"gpu_curve\",\"state\":\"profile:oc\"}\n",
        "not json\n",
        "{\"t\":10100,\"boot\":\"b2\",\"kind\":\"cpu_co\",\"state\":\"profile:hard\"}\n");

    #[test]
    fn rows_states_and_curves() {
        let r = build(&exposure(), JOURNAL, "b5", 40600, &json!({"available": false, "entries": []}));
        let b = r["boots"].as_array().unwrap();
        assert_eq!(b.len(), 5);
        assert_eq!(b[0]["boot"], "b5");   // newest first
        assert_eq!(b[0]["state"], "running");
        assert_eq!(b[0]["duration"], 600);
        assert_eq!(b[1]["state"], "unclean");
        assert_eq!(b[2]["state"], "unknown");   // left open: the guard did not run
        assert!(b[2].get("duration").is_none());
        assert_eq!(b[3]["state"], "unclean");
        assert_eq!(b[3]["curves"]["cpu_co"], "profile:hard");
        assert_eq!(b[4]["state"], "clean");
        assert_eq!(b[4]["duration"], 4000);
        assert_eq!(b[4]["event_total"], 4);
        assert_eq!(b[4]["curves"]["cpu_co"], "profile:mild → reset");   // repeats collapsed
        assert_eq!(b[4]["curves"]["gpu_curve"], "profile:oc");
        assert_eq!(b[2]["curves"], json!({}));   // no journal lines = stock
        assert_eq!(r["previous_unclean"]["boot"], "b4");
        assert_eq!(r["previous_unclean"]["end"], 31000);
        assert_eq!(r["pstore"]["available"], false);
    }

    #[test]
    fn previous_unclean_only_when_the_boot_before_is() {
        let ok = build(&exposure(), "", "b3", 20100, &json!({}));
        assert_eq!(ok["previous_unclean"]["boot"], "b2");
        let none = build(&exposure(), "", "b2", 10100, &json!({}));
        assert!(none["previous_unclean"].is_null());   // b1 was clean
        // The current boot has no entry yet: the last one listed is the previous boot.
        let missing = build(&exposure(), "", "b6", 50000, &json!({}));
        assert!(missing["previous_unclean"].is_null());   // b5 is only "unknown"
        let empty = build(&json!({}), "", "b1", 1, &json!({}));
        assert_eq!(empty["boots"].as_array().unwrap().len(), 0);
        assert!(empty["previous_unclean"].is_null());
        let broken = build(&json!({"boots": [{"start": 1}, "x", {"boot": 5}]}), "garbage\n{", "b1", 1, &Value::Null);
        assert_eq!(broken["boots"].as_array().unwrap().len(), 0);
    }

    #[test]
    fn pstore_records_go_to_the_unclean_boot_they_belong_to() {
        let p = json!({"available": true, "entries": [
            {"name": "in-b2", "t": 11500},
            {"name": "gap-after-b2", "t": 15000},     // between b2's end and b3's start: still b2's lifetime
            {"name": "in-b4", "t": 30500},
            {"name": "near-b4", "t": 29800},          // before b4 started, within a day: nearest unclean boot
            {"name": "far-away", "t": 500000}         // more than a day from every unclean boot: not attributed
        ]});
        let r = build(&exposure(), "", "b5", 40100, &p);
        let e = r["pstore"]["entries"].as_array().unwrap();
        assert_eq!(e[0]["boot"], "b2");
        assert_eq!(e[1]["boot"], "b2");
        assert_eq!(e[2]["boot"], "b4");
        assert_eq!(e[3]["boot"], "b4");
        assert!(e[4].get("boot").is_none());
        let b = r["boots"].as_array().unwrap();
        assert_eq!(b[3]["pstore"], json!([0, 1]));
        assert_eq!(b[1]["pstore"], json!([2, 3]));
        assert_eq!(b[0]["pstore"], json!([]));
    }

    #[test]
    fn link_picks_the_containing_then_the_nearest() {
        let spans = [(100, 200), (200, 300)];
        assert_eq!(link_dirty(&spans, 150), Some(0));
        assert_eq!(link_dirty(&spans, 200), Some(1));
        assert_eq!(link_dirty(&spans, 99), Some(0));
        assert_eq!(link_dirty(&spans, 300 + LINK_SLACK_S + 1), None);
        assert_eq!(link_dirty(&[], 5), None);
        assert_eq!(link_dirty(&[(100, u64::MAX)], 5_000_000), Some(0));   // the last boot has no upper bound
    }
}
