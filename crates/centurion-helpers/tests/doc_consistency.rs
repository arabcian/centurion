//! docs/AUTOTUNE.md states exact numbers (weights, margins, caps). This test keeps the document,
//! the code and the GUI's fallback copy of the weights from drifting apart.

use centurion_helpers::autotune::{Goal, Weights, MARGIN};

const DOC: &str = include_str!("../../../docs/AUTOTUNE.md");
const AUTOTUNE_RS: &str = include_str!("../src/autotune.rs");
const CALIB_RS: &str = include_str!("../src/calib.rs");
const GUI: &str = include_str!("../../../gui/src/optimizetab.cpp");

/// The value of `const NAME: f64 = <v>;` in a source file.
fn src_const(src: &str, name: &str) -> f64 {
    let pat = format!("const {name}: f64 = ");
    let rest = &src[src.find(&pat).unwrap_or_else(|| panic!("{name} not found")) + pat.len()..];
    rest[..rest.find(';').unwrap()].trim().parse().unwrap()
}

/// Squeezes whitespace so wrapped doc lines match.
fn flat(s: &str) -> String { s.split_whitespace().collect::<Vec<_>>().join(" ") }

fn nums(s: &str) -> Vec<f64> {
    s.split(|c: char| !(c.is_ascii_digit() || c == '.')).filter(|t| !t.is_empty()).map(|t| t.parse().unwrap()).collect()
}

const GOALS: [(Goal, &str, &str); 4] = [
    (Goal::Gaming, "Gaming", "gaming"), (Goal::Desktop, "Desktop", ""),
    (Goal::Throughput, "Throughput", "throughput"), (Goal::PowerSave, "Power save", "powersave"),
];

#[test]
fn weight_table_matches_code() {
    for (g, row, _) in GOALS {
        let line = DOC.lines().find(|l| l.starts_with(&format!("| {row} "))).unwrap_or_else(|| panic!("no doc row for {row}"));
        let doc = nums(line);
        let w = Weights::for_goal(g);
        // lat, thr, pwr, mem, stab, storage, heat, budget
        let code = [w.latency, w.throughput, w.power, w.footprint, w.stability, w.storage, w.heat, w.budget];
        assert_eq!(doc, code, "AUTOTUNE.md weight row {row}");
    }
}

#[test]
fn gui_fallback_weights_match_code() {
    let body = &GUI[GUI.find("fallbackWeights(const QString &goal)").expect("fallbackWeights")..];
    let body = &body[..body.find("\n}").unwrap()];
    for (g, _, key) in GOALS {
        let line = if key.is_empty() {
            body.lines().find(|l| l.trim_start().starts_with("return {")).unwrap()
        } else {
            body.lines().find(|l| l.contains(&format!("\"{key}\""))).unwrap()
        };
        let list = &line[line.find('{').unwrap()..];
        let w = Weights::for_goal(g);
        let code = [w.latency, w.throughput, w.power, w.footprint, w.stability, w.storage, w.load, w.heat, w.budget];
        assert_eq!(nums(list), code, "optimizetab.cpp fallbackWeights for {key:?}");
    }
}

#[test]
fn constants_match_doc() {
    let doc = flat(DOC);
    assert!(doc.contains(&format!("margin ({MARGIN:.2})")), "MARGIN {MARGIN} not in AUTOTUNE.md");
    let seen = src_const(AUTOTUNE_RS, "SEEN_Z");
    assert!(doc.contains(&format!("weighs by {seen} posterior standard deviations")), "SEEN_Z {seen} not in AUTOTUNE.md");
    let keep = src_const(AUTOTUNE_RS, "KEEP_Z");
    assert!(doc.contains(&format!("(mean + {keep} sd < 0")), "KEEP_Z {keep} not in AUTOTUNE.md");
    let caps = CALIB_RS.lines().find(|l| l.contains("const PHASE_ROWS")).expect("PHASE_ROWS");
    let caps = nums(&caps[caps.find('[').unwrap() + 1..]);
    let caps: Vec<f64> = caps[1..].to_vec(); // drop the array length
    let text = format!("at most {} idle / {} load / {} IO / {} DEV rows", caps[0], caps[1], caps[2], caps[3]);
    assert!(doc.contains(&text), "PHASE_ROWS {caps:?}: expected \"{text}\" in AUTOTUNE.md");
}
