//! Decision-quality regression on synthetic machines (roadmap B1).
//!
//! A scenario is a machine with known truth: per knob value an effect vector over the four
//! objectives (latency, throughput, power, memory), pair interactions, dose-response ladders with
//! a peak, and "blind" effects the benchmarks cannot see. Runs are observed through session
//! offsets, OU drift, Student-t (ν = 3) noise with 2 % outliers, a start temperature that rises
//! with the design order, an unobserved power-budget state and a periodic parasite.
//!
//! The design and the decisions go through the production code: `model::initial_design`,
//! `PhaseSet` fits, `analyse` (doubt-driven picks, per-goal optima) and autotune's `seen_in`.
//!
//! `cargo test --release -p centurion-helpers sim_regression -- --ignored --nocapture` compares the
//! metrics with `tests/sim-baseline.json` (±0.03 absolute each); `CENTURION_SIM_BASELINE=write`
//! rewrites the file (after a deliberate change to the statistics).
#![allow(dead_code)]

use super::*;
use centurion_helpers::model::PhaseSet;

/// Value-level effect: (key, value) -> effect per objective (relative, + = better).
type Eff = BTreeMap<(String, String), [f64; 4]>;

pub struct Noise {
    /// Per-objective run noise (sd).
    pub sd: [f64; 4],
    /// Heavy tails: Student-t with ν = 3 instead of normal, and this share of outlier runs (± 6 sd).
    pub heavy: bool,
    pub outliers: f64,
    /// Per-session offset (sd) and within-session OU drift (mean reversion per run, step sd).
    pub sess_sd: f64,
    pub ou: (f64, f64),
    /// Effect of the start temperature (0 cool .. 1 hot, rises with the design order) per objective;
    /// the temperature itself is recorded in the row (`tc`), as the real runner does.
    pub temp: [f64; 4],
    /// Unobserved power-budget exhaustion: effect on throughput and power when the budget is spent;
    /// it drains with every run and refills at a reference run (PL1 tau / STAPM-like).
    pub budget: f64,
    /// Periodic parasite: every `period` runs, two runs lose `amp` on latency and throughput.
    pub parasite: Option<(usize, f64)>,
}

impl Noise {
    fn iid(sd: f64) -> Noise { Noise { sd: [sd; 4], heavy: false, outliers: 0.0, sess_sd: 0.0, ou: (0.0, 0.0), temp: [0.0; 4], budget: 0.0, parasite: None } }
}

pub struct Scenario {
    pub name: &'static str,
    pub factors: Vec<Factor>,
    pub main: Eff,
    pub pairs: Vec<((String, String), (String, String), [f64; 4])>,
    /// Effects the benchmarks cannot see (they count in the truth, never in a row).
    pub blind: Eff,
    /// Objectives the benchmarks measure (others are NaN in the rows).
    pub measured: [bool; 4],
    pub noise: Noise,
    pub budget: usize,
    pub sessions: usize,
}

fn fac(k: &str, r: &str, v: &[&str]) -> Factor { Factor { key: k.into(), reference: r.into(), values: v.iter().map(|s| s.to_string()).collect() } }
fn kv(k: &str, v: &str) -> (String, String) { (k.into(), v.into()) }

impl Scenario {
    /// The original 16-knob, single-objective, iid machine of `sim` (same truth, x2 as in `sim::noisy`).
    pub fn classic() -> Scenario {
        let o = |x: f64| [2.0 * x, 0.0, 0.0, 0.0];
        let main: Eff = [(kv("vm.a", "1"), 0.05), (kv("vm.b", "1"), 0.04), (kv("vm.c", "2"), 0.03), (kv("vm.c", "4"), 0.06), (kv("cpu.a", "1"), 0.05),
                         (kv("cpu.b", "1"), 0.03), (kv("vm.d", "1"), 0.06), (kv("vm.e", "1"), 0.06), (kv("cpu.d", "1"), -0.05), (kv("vm.f", "1"), -0.04),
                         (kv("cpu.e", "1"), 0.03)].into_iter().map(|(k, x)| (k, o(x))).collect();
        let pairs = vec![(kv("vm.a", "1"), kv("vm.b", "1"), o(0.08)), (kv("cpu.a", "1"), kv("cpu.c", "1"), o(0.06)), (kv("vm.c", "4"), kv("cpu.a", "1"), o(0.05)),
                         (kv("vm.d", "1"), kv("vm.e", "1"), o(-0.06)), (kv("cpu.b", "1"), kv("cpu.e", "1"), o(-0.08))];
        Scenario { name: "classic", factors: super::sim::factors(), main, pairs, blind: Eff::new(), measured: [true, false, false, false],
                   noise: Noise::iid(0.05), budget: 100, sessions: 1 }
    }

    /// 48 knobs (binary, unordered choices, numeric ladders with a peaked dose response), four
    /// objectives with trade-offs, interactions, blind effects and every disturbance of `Noise`.
    pub fn realistic(seed: u64) -> Scenario {
        let mut rng = Rng::new(seed);
        let gauss = |rng: &mut Rng| normal(rng);
        let mut factors = Vec::new();
        let mut main = Eff::new();
        let vec_for = |rng: &mut Rng| -> [f64; 4] {
            let mut e = [0.0; 4];
            if rng.unit() < 0.4 { return e; } // null knob
            let p = rng.below(4);
            e[p] = 0.08 * gauss(rng); // the scale of `classic` (effects x2)
            if rng.unit() < 0.3 { let q = (p + 1 + rng.below(3)) % 4; e[q] = -0.5 * e[p]; } // trade-off
            e
        };
        for i in 0..30 {
            let k = format!("b{i:02}");
            main.insert((k.clone(), "1".into()), vec_for(&mut rng));
            factors.push(fac(&k, "0", &["1"]));
        }
        for i in 0..10 {
            let k = format!("c{i:02}");
            for v in ["b", "c"] { main.insert((k.clone(), v.into()), vec_for(&mut rng)); }
            factors.push(fac(&k, "a", &["b", "c"]));
        }
        let ladder = ["1", "2", "8", "16", "32"];
        for i in 0..8 {
            let k = format!("n{i:02}");
            let shape = vec_for(&mut rng);
            let peak = rng.unit() * 5.0;
            let g = |v: f64| (-(v.log2() - peak).powi(2) / 2.0).exp();
            for v in ladder { main.insert((k.clone(), v.into()), shape.map(|x| 2.0 * x * (g(v.parse().unwrap()) - g(4.0)))); }
            factors.push(fac(&k, "4", &ladder));
        }
        let live: Vec<(String, String)> = main.iter().filter(|(_, e)| e.iter().any(|x| *x != 0.0)).map(|(k, _)| k.clone()).collect();
        let mut pairs = Vec::new();
        for _ in 0..8 {
            let (a, b) = (live[rng.below(live.len())].clone(), live[rng.below(live.len())].clone());
            if a.0 == b.0 { continue; }
            let mut e = [0.0; 4];
            e[rng.below(4)] = if rng.unit() < 0.5 { 0.06 } else { -0.06 };
            pairs.push((a, b, e));
        }
        let mut blind = Eff::new();
        for _ in 0..4 {
            let k = live[rng.below(live.len())].clone();
            let mut e = [0.0; 4];
            e[2] = -0.08; // e.g. a power cost no sub-second benchmark sees
            blind.insert(k, e);
        }
        Scenario { name: "realistic", factors, main, pairs, blind, measured: [true; 4],
                   noise: Noise { sd: [0.04, 0.02, 0.03, 0.02], heavy: true, outliers: 0.02, sess_sd: 0.03, ou: (0.1, 0.01),
                                  temp: [0.02, -0.03, -0.02, 0.0], budget: 0.03, parasite: Some((17, 0.08)) },
                   budget: 240, sessions: 3 }
    }

    fn sum(&self, eff: &Eff, cfg: &Cfg) -> [f64; 4] {
        let mut y = [0.0; 4];
        for kv in cfg { if let Some(e) = eff.get(kv) { for o in 0..4 { y[o] += e[o]; } } }
        y
    }
    /// What the benchmarks can see of `cfg` (per objective, relative to the reference).
    pub fn visible(&self, cfg: &Cfg) -> [f64; 4] {
        let mut y = self.sum(&self.main, cfg);
        for (a, b, e) in &self.pairs { if cfg.contains(a) && cfg.contains(b) { for o in 0..4 { y[o] += e[o]; } } }
        y
    }
    /// The truth: visible plus blind effects.
    pub fn truth(&self, cfg: &Cfg) -> [f64; 4] {
        let (v, b) = (self.visible(cfg), self.sum(&self.blind, cfg));
        std::array::from_fn(|o| v[o] + b[o])
    }
    /// A goal's true net utility: its weights over the truth, plus the modesty cost of the changes.
    pub fn net(&self, g: Goal, cfg: &Cfg) -> f64 {
        let (w, t) = (goal_wts(g, Phase::Load), self.truth(cfg));
        (0..4).map(|o| w[o] * t[o]).sum::<f64>() + self.modesty(cfg)
    }
    fn modesty(&self, cfg: &Cfg) -> f64 {
        cfg.iter().map(|(k, v)| self.factors.iter().find(|f| f.key == *k).map_or(0.0, |f| model::modest_cost(&f.reference, v))).sum()
    }
    /// Best configuration of the truth for goal `g`: coordinate search with restarts.
    pub fn optimum(&self, g: Goal) -> Cfg {
        let mut rng = Rng::new(1);
        let mut best: (f64, Cfg) = (f64::MIN, Vec::new());
        for r in 0..24 {
            let mut c: Cfg = Vec::new();
            if r > 0 { for f in &self.factors { if rng.unit() < 0.3 { c.push((f.key.clone(), f.values[rng.below(f.values.len())].clone())); } } }
            loop {
                let mut imp = false;
                for f in &self.factors {
                    for v in std::iter::once(f.reference.clone()).chain(f.values.iter().cloned()) {
                        let mut d: Cfg = c.iter().filter(|(k, _)| *k != f.key).cloned().collect();
                        if v != f.reference { d.push((f.key.clone(), v)); }
                        if self.net(g, &d) > self.net(g, &c) + 1e-12 { c = d; imp = true; }
                    }
                }
                if !imp { break; }
            }
            let u = self.net(g, &c);
            if u > best.0 { best = (u, c); }
        }
        best.1
    }
    /// Knobs with no effect at all (truth and interactions): what a "seen" verdict must not flag.
    pub fn null_keys(&self) -> Vec<String> {
        self.factors.iter().filter(|f| {
            let touch = |(k, _): &(String, String)| *k == f.key;
            !self.main.iter().any(|(kv, e)| touch(kv) && e.iter().any(|x| *x != 0.0)) && !self.blind.keys().any(touch)
                && !self.pairs.iter().any(|(a, b, _)| touch(a) || touch(b))
        }).map(|f| f.key.clone()).collect()
    }
}

fn normal(rng: &mut Rng) -> f64 { (-2.0 * (1.0 - rng.unit()).ln()).sqrt() * (std::f64::consts::TAU * rng.unit()).cos() }
/// Student-t with ν = 3, scaled to unit sd (the sd of t3 is √3).
fn t3(rng: &mut Rng) -> f64 { let chi = (0..3).map(|_| normal(rng).powi(2)).sum::<f64>(); normal(rng) / (chi / 3.0).sqrt() / 3f64.sqrt() }

/// The observation process of one design (sessions, drift, temperature, budget, parasite).
struct Bench<'a> { sc: &'a Scenario, rng: Rng, rows: Vec<model::Row>, sess: u64, off: [f64; 4], ou: [f64; 4], budget: f64, n: usize, in_sess: usize }

impl<'a> Bench<'a> {
    fn new(sc: &'a Scenario, seed: u64) -> Bench<'a> { Bench { sc, rng: Rng::new(seed), rows: Vec::new(), sess: 0, off: [0.0; 4], ou: [0.0; 4], budget: 1.0, n: 0, in_sess: 0 } }
    fn session(&mut self) {
        self.sess += 1;
        self.in_sess = 0;
        let sd = self.sc.noise.sess_sd;
        self.off = std::array::from_fn(|_| sd * normal(&mut self.rng));
        self.ou = [0.0; 4];
        self.budget = 1.0;
    }
    fn run(&mut self, cfg: &Cfg) {
        let nz = &self.sc.noise;
        let per = (self.sc.budget / self.sc.sessions).max(1);
        let tc = (self.in_sess as f64 / per as f64).min(1.0) * 0.8 + 0.2 * self.rng.unit();
        let parasite = nz.parasite.is_some_and(|(p, _)| self.n % p < 2);
        let outlier = self.rng.unit() < nz.outliers;
        let vis = self.sc.visible(cfg);
        let mut y = [f64::NAN; 4];
        for o in 0..4 {
            self.ou[o] += -nz.ou.0 * self.ou[o] + nz.ou.1 * normal(&mut self.rng);
            let e = if nz.heavy { t3(&mut self.rng) } else { normal(&mut self.rng) };
            let mut v = vis[o] + self.off[o] + self.ou[o] + nz.temp[o] * tc + nz.sd[o] * e;
            if o == 1 || o == 2 { v -= nz.budget * (1.0 - self.budget); }
            if parasite && o < 2 { v -= nz.parasite.map_or(0.0, |(_, a)| a); }
            if outlier { v += 6.0 * nz.sd[o] * if self.rng.unit() < 0.5 { 1.0 } else { -1.0 }; }
            if self.sc.measured[o] { y[o] = v; }
        }
        self.budget = if cfg.is_empty() { 1.0 } else { self.budget * 0.85 };
        self.rows.push(model::Row { cfg: cfg.clone(), y, w: 1.0, sess: self.sess, pos: (self.in_sess as f64 / per as f64).min(1.0), t: self.n as f64 * 6.0, tc });
        self.n += 1;
        self.in_sess += 1;
    }
}

/// Fits of the rows so far (the production path).
fn fit(fs: &[Factor], rows: &[model::Row]) -> Option<(PhaseSet, [Option<Arc<model::Fit>>; 4])> {
    let ps = PhaseSet::build(fs.to_vec(), rows.to_vec())?;
    let fits = std::array::from_fn(|o| ps.fit_obj(o, None, 1));
    Some((ps, fits))
}

fn analysis(fs: &[Factor], rows: &[model::Row], want: usize, rng: &mut Rng) -> Option<(Analysis, PhaseSet, [Option<Arc<model::Fit>>; 4])> {
    let (ps, fits) = fit(fs, rows)?;
    let an = analyse(goal_models(&ps.space, &fits, Phase::Load), Phase::Load, fs, &[], want, 0, 6, rng, &Vec::new(), &Choice { cost: &|_| 1.0, heat: false })?;
    Some((an, ps, fits))
}

/// One calibration: per session reference runs (one every 6), a space-filling start in the first
/// session, doubt-driven picks after; then the final decisions.
fn calibrate(sc: &Scenario, seed: u64) -> Option<(Analysis, PhaseSet, [Option<Arc<model::Fit>>; 4])> {
    let mut b = Bench::new(sc, seed);
    let mut rng = Rng::new(seed ^ 0xD5);
    let per = sc.budget / sc.sessions;
    for s in 0..sc.sessions {
        b.session();
        b.run(&Vec::new());
        let end = b.n + per - 1;
        if s == 0 {
            for (i, c) in model::initial_design(&sc.factors, (per as f64 * 0.8) as usize, &mut rng).into_iter().enumerate() {
                b.run(&c);
                if i % 6 == 5 { b.run(&Vec::new()); }
            }
        }
        while b.n + 6 <= end {
            let Some((an, _, _)) = analysis(&sc.factors, &b.rows, 5, &mut rng) else { break };
            let picks = if an.picks.is_empty() { model::initial_design(&sc.factors, 5, &mut rng) } else { an.picks };
            for c in picks { b.run(&c); }
            b.run(&Vec::new());
        }
    }
    analysis(&sc.factors, &b.rows, 0, &mut rng)
}

/// Per goal: wrong-decision rate over the knobs and regret; overall: 90 % interval coverage of the
/// optima's predicted gain (against what the benchmarks can see) and the false "seen" rate.
#[derive(Debug, Default)]
pub struct Metrics { pub wrong: [f64; 4], pub regret: [f64; 4], pub cover: f64, pub false_seen: f64 }

fn value_of(cfg: &Cfg, k: &str) -> Option<String> { cfg.iter().find(|(a, _)| a == k).map(|(_, v)| v.clone()) }

pub fn evaluate(sc: &Scenario, seeds: &[u64]) -> Metrics {
    let mut m = Metrics::default();
    let (mut cov, mut cov_n, mut seen, mut seen_n) = (0.0, 0.0, 0.0, 0.0);
    let nulls = sc.null_keys();
    let opt: Vec<Cfg> = Goal::ALL.iter().map(|g| sc.optimum(*g)).collect();
    for &seed in seeds {
        let Some((an, ps, fits)) = calibrate(sc, seed) else { continue };
        for (gi, g) in Goal::ALL.iter().enumerate() {
            let Some((_, cfg, mu, sd)) = an.optima.iter().find(|o| o.0 == *g) else { continue };
            let wrong = sc.factors.iter().filter(|f| value_of(cfg, &f.key) != value_of(&opt[gi], &f.key)).count();
            m.wrong[gi] += wrong as f64 / sc.factors.len() as f64 / seeds.len() as f64;
            m.regret[gi] += (sc.net(*g, &opt[gi]) - sc.net(*g, cfg)) / seeds.len() as f64;
            let (w, v) = (goal_wts(*g, Phase::Load), sc.visible(cfg));
            let gain: f64 = (0..4).filter(|o| sc.measured[*o]).map(|o| w[o] * v[o]).sum();
            if *sd > 0.0 { cov += f64::from((gain - mu).abs() <= 1.645 * sd); cov_n += 1.0; }
        }
        if let Some(model) = Model::new(ps.space.clone(), fits.clone(), [1.0; 4]) {
            let j = Joint::new(None, Some(model), None, 1.0);
            for k in &nulls { seen += f64::from(centurion_helpers::autotune::seen_in(&j, k)); seen_n += 1.0; }
        }
    }
    m.cover = if cov_n > 0.0 { cov / cov_n } else { f64::NAN };
    m.false_seen = if seen_n > 0.0 { seen / seen_n } else { 0.0 };
    m
}

impl Metrics {
    fn to_json(&self) -> Value {
        let r = |x: f64| (x * 1000.0).round() / 1000.0;
        json!({"wrong": self.wrong.map(r), "regret": self.regret.map(r), "cover": r(self.cover), "false_seen": r(self.false_seen)})
    }
}

const TOL: f64 = 0.03;

#[test]
#[ignore]
fn sim_regression() {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/sim-baseline.json");
    let scenarios = [(Scenario::classic(), vec![100, 101, 102]), (Scenario::realistic(7), vec![1, 2, 3])];
    let mut out = serde_json::Map::new();
    eprintln!("{:<10} {:<10} {:>8} {:>8} {:>8} {:>8}", "scenario", "goal", "wrong", "regret", "cover90", "false seen");
    for (sc, seeds) in &scenarios {
        let m = evaluate(sc, seeds);
        for (gi, g) in Goal::ALL.iter().enumerate() {
            eprintln!("{:<10} {:<10} {:>8.3} {:>8.3} {:>8.3} {:>8.3}", sc.name, g.key(), m.wrong[gi], m.regret[gi], m.cover, m.false_seen);
        }
        out.insert(sc.name.into(), m.to_json());
    }
    let now = Value::Object(out);
    if std::env::var("CENTURION_SIM_BASELINE").as_deref() == Ok("write") || !path.exists() {
        std::fs::write(&path, serde_json::to_string_pretty(&now).unwrap() + "\n").unwrap();
        eprintln!("baseline written to {}", path.display());
        return;
    }
    let base: Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
    let mut bad = Vec::new();
    for (sc, m) in now.as_object().unwrap() {
        for (k, v) in m.as_object().unwrap() {
            let (a, b) = (v.as_array().cloned().unwrap_or_else(|| vec![v.clone()]), base[sc][k].as_array().cloned().unwrap_or_else(|| vec![base[sc][k].clone()]));
            for (i, (x, y)) in a.iter().zip(&b).enumerate() {
                let (x, y) = (x.as_f64().unwrap_or(f64::NAN), y.as_f64().unwrap_or(f64::NAN));
                if !((x - y).abs() <= TOL || x.is_nan() && y.is_nan()) { bad.push(format!("{sc}.{k}[{i}]: {x:.3} vs baseline {y:.3}")); }
            }
        }
    }
    assert!(bad.is_empty(), "decision quality moved beyond ±{TOL} (CENTURION_SIM_BASELINE=write to accept):\n{}", bad.join("\n"));
}

#[test]
fn scenarios_are_well_formed() {
    let sc = Scenario::realistic(7);
    assert_eq!(sc.factors.len(), 48);
    assert!(sc.null_keys().len() >= 5, "null knobs exist for the false-seen rate");
    assert!(!sc.blind.is_empty() && sc.pairs.len() >= 4);
    // A ladder has a peak somewhere: not every value moves the same way.
    let lad = &sc.factors[40];
    assert_eq!(lad.values.len(), 5);
    // Truth = visible + blind; the classic scenario reproduces `sim::truth` (x2 on latency).
    let c = Scenario::classic();
    for cfg in [vec![kv("vm.a", "1"), kv("vm.b", "1")], vec![kv("vm.c", "4"), kv("cpu.a", "1"), kv("cpu.c", "1")], vec![kv("cpu.b", "1"), kv("cpu.e", "1")]] {
        assert!((c.truth(&cfg)[0] - 2.0 * super::sim::truth(&cfg)).abs() < 1e-12, "{cfg:?}");
    }
    let opt = c.optimum(Goal::Gaming);
    assert!(c.net(Goal::Gaming, &opt) > c.net(Goal::Gaming, &Vec::new()));
    // The observation process is deterministic per seed.
    let (mut a, mut b) = (Bench::new(&sc, 5), Bench::new(&sc, 5));
    a.session(); b.session();
    for _ in 0..20 { a.run(&vec![kv("b00", "1")]); b.run(&vec![kv("b00", "1")]); }
    assert_eq!(a.rows.iter().map(|r| r.y[0].to_bits()).collect::<Vec<_>>(), b.rows.iter().map(|r| r.y[0].to_bits()).collect::<Vec<_>>());
    let ys: Vec<f64> = a.rows.iter().map(|r| r.y[1]).collect();
    assert!(ys.windows(2).any(|w| w[0] != w[1]), "noise present");
}
