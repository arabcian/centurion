//! Read-only profile/flatten preflight. It is a consistency guard, not an
//! electrical stability test. No GPU mutations occur in this module.
use crate::hal::vfcurve::PointDeltas;
use crate::types::{CurveState, Domain, VfPoint};

const MAX_BASE_DRIFT_KHZ: i64 = 45_000;
const MAX_ANCHOR_OVERSHOOT_KHZ: i64 = 30_000;
const MAX_LOCAL_DOWN_KHZ: i64 = 45_000;

fn projected(p: &VfPoint, deltas: &PointDeltas) -> i64 {
    p.base_freq_khz() + deltas.get(&(p.index as i64)).copied().unwrap_or(i64::from(p.delta_khz))
}

/// Check that a programmed curve doesn't fold back or contain an absurd point.
/// Only adjacent points with similar voltage are compared. The stock curve's
/// own shape is the driver's data (Blackwell has 150-200 MHz jumps at one
/// voltage), so only a drop the *offsets* introduce is rejected; a one-point
/// spike shows up as such a drop on its far side.
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
                let stock_step = base - q.base_freq_khz();
                if step < -MAX_LOCAL_DOWN_KHZ && step < stock_step - MAX_LOCAL_DOWN_KHZ {
                    return Err(format!("points {}→{}: offsets fold the curve back by {} kHz", q.index, p.index, -step));
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
    // Real Blackwell stock curve: +187 MHz between points 29→30 at the same
    // voltage. The driver's own shape must never block a write.
    #[test]
    fn stock_curve_jump_is_not_an_anomaly() {
        let s = st(vec![pt(28, 1_400_000, 700_000, 0), pt(29, 1_413_000, 705_000, 0),
                        pt(30, 1_600_000, 705_000, 0), pt(31, 1_615_000, 710_000, 0),
                        pt(32, 1_630_000, 715_000, 0)]);
        assert!(validate_curve_shape(&s, &PointDeltas::new()).is_ok());
        let mut d = PointDeltas::new();
        d.extend([(28, 100_000), (29, 100_000), (30, 100_000), (31, 100_000), (32, 100_000)]);
        assert!(validate_curve_shape(&s, &d).is_ok());
        // Flatten starting before the jump: plateau at 1600 MHz from point 29.
        d.clear();
        d.extend([(28, 0), (29, 187_000), (30, 0), (31, -15_000), (32, -30_000)]);
        assert!(verify_flatten_preflight(&s, &s, 29, 1600, &d).is_ok());
    }
    #[test]
    fn rejects_fold_or_spike_introduced_by_the_write() {
        let s = st(vec![pt(29, 1_413_000, 705_000, 0), pt(30, 1_600_000, 705_000, 0),
                        pt(31, 1_615_000, 710_000, 0)]);
        let mut d = PointDeltas::new();
        d.extend([(29, 0), (30, -250_000), (31, 0)]); // fold: 30 drops below 29
        assert!(validate_curve_shape(&s, &d).is_err());
        d.extend([(29, 0), (30, 200_000), (31, 0)]);  // one-point spike
        assert!(validate_curve_shape(&s, &d).is_err());
    }
    #[test]
    fn rejects_unstable_base() {
        let s = st(vec![pt(72, 2_670_000, 810_000, 0)]);
        let c = st(vec![pt(72, 2_750_000, 810_000, 0)]);
        let mut d = PointDeltas::new(); d.insert(72, 30_000);
        assert!(verify_flatten_preflight(&s, &c, 72, 2700, &d).is_err());
    }
}
