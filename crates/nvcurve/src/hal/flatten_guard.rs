//! Read-only profile/flatten preflight. It is a consistency guard, not an
//! electrical stability test. No GPU mutations occur in this module.
use crate::hal::vfcurve::PointDeltas;
use crate::types::{CurveState, Domain, VfPoint};

const MAX_BASE_DRIFT_KHZ: i64 = 45_000;
const MAX_ANCHOR_OVERSHOOT_KHZ: i64 = 30_000;
const MAX_LOCAL_UP_KHZ: i64 = 180_000;
const MAX_LOCAL_DOWN_KHZ: i64 = 45_000;
const MAX_INTERPOLATION_DEVIATION_KHZ: i64 = 90_000;

fn projected(p: &VfPoint, deltas: &PointDeltas) -> i64 {
    p.base_freq_khz() + deltas.get(&(p.index as i64)).copied().unwrap_or(i64::from(p.delta_khz))
}

/// Check that a programmed curve doesn't fold back or contain an absurd point.
/// Only adjacent points with similar voltage are compared: frequency jumps
/// over a large voltage span can be legitimate.
pub fn validate_curve_shape(state: &CurveState, deltas: &PointDeltas) -> Result<(), String> {
    let mut previous: Option<&VfPoint> = None;
    for p in state.points.iter().filter(|p| p.domain == Domain::Gpu && p.freq_khz != 0) {
        let base = p.base_freq_khz();
        let target = projected(p, deltas);
        if !(1_000..=6_000_000).contains(&base) || !(1_000..=6_000_000).contains(&target) {
            return Err(format!("point {}: impossible base ({base}) or projected ({target}) kHz", p.index));
        }
        if !(300_000..=1_500_000).contains(&p.volt_uv) {
            return Err(format!("point {}: invalid voltage {} uV", p.index, p.volt_uv));
        }
        if let Some(q) = previous {
            if p.volt_uv < q.volt_uv {
                return Err(format!("voltage reversal at points {} and {}", q.index, p.index));
            }
            if p.volt_uv - q.volt_uv <= 25_000 {
                let step = target - projected(q, deltas);
                if !(-MAX_LOCAL_DOWN_KHZ..=MAX_LOCAL_UP_KHZ).contains(&step) {
                    return Err(format!("points {}→{}: anomalous frequency step {step} kHz", q.index, p.index));
                }
            }
        }
        previous = Some(p);
    }
    Ok(())
}

/// The profile's flatten calculation must yield precisely the target on every
/// selected point. A second independent driver read detects driver changes or
/// another writer before sending a SET_CLOCK_BOOST_TABLE request.
pub fn verify_flatten_preflight(
    initial: &CurveState, confirmation: &CurveState, from: i64, mhz: i64, deltas: &PointDeltas,
) -> Result<(), String> {
    let target_khz = mhz.checked_mul(1000).ok_or("target frequency overflow")?;
    if initial.points.len() != confirmation.points.len() {
        return Err("GPU point count changed between reads".into());
    }
    for (p, q) in initial.points.iter().zip(&confirmation.points) {
        if p.index != q.index || p.domain != q.domain || p.volt_uv != q.volt_uv {
            return Err(format!("point {}: domain/index/voltage changed between driver reads", p.index));
        }
        if p.delta_khz != q.delta_khz {
            return Err(format!("point {}: prior CT offset changed during preflight", p.index));
        }
        if p.domain == Domain::Gpu && p.index as i64 >= from && p.freq_khz != 0 {
            let base = p.base_freq_khz();
            let fresh = q.base_freq_khz();
            if (base - fresh).abs() > MAX_BASE_DRIFT_KHZ {
                return Err(format!("point {}: base drifted {} kHz across reads", p.index, (base - fresh).abs()));
            }
            let delta = deltas.get(&(p.index as i64))
                .ok_or_else(|| format!("point {}: flatten delta missing", p.index))?;
            let projected_khz = base.checked_add(*delta).ok_or("frequency overflow")?;
            if projected_khz != target_khz {
                return Err(format!("point {}: flatten projected {projected_khz}, expected {target_khz} kHz", p.index));
            }
        }
    }
    validate_curve_shape(initial, deltas)?;

    // Check the point immediately before the plateau; a steep rising anchor
    // above the flat region would create a fold, regardless of CT validity.
    if let Some(anchor) = initial.points.iter().filter(|p| p.domain == Domain::Gpu && (p.index as i64) < from && p.freq_khz > 0).last() {
        if projected(anchor, deltas) > target_khz + MAX_ANCHOR_OVERSHOOT_KHZ {
            return Err(format!("point {}: anchor exceeds plateau by more than 30 MHz", anchor.index));
        }
    }
    // Reject a local base outlier at otherwise evenly spaced voltages. Do not
    // interpolate across a 0 mV or very large-voltage interval.
    let gpu: Vec<_> = initial.points.iter().filter(|p| p.domain == Domain::Gpu && p.freq_khz > 0).collect();
    for w in gpu.windows(3) {
        let (a, b, c) = (w[0], w[1], w[2]);
        if (b.index as i64) < from || c.volt_uv <= a.volt_uv || c.volt_uv - a.volt_uv > 50_000 { continue; }
        let interp = a.base_freq_khz() + (c.base_freq_khz() - a.base_freq_khz())
            * i64::from(b.volt_uv - a.volt_uv) / i64::from(c.volt_uv - a.volt_uv);
        if (b.base_freq_khz() - interp).abs() > MAX_INTERPOLATION_DEVIATION_KHZ {
            return Err(format!("point {}: anomalous base frequency vs neighbours", b.index));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn pt(i: usize, freq: u32, v: u32, delta: i32) -> VfPoint {
        VfPoint { index: i, freq_khz: freq, base_khz: None, volt_uv: v,
                  delta_khz: delta, domain: Domain::Gpu }
    }
    fn st(points: Vec<VfPoint>) -> CurveState {
        CurveState { points, timestamp: 0.0, gpu_name: String::new(), domain_source: "flags" }
    }
    #[test]
    fn healthy_flatten_and_small_drift() {
        let before = st(vec![pt(71, 2_650_000, 800_000, 0), pt(72, 2_670_000, 810_000, 0),
                             pt(73, 2_690_000, 820_000, 0)]);
        let after = st(vec![pt(71, 2_650_000, 800_000, 0), pt(72, 2_685_000, 810_000, 0),
                            pt(73, 2_690_000, 820_000, 0)]);
        let mut d = PointDeltas::new();
        d.extend([(71, 0), (72, 30_000), (73, 10_000)]);
        assert!(verify_flatten_preflight(&before, &after, 72, 2700, &d).is_ok());
    }
    #[test]
    fn bad_projection_and_anchor() {
        let s = st(vec![pt(71, 2_740_000, 800_000, 0), pt(72, 2_670_000, 810_000, 0)]);
        let mut d = PointDeltas::new(); d.extend([(71, 0), (72, 30_000)]);
        assert!(verify_flatten_preflight(&s, &s, 72, 2700, &d).is_err());
        let s = st(vec![pt(71, 2_650_000, 800_000, 0), pt(72, 2_670_000, 810_000, 0)]);
        d.insert(72, 25_000);
        assert!(verify_flatten_preflight(&s, &s, 72, 2700, &d).is_err());
    }
    #[test]
    fn rejects_stale_offsets_and_voltages() {
        let s = st(vec![pt(72, 2_670_000, 810_000, 0)]);
        let c = st(vec![pt(72, 2_670_000, 810_000, 15_000)]);
        let mut d = PointDeltas::new(); d.insert(72, 30_000);
        assert!(verify_flatten_preflight(&s, &c, 72, 2700, &d).is_err());
        let c = st(vec![pt(72, 2_670_000, 820_000, 0)]);
        assert!(verify_flatten_preflight(&s, &c, 72, 2700, &d).is_err());
    }
    #[test]
    fn rejects_unstable_base() {
        let s = st(vec![pt(72, 2_670_000, 810_000, 0)]);
        let c = st(vec![pt(72, 2_750_000, 810_000, 0)]);
        let mut d = PointDeltas::new(); d.insert(72, 30_000);
        assert!(verify_flatten_preflight(&s, &c, 72, 2700, &d).is_err());
    }
}
