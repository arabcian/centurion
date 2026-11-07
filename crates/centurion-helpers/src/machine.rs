//! Supported-machine gate: Centurion only runs on Lenovo Legion,
//! LOQ and IdeaPad Gaming laptops. Every helper refuses to touch hardware
//! elsewhere (the GUI has the same check, gui/src/sysinfo.cpp).
//!
//! DMI on these models: sys_vendor "LENOVO", product_name = machine type
//! ("83RU"), product_version / product_family = the marketing name
//! ("Legion Pro 7 16AFR10H", "LOQ 15IRH8", "IdeaPad Gaming 3 15ACH6").

use std::path::Path;

/// Marketing-name prefixes accepted (case-insensitive, word-anchored).
pub const FAMILIES: &[&str] = &["Legion", "LOQ", "IdeaPad Gaming"];

/// China-market Legion / GeekPro names that may not carry "Legion" in DMI
/// (from LenovoLegionToolkit's allowed-model list): "Y9000P IAX10", "R9000K"…
pub const CN_MODELS: &[&str] = &["Y9000", "R9000", "Y7000", "R7000", "G5000"];

/// A word of the name starts with one of CN_MODELS ("Y9000P" yes, "XY9000" no).
fn names_cn_model(name: &str) -> bool {
    name.to_ascii_uppercase().split(|c: char| !c.is_ascii_alphanumeric())
        .any(|w| CN_MODELS.iter().any(|m| w.starts_with(m)))
}

fn read(base: &Path, f: &str) -> String {
    std::fs::read_to_string(base.join(f)).map(|s| s.trim().to_owned()).unwrap_or_default()
}

/// "Legion Pro 7" matches "Legion", "Lenovo LOQ 15IAX9" matches "LOQ",
/// but "Legionnaire" / "BLOQ" do not.
fn names_family(name: &str) -> bool {
    let n = name.to_ascii_lowercase();
    FAMILIES.iter().any(|fam| {
        let f = fam.to_ascii_lowercase();
        n.match_indices(&f).any(|(i, _)| {
            let before = n[..i].chars().next_back().map_or(true, |c| !c.is_ascii_alphanumeric());
            let after = n[i + f.len()..].chars().next().map_or(true, |c| !c.is_ascii_alphanumeric());
            before && after
        })
    })
}

/// Ok(model name) on a supported machine, Err(reason) otherwise.
pub fn check_at(dmi: &Path) -> Result<String, String> {
    let vendor = read(dmi, "sys_vendor");
    let names = [read(dmi, "product_version"), read(dmi, "product_family")];
    let shown = names.iter().find(|s| !s.is_empty()).cloned()
        .unwrap_or_else(|| read(dmi, "product_name"));
    if !vendor.eq_ignore_ascii_case("LENOVO") {
        let what = [vendor.as_str(), shown.as_str()].iter().filter(|s| !s.is_empty()).copied()
            .collect::<Vec<_>>().join(" ");
        let what = if what.is_empty() { "no DMI information".to_owned() } else { what };
        return Err(format!("unsupported machine ({what}): Centurion runs only on \
                            Lenovo Legion, LOQ and IdeaPad Gaming laptops"));
    }
    if names.iter().any(|n| names_family(n) || names_cn_model(n)) {
        Ok(shown)
    } else {
        Err(format!("unsupported Lenovo model ({shown}): Centurion runs only on \
                     Legion, LOQ and IdeaPad Gaming laptops"))
    }
}

pub fn check() -> Result<String, String> { check_at(Path::new("/sys/class/dmi/id")) }

/// BIOS identity: "SMCN19WW" → prefix "SMCN" (the board family), version 19.
/// Lenovo reuses a prefix across every BIOS release of one board, so firmware
/// quirks are keyed on it (the scheme LenovoLegionToolkit uses).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Bios { pub prefix: String, pub version: Option<u32> }

pub fn parse_bios(raw: &str) -> Bios {
    let raw = raw.trim();
    let head: String = raw.chars().take(4).collect();
    if head.len() < 4 || !head.chars().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit()) {
        return Bios::default();
    }
    let rest = &raw[4..];
    let version = rest.as_bytes().windows(2).position(|w| w[0].is_ascii_digit() && w[1].is_ascii_digit())
        .and_then(|i| rest[i..i + 2].parse().ok());
    Bios { prefix: head, version }
}

pub fn bios_at(dmi: &Path) -> Bios { parse_bios(&read(dmi, "bios_version")) }
pub fn bios() -> Bios { bios_at(Path::new("/sys/class/dmi/id")) }

/// Known firmware bugs when switching platform profiles, as worked around by
/// LenovoLegionToolkit (PowerModeFeature):
///  - J2CN boards: Quiet → Performance directly misbehaves; go through Balanced.
///  - K1CN boards: leaving Custom directly misbehaves; step through another
///    mode first (Quiet via Performance, Balanced via Quiet, Performance via
///    Balanced, Extreme is simply written twice).
/// Returns the mode to write first (kernel names), or None.
pub fn profile_detour(bios: &Bios, from: &str, to: &str) -> Option<&'static str> {
    if bios.prefix.eq_ignore_ascii_case("J2CN") && from == "low-power" && to == "performance" {
        return Some("balanced");
    }
    if bios.prefix.eq_ignore_ascii_case("K1CN") && from == "custom" && to != "custom" {
        return match to {
            "low-power" => Some("performance"),
            "balanced" => Some("low-power"),
            "performance" => Some("balanced"),
            "max-power" => Some("max-power"),
            _ => None,
        };
    }
    None
}

// ── Model database (series / generation / firmware quirks) ──────────────────
//
// Which Legion a machine is decides which optional interfaces Centurion may offer.
// The tables below follow LenovoLegionToolkit's Compatibility.cs (machine-type
// map, model keywords, generation rule, BIOS quirk lists); the code is Centurion's own.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Series { Legion5, LegionPro5, LegionSlim5, Legion7, LegionPro7, Legion9, LegionGo, LenovoSlim, LegionLegacy, IdeaPadGaming, Loq, Unknown }

impl Series {
    pub fn name(self) -> &'static str {
        match self {
            Series::Legion5 => "Legion 5", Series::LegionPro5 => "Legion Pro 5", Series::LegionSlim5 => "Legion Slim 5",
            Series::Legion7 => "Legion 7", Series::LegionPro7 => "Legion Pro 7", Series::Legion9 => "Legion 9",
            Series::LegionGo => "Legion Go", Series::LenovoSlim => "Lenovo Slim", Series::LegionLegacy => "Legion",
            Series::IdeaPadGaming => "IdeaPad Gaming", Series::Loq => "LOQ", Series::Unknown => "unknown",
        }
    }
}

/// DMI product_name prefix (Lenovo machine type) → series.
const MACHINE_TYPES: &[(&str, Series)] = &[
    ("83F0", Series::Legion5), ("83F1", Series::Legion5), ("83M0", Series::Legion5), ("83NX", Series::Legion5),
    ("83N2", Series::Legion5), ("83LY", Series::Legion5), ("83DG", Series::Legion5), ("83EW", Series::Legion5),
    ("83EG", Series::Legion5), ("83JJ", Series::Legion5), ("82RC", Series::Legion5), ("82RB", Series::Legion5),
    ("82TB", Series::Legion5), ("83EF", Series::Legion5), ("82RE", Series::Legion5), ("82RD", Series::Legion5),
    ("83Q7", Series::Legion5), ("83RW", Series::Legion5),
    ("83DH", Series::LegionSlim5), ("83EX", Series::LegionSlim5), ("82Y5", Series::LegionSlim5),
    ("82Y9", Series::LegionSlim5), ("82YA", Series::LegionSlim5), ("83D6", Series::LegionSlim5),
    ("83LT", Series::LegionPro5), ("83F3", Series::LegionPro5), ("83DF", Series::LegionPro5), ("83F2", Series::LegionPro5),
    ("83LU", Series::LegionPro5), ("82WM", Series::LegionPro5), ("83NN", Series::LegionPro5), ("82WK", Series::LegionPro5),
    ("82JQ", Series::LegionPro5),
    ("83KY", Series::Legion7), ("83FD", Series::Legion7), ("82UH", Series::Legion7), ("82TD", Series::Legion7),
    ("82N6", Series::Legion7),
    ("83RU", Series::LegionPro7), ("83F5", Series::LegionPro7), ("83DE", Series::LegionPro7), ("82WR", Series::LegionPro7),
    ("82WQ", Series::LegionPro7), ("82WS", Series::LegionPro7),
    ("83G0", Series::Legion9), ("83EY", Series::Legion9),
    ("83E1", Series::LegionGo),
];

/// Machine type first; else the marketing name. "IdeaPad Gaming" is tested
/// before any plain "IdeaPad" rule could apply, and a bare "Legion" is the
/// catch-all for models missing from the table.
pub fn series_of(model: &str, machine_type: &str) -> Series {
    let mt: String = machine_type.trim().chars().take(4).collect::<String>().to_ascii_uppercase();
    if let Some(&(_, s)) = MACHINE_TYPES.iter().find(|(k, _)| *k == mt) { return s; }
    let m = model.to_ascii_lowercase();
    if m.contains("loq") { return Series::Loq; }
    if m.contains("ideapad gaming") { return Series::IdeaPadGaming; }
    if m.contains("lenovo slim") { return Series::LenovoSlim; }
    if m.contains("legion") || names_cn_model(model) { return Series::LegionLegacy; }
    Series::Unknown
}

/// Product generation from the model code: "16AFR10H" → 10, "15ARH05H" → 5,
/// "15IRH8" → 8, "… G4" → 4. 0 = cannot tell (e.g. "Y540-15IRH").
pub fn generation_of(model: &str) -> u32 {
    let c: Vec<char> = model.chars().collect();
    let take = |i: usize| -> u32 {
        let d: String = c[i..].iter().take(2).take_while(|x| x.is_ascii_digit()).collect();
        d.parse().unwrap_or(0)
    };
    // 1-2 digits right after three letters: the platform code (AFR10, IRH8).
    for i in 3..c.len() {
        if c[i].is_ascii_digit() && c[i - 3..i].iter().all(|x| x.is_ascii_alphabetic()) { return take(i); }
    }
    // "G<n>" / "Gen<n>"-style suffix.
    for i in 1..c.len() {
        if c[i].is_ascii_digit() && c[i - 1].eq_ignore_ascii_case(&'g') {
            let d: String = c[i..].iter().take_while(|x| x.is_ascii_digit()).collect();
            if let Ok(v) = d.parse() { return v; }
        }
    }
    // A lone 1-2 digit number that is not a screen size.
    let mut i = 0;
    while i < c.len() {
        if c[i].is_ascii_digit() {
            let j = (i..c.len()).find(|&k| !c[k].is_ascii_digit()).unwrap_or(c.len());
            if j - i <= 2 {
                let v: u32 = c[i..j].iter().collect::<String>().parse().unwrap_or(0);
                if !(14..=18).contains(&v) { return v; }
            }
            i = j;
        } else { i += 1; }
    }
    0
}

#[derive(Debug, Clone, PartialEq)]
pub struct Info { pub model: String, pub machine_type: String, pub series: Series, pub generation: u32, pub bios: Bios }

pub fn info_at(dmi: &Path) -> Info {
    let names = [read(dmi, "product_version"), read(dmi, "product_family")];
    let model = names.iter().find(|s| !s.is_empty()).cloned().unwrap_or_default();
    let machine_type: String = read(dmi, "product_name").chars().take(4).collect();
    Info { series: series_of(&model, &machine_type), generation: generation_of(&model), bios: bios_at(dmi), model, machine_type }
}

pub fn info() -> &'static Info {
    static I: std::sync::OnceLock<Info> = std::sync::OnceLock::new();
    I.get_or_init(|| info_at(Path::new("/sys/class/dmi/id")))
}

/// First-generation Custom mode (2021/22 boards): broken before these BIOS
/// releases. Some(min) = this BIOS is older than the first working one.
pub fn custom_mode_needs_bios(bios: &Bios) -> Option<u32> {
    const MIN: &[(&str, u32)] = &[("G9CN", 24), ("GKCN", 46), ("H1CN", 39), ("HACN", 31), ("HHCN", 20)];
    let &(_, min) = MIN.iter().find(|(p, _)| bios.prefix.eq_ignore_ascii_case(p))?;
    (bios.version? < min).then_some(min)
}

/// The EC's Fn-lock bit means the opposite on the IdeaPad-style keyboards
/// (hotkeys are the primary function there): 1 = F1–F12 primary.
pub fn fn_lock_inverted(series: Series) -> bool { matches!(series, Series::IdeaPadGaming | Series::Unknown) }

#[cfg(test)]
mod tests {
    use super::*;

    fn dmi(vendor: &str, version: &str, family: &str) -> tempdir::Dir {
        let d = tempdir::Dir::new();
        for (f, v) in [("sys_vendor", vendor), ("product_version", version), ("product_family", family), ("product_name", "83RU")] {
            std::fs::write(d.0.join(f), format!("{v}\n")).unwrap();
        }
        d
    }

    mod tempdir {
        pub struct Dir(pub std::path::PathBuf);
        impl Dir {
            pub fn new() -> Self {
                use std::sync::atomic::{AtomicU32, Ordering};
                static N: AtomicU32 = AtomicU32::new(0);
                let p = std::env::temp_dir().join(format!("centurion-dmi-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
                std::fs::create_dir_all(&p).unwrap();
                Dir(p)
            }
        }
        impl Drop for Dir { fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.0); } }
    }

    #[test]
    fn accepted() {
        for (ver, fam) in [("Legion Pro 7 16AFR10H", "Legion Pro 7 16AFR10H"), ("LOQ 15IRH8", ""),
                           ("", "IdeaPad Gaming 3 15ACH6"), ("Lenovo Legion Y540-15IRH", "Legion Y540"),
                           ("Legion Go 8APU1", "Legion Go"), ("Lenovo LOQ 15IAX9", "")] {
            let d = dmi("LENOVO", ver, fam);
            assert!(check_at(&d.0).is_ok(), "{ver} / {fam}");
        }
    }

    #[test]
    fn china_models() {
        for ver in ["Lenovo Y9000P IAX10", "R9000K", "Y7000P 2024", "GeekPro G5000 IAX10"] {
            assert!(check_at(&dmi("LENOVO", ver, "").0).is_ok(), "{ver}");
        }
        for ver in ["XY9000", "Y900", "Yoga 9000"] {
            assert!(check_at(&dmi("LENOVO", ver, "").0).is_err(), "{ver}");
        }
        assert!(check_at(&dmi("HP", "Y9000P", "").0).is_err());
    }

    #[test]
    fn bios_and_detours() {
        assert_eq!(parse_bios("SMCN19WW"), Bios { prefix: "SMCN".into(), version: Some(19) });
        assert_eq!(parse_bios("J2CN25WW\n"), Bios { prefix: "J2CN".into(), version: Some(25) });
        assert_eq!(parse_bios("1.19"), Bios::default());
        let (j2, k1, sm) = (parse_bios("J2CN25WW"), parse_bios("K1CN31WW"), parse_bios("SMCN19WW"));
        assert_eq!(profile_detour(&j2, "low-power", "performance"), Some("balanced"));
        assert_eq!(profile_detour(&j2, "balanced", "performance"), None);
        assert_eq!(profile_detour(&k1, "custom", "balanced"), Some("low-power"));
        assert_eq!(profile_detour(&k1, "custom", "custom"), None);
        assert_eq!(profile_detour(&sm, "custom", "balanced"), None);
        assert_eq!(profile_detour(&sm, "low-power", "performance"), None);
    }

    #[test]
    fn refused() {
        for (vendor, ver, fam) in [("LENOVO", "ThinkPad X1 Carbon Gen 11", "ThinkPad X1 Carbon Gen 11"),
                                   ("LENOVO", "IdeaPad 5 14ALC05", "IdeaPad 5 14ALC05"),
                                   ("LENOVO", "Yoga Slim 7", ""), ("Dell Inc.", "Legion", "Legion"),
                                   ("ASUSTeK COMPUTER INC.", "ROG Strix", ""), ("", "", ""),
                                   ("LENOVO", "Legionnaire 1", ""), ("LENOVO", "BLOQ 3", "")] {
            let d = dmi(vendor, ver, fam);
            assert!(check_at(&d.0).is_err(), "{vendor} {ver} / {fam}");
        }
    }

    #[test]
    fn series_and_generation() {
        assert_eq!(series_of("Legion Pro 7 16AFR10H", "83RU"), Series::LegionPro7);
        assert_eq!(series_of("Legion 5 Pro 16ACH6H", "82JQ"), Series::LegionPro5);
        assert_eq!(series_of("Legion 5 15ARH05H", "82B1"), Series::LegionLegacy);
        assert_eq!(series_of("LOQ 15IRH8", "82XV"), Series::Loq);
        assert_eq!(series_of("IdeaPad Gaming 3 15ACH6", "82K2"), Series::IdeaPadGaming);
        assert_eq!(series_of("Lenovo Y9000P IAX10", ""), Series::LegionLegacy);
        assert_eq!(series_of("ThinkPad X1", "21HM"), Series::Unknown);
        for (m, g) in [("Legion Pro 7 16AFR10H", 10), ("Legion 5 15ARH05H", 5), ("LOQ 15IRH8", 8), ("Legion Go 8APU1", 1),
                       ("Legion Y540-15IRH", 0), ("Legion 5 Pro 16ACH6H", 6), ("Y9000P IRX9", 9), ("ThinkBook 16p G4+", 4),
                       ("Legion Slim 7 Gen 7", 7), ("", 0)] {
            assert_eq!(generation_of(m), g, "{m}");
        }
    }

    #[test]
    fn quirk_tables() {
        assert_eq!(custom_mode_needs_bios(&parse_bios("GKCN40WW")), Some(46));
        assert_eq!(custom_mode_needs_bios(&parse_bios("GKCN46WW")), None);
        assert_eq!(custom_mode_needs_bios(&parse_bios("SMCN19WW")), None);
        assert!(fn_lock_inverted(Series::IdeaPadGaming));
        assert!(!fn_lock_inverted(Series::LegionPro7) && !fn_lock_inverted(Series::Loq));
    }
}
