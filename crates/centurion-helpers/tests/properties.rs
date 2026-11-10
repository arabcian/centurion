//! Property-based tests (proptest) for the pure validation / parsing code the root helpers rely on.
//!
//! The helpers write firmware, EC and MSR state, so the checks in front of those writes must hold
//! for *any* input, not only the handful a unit test names: a validator that accepts one value too
//! many, or panics on an odd string, is a hardware risk. Each test states one invariant.
//! (Validators private to a helper binary are tested inside that binary's `mod tests`.)

use centurion_helpers::cooling::{self, add_run, assess, baseline_for, forget, promote_latest, run_json, summarise, Context, Level, Metrics, Sample, MAX_RUNS};
use centurion_helpers::fan_table::{validate, TAIL_FLOORS};
use centurion_helpers::health::scrub;
use centurion_helpers::intel_uv::{amps_to_icc_field, mv_to_ticks_ex, parse_profile, ICC_MAX_FIELD, TICKS_PER_MV, UV_MAX_POSITIVE_MV, UV_MIN_MV};
use centurion_helpers::tune::{cpu_list, cpumask_hex, fmt_cpu_list, parse_cpumask, size_kib};
use centurion_helpers::fw_write_order;
use proptest::collection::{btree_set, vec};
use proptest::prelude::*;
use serde_json::{json, Value};
use std::collections::BTreeMap;

/// Arbitrary JSON, a few levels deep: what a hostile or buggy GUI could put on a helper's stdin.
fn any_json() -> impl Strategy<Value = Value> {
    let leaf = prop_oneof![
        Just(Value::Null),
        any::<bool>().prop_map(Value::Bool),
        any::<i64>().prop_map(|n| json!(n)),
        any::<u64>().prop_map(|n| json!(n)),
        any::<f64>().prop_filter("finite", |f| f.is_finite()).prop_map(|f| json!(f)),
        ".{0,12}".prop_map(Value::String),
    ];
    leaf.prop_recursive(3, 24, 4, |inner| {
        prop_oneof![
            vec(inner.clone(), 0..4).prop_map(Value::Array),
            vec(("[a-z_]{1,10}", inner), 0..4).prop_map(|kv| Value::Object(kv.into_iter().collect())),
        ]
    })
}

/// Mostly numbers around the interesting range (so the accepting branch runs often), sometimes
/// anything at all, including NaN and infinities.
fn near_range(r: std::ops::Range<f64>) -> impl Strategy<Value = f64> {
    prop_oneof![4 => r, 1 => any::<f64>()]
}


fn metrics() -> impl Strategy<Value = Metrics> {
    (20.0..60.0f64, 40.0..100.0f64, prop::option::of(0.0..9000.0f64), prop::option::of(400.0..6000.0f64))
        .prop_map(|(start_temp, steady_temp, fan_rpm, mhz)| Metrics { start_temp, steady_temp, max_temp: steady_temp + 2.0, fan_rpm, mhz, fan_spread: None })
}

fn rank(l: Level) -> u8 {
    match l { Level::Better => 0, Level::Ok => 1, Level::Watch => 2, Level::Warn => 3, Level::Inconclusive => 99 }
}

fn ctx(pl1: i64, target: Option<i64>) -> Context {
    let mut limits = BTreeMap::new();
    limits.insert("ppt_pl1_spl".to_owned(), pl1);
    if let Some(t) = target { limits.insert("cpu_temp".to_owned(), t); }
    Context { profile: Some("custom".into()), power: "ac".into(), limits, threads: 16 }
}

proptest! {
    #![proptest_config(ProptestConfig { failure_persistence: None, ..ProptestConfig::default() })]
    // ── fan table ────────────────────────────────────────────────────────────

    /// An accepted fan table is 10 levels in 1..=10, never decreasing, with the three hottest
    /// steps at or above their thermal floors. Inputs are mostly sorted and the right length, so
    /// that a good share of them reach the checks after the first (random vectors almost never do).
    #[test]
    fn fan_table_accepted_means_safe(raw in vec(0u8..=11, 9..=11), sort in 0u8..4) {
        let mut levels = raw;
        if sort != 0 { levels.sort_unstable(); }
        if validate(&levels).is_ok() {
            prop_assert_eq!(levels.len(), 10);
            prop_assert!(levels.iter().all(|v| (1..=10).contains(v)));
            prop_assert!(levels.windows(2).all(|w| w[0] <= w[1]));
            for (k, floor) in TAIL_FLOORS.iter().enumerate() {
                prop_assert!(levels[7 + k] >= *floor);
            }
        }
    }

    /// Tables that break only the floor of one tail step are refused.
    #[test]
    fn fan_table_floor_is_enforced(mut levels in vec(1u8..=10, 10), k in 0usize..3) {
        levels.sort_unstable();
        for (j, floor) in TAIL_FLOORS.iter().enumerate() { levels[7 + j] = levels[7 + j].max(*floor); }
        levels.sort_unstable();
        // Lower step 7+k below its floor, keeping the order (everything before it goes down too).
        let low = TAIL_FLOORS[k] - 1;
        for v in levels.iter_mut().take(7 + k + 1) { *v = (*v).min(low); }
        prop_assert!(validate(&levels).is_err(), "{:?}", levels);
    }

    /// Any table meeting those rules is accepted (the validator is not stricter than documented).
    #[test]
    fn fan_table_safe_means_accepted(mut levels in vec(1u8..=10, 10)) {
        levels.sort_unstable();
        for (k, floor) in TAIL_FLOORS.iter().enumerate() { levels[7 + k] = levels[7 + k].max(*floor); }
        levels.sort_unstable();
        prop_assert!(validate(&levels).is_ok());
    }

    // ── Intel undervolt ──────────────────────────────────────────────────────

    /// A voltage offset is accepted only inside [-1000 mV, 0] (or +50 mV with allow_positive), is
    /// finite, and converts to a tick count that fits the signed 11-bit field.
    #[test]
    fn undervolt_offset_bounds(mv in near_range(-1100.0..80.0), allow in any::<bool>()) {
        let hi = if allow { UV_MAX_POSITIVE_MV } else { 0.0 };
        match mv_to_ticks_ex(mv, allow) {
            Ok(t) => {
                prop_assert!(mv.is_finite() && mv >= UV_MIN_MV && mv <= hi);
                prop_assert!((-1024..=1023).contains(&t), "ticks {} outside the 11-bit field", t);
                prop_assert!((t as f64 / TICKS_PER_MV - mv).abs() <= 0.5 / TICKS_PER_MV + 1e-9);
            }
            Err(_) => prop_assert!(!mv.is_finite() || mv < UV_MIN_MV || mv > hi),
        }
    }

    /// IccMax never rounds up past the requested ceiling and always fits its 11-bit field.
    #[test]
    fn iccmax_never_exceeds_request(a in near_range(-5.0..600.0)) {
        if let Ok(f) = amps_to_icc_field(a) {
            prop_assert!((1..=ICC_MAX_FIELD).contains(&f));
            prop_assert!(f as f64 / 4.0 <= a);
        }
    }

    /// parse_profile answers Ok or Err for any JSON and never panics; an accepted profile keeps
    /// every voltage inside the documented range.
    #[test]
    fn profile_parser_total(v in any_json()) {
        if let Ok(p) = parse_profile(&v) {
            for (_, _, mv) in &p.voltage { prop_assert!(*mv >= UV_MIN_MV && *mv <= UV_MAX_POSITIVE_MV); }
            if !p.allow_positive { for (_, _, mv) in &p.voltage { prop_assert!(*mv <= 0.0); } }
        }
    }

    /// The same, with a well-formed shape and hostile numbers.
    #[test]
    fn profile_parser_hostile_numbers(mv in near_range(-1100.0..80.0), w in near_range(-5.0..700.0), t in -100i32..100) {
        let v = json!({"voltage": {"core": mv}, "power": {"pl1": {"watts": w}}, "tjoffset": t});
        if let Ok(p) = parse_profile(&v) {
            prop_assert!(p.voltage.iter().all(|(_, _, m)| *m <= 0.0 && *m >= UV_MIN_MV));
            prop_assert!(p.pl1.map_or(true, |p| p.watts >= 1.0));
            prop_assert!(p.tjoffset.map_or(true, |t| t <= 63));
        }
    }

    // ── firmware write order ─────────────────────────────────────────────────

    /// The order is a permutation of the request, whatever it contains.
    #[test]
    fn fw_order_is_a_permutation(items in vec((prop::sample::select(vec!["cpu_temp", "ppt_pl1_spl", "ppt_pl2_sppt", "ppt_pl3_fppt", "ppt_pl4_ipl", "ppt_cpu_cl", "gpu_temp", "other"]), prop::option::of(1i64..400), 1i64..400), 0..12)) {
        let items: Vec<(&str, Option<i64>, i64)> = items;
        let mut order = fw_write_order(&items);
        order.sort_unstable();
        prop_assert_eq!(order, (0..items.len()).collect::<Vec<_>>());
    }

    /// Starting from an ordered limit chain (SPL <= SPPT <= FPPT <= IPL) and moving to another
    /// ordered chain, no intermediate state breaks the order - the firmware silently drops a write
    /// that would.
    #[test]
    fn fw_order_keeps_chain_sorted(
        present in btree_set(0usize..4, 1..=4),
        mut cur in vec(1i64..400, 4),
        mut new in vec(1i64..400, 4),
    ) {
        const CHAIN: [&str; 4] = ["ppt_pl1_spl", "ppt_pl2_sppt", "ppt_pl3_fppt", "ppt_pl4_ipl"];
        cur.sort_unstable();
        new.sort_unstable();
        let idx: Vec<usize> = present.into_iter().collect();
        let items: Vec<(&str, Option<i64>, i64)> = idx.iter().map(|&i| (CHAIN[i], Some(cur[i]), new[i])).collect();
        let mut state: Vec<i64> = idx.iter().map(|&i| cur[i]).collect();
        for j in fw_write_order(&items) {
            state[j] = items[j].2;
            prop_assert!(state.windows(2).all(|w| w[0] <= w[1]), "order broken after writing {}: {:?}", items[j].0, state);
        }
    }

    // ── CPU lists and masks ──────────────────────────────────────────────────

    #[test]
    fn cpu_list_round_trip(cpus in vec(0usize..2048, 0..64)) {
        let mut want = cpus.clone();
        want.sort_unstable();
        want.dedup();
        prop_assert_eq!(cpu_list(&fmt_cpu_list(&cpus)), want);
    }

    #[test]
    fn cpumask_round_trip(cpus in vec(0usize..1024, 1..64)) {
        let mut want = cpus.clone();
        want.sort_unstable();
        want.dedup();
        prop_assert_eq!(parse_cpumask(&cpumask_hex(&cpus)), want);
    }

    /// Arbitrary text from sysfs never panics and never yields a huge expansion.
    #[test]
    fn cpu_text_parsers_total(s in ".{0,60}") {
        prop_assert!(cpu_list(&s).len() <= 4096 * 64);
        let _ = parse_cpumask(&s);
        let _ = size_kib(&s);
    }

    /// A cache size read from sysfs ("32768K", "96M", or garbage) never overflows or panics.
    #[test]
    fn size_kib_never_overflows(s in "[0-9]{0,22}[KM]?") {
        let _ = size_kib(&s);
    }

    // ── kernel-log scrubbing ─────────────────────────────────────────────────

    /// No 16+ digit hex run starting with ffff (a kernel address) survives scrubbing.
    #[test]
    fn scrub_removes_kernel_addresses(prefix in "[ a-z]{0,8}", tail in "[0-9a-f]{12,20}", suffix in "[ a-z]{0,8}") {
        let line = format!("{prefix}0xffff{tail}{suffix}");
        prop_assert!(!scrub(&line).to_ascii_lowercase().contains("ffff"), "{}", scrub(&line));
    }

    #[test]
    fn scrub_total(s in any::<String>()) {
        let _ = scrub(&s);
    }

    // ── cooling health ───────────────────────────────────────────────────────

    /// A hotter die never makes the verdict milder, whatever else the runs hold.
    #[test]
    fn cooling_hotter_is_never_milder(base in metrics(), now in metrics(), extra in 0.0..15.0f64, limit in 80.0..100.0f64) {
        let a = assess(&base, &now, limit, false, false);
        let hotter = Metrics { steady_temp: now.steady_temp + extra, max_temp: now.max_temp + extra, ..now };
        let b = assess(&base, &hotter, limit, false, false);
        if a.level != Level::Inconclusive {
            prop_assert!(rank(b.level) >= rank(a.level), "{:?} -> {:?}", a.level, b.level);
        }
    }

    /// A run compared with itself is fine; noise or a warm start is never a verdict.
    #[test]
    fn cooling_same_run_is_ok_noise_is_inconclusive(m in metrics(), limit in 80.0..100.0f64, warm in 6.0..30.0f64) {
        prop_assert_eq!(assess(&m, &m, limit, false, false).level, Level::Ok);
        prop_assert_eq!(assess(&m, &m, limit, true, false).level, Level::Inconclusive);
        prop_assert_eq!(assess(&m, &m, limit, false, true).level, Level::Inconclusive);
        let warm_run = Metrics { start_temp: m.start_temp + warm, ..m };
        prop_assert_eq!(assess(&m, &warm_run, limit, false, false).level, Level::Inconclusive);
    }

    /// The steady-state temperature is an average of readings: it stays between their extremes.
    #[test]
    fn cooling_summary_is_bounded(temps in vec(30.0..105.0f64, 1..200)) {
        let samples: Vec<Sample> = temps.iter().enumerate().map(|(i, t)| Sample { t: i as f64 * 2.0, temp: *t, fan: None, mhz: None, fans: vec![] }).collect();
        let m = summarise(&samples, 40.0).unwrap();
        let (lo, hi) = (temps.iter().cloned().fold(f64::MAX, f64::min), temps.iter().cloned().fold(f64::MIN, f64::max));
        prop_assert!(m.steady_temp >= lo - 1e-9 && m.steady_temp <= hi + 1e-9);
        prop_assert!((m.max_temp - hi).abs() < 1e-9);
    }

    /// Whatever sequence of runs, promotions and forgets: at most one baseline per setting, a
    /// baseline is never a noisy run, and the store never outgrows its cap.
    #[test]
    fn cooling_store_invariants(ops in vec((0u8..3, 0usize..3, 60.0..95.0f64, any::<bool>()), 0..150)) {
        let contexts = [ctx(80, Some(95)), ctx(110, Some(95)), ctx(80, Some(85))];
        let mut runs: Vec<Value> = Vec::new();
        for (i, (op, c, temp, noisy)) in ops.into_iter().enumerate() {
            let c = &contexts[c];
            match op {
                0 | 1 => add_run(&mut runs, run_json(i as i64, c, &Metrics { start_temp: 45.0, steady_temp: temp, max_temp: temp + 1.0, fan_rpm: None, mhz: None, fan_spread: None }, noisy, 240)),
                _ if i % 2 == 0 => { promote_latest(&mut runs, &c.key()); }
                _ => forget(&mut runs, &c.key()),
            }
            prop_assert!(runs.len() <= MAX_RUNS);
            for c in &contexts {
                let n = runs.iter().filter(|r| r["key"] == c.key().as_str() && r["baseline"] == true).count();
                prop_assert!(n <= 1);
            }
            prop_assert!(runs.iter().all(|r| r["baseline"] != true || r["noisy"] != true));
        }
        // A baseline lookup agrees with the flags.
        for c in &contexts { if let Some(b) = baseline_for(&runs, &c.key()) { prop_assert_eq!(&b["baseline"], &json!(true)); } }
    }

    /// Firmware settings decide comparability: any change to a limit changes the key, equal settings never do.
    #[test]
    fn cooling_key_tracks_limits(a in 5i64..250, b in 5i64..250, t1 in prop::option::of(70i64..100), t2 in prop::option::of(70i64..100)) {
        let (x, y) = (ctx(a, t1), ctx(b, t2));
        prop_assert_eq!(x.key() == y.key(), a == b && t1 == t2);
        let _ = cooling::DEFAULT_LIMIT_C;
    }
}

// ── signature.json (calib::Calibration) ──────────────────────────────────────────────────────

use centurion_helpers::calib::{Calibration, Phase, Row};

/// A signature as the calibration writes it: a few contexts and design rows (NaN objectives included).
fn any_signature() -> impl Strategy<Value = Calibration> {
    let row = (0usize..4, 0u64..20, vec(("[a-z]{1,3}\\.[a-z]{1,4}", "[0-9]{1,2}"), 0..4),
               vec(prop_oneof![Just(f64::NAN), -1.0f64..1.0], 4), 0.0f64..2.0, any::<bool>());
    (vec(row, 0..40), vec("[a-z]{1,8}", 0..3)).prop_map(|(rows, profiles)| {
        let mut c = Calibration::default();
        let cxs: Vec<String> = profiles.iter().map(|p| c.add_context(json!({"profile": p, "power": "ac"}))).collect();
        for (i, (ph, sess, cfg, y, w, field)) in rows.into_iter().enumerate() {
            c.rows.push(Row { phase: Phase::ALL[ph], sess, t: 1_700_000_000 + i as u64, cfg, y: [y[0], y[1], y[2], y[3]], w, field,
                              cx: cxs.get(i % cxs.len().max(1)).cloned().unwrap_or_default(), ..Default::default() });
        }
        c.note_predcheck("confirm", "idle", "gaming", 0.05, 0.01, 0.04);
        c
    })
}

proptest! {
    /// Whatever JSON the file holds (any version number, any shape), reading it never panics, and
    /// what it reads as is stable: written back and read again, it stays the same.
    #[test]
    fn signature_reader_survives_any_json(v in any_json(), version in prop_oneof![Just(None), (0u64..12).prop_map(Some)]) {
        let mut v = v;
        if let (Some(n), Some(o)) = (version, v.as_object_mut()) { o.insert("version".into(), json!(n)); }
        let c = Calibration::from_json(&v);
        let once = c.to_json();
        prop_assert_eq!(Calibration::from_json(&once).to_json(), once);
    }

    /// Hand-shaped old-format fields (rows, keys, quality, verify of the wrong types) do not panic.
    #[test]
    fn old_format_fields_of_the_wrong_type(version in 1u64..10, rows in any_json(), keys in any_json(), q in any_json()) {
        let v = json!({"version": version, "rows": rows, "keys": keys, "quality": q, "verify": q, "predcheck": q, "contexts": keys});
        let _ = Calibration::from_json(&v).to_json();
    }

    /// Random bytes: either not JSON (the loader's "damaged" path) or read without a panic.
    #[test]
    fn damaged_bytes_never_panic(b in vec(any::<u8>(), 0..256)) {
        if let Ok(v) = serde_json::from_slice::<Value>(&b) { let _ = Calibration::from_json(&v); }
    }

    /// A signature the calibration wrote round-trips: rows, contexts, prediction checks.
    #[test]
    fn signature_round_trip(c in any_signature()) {
        let j = c.to_json();
        let back = Calibration::from_json(&j);
        prop_assert_eq!(back.rows.len(), c.rows.len());
        prop_assert_eq!(&back.contexts, &c.contexts);
        prop_assert_eq!(&back.predcheck, &c.predcheck);
        prop_assert_eq!(back.to_json(), j);
    }
}
