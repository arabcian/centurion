//! The work: a chunk of arithmetic whose result is a 64-bit hash that depends on every step.
//!
//! Each chunk fills a 256 KiB buffer from its seed, then iterates
//!   - the logistic map x ← r·(x − x²) on 8 (sse), 16 (avx2) or 32 (avx512) doubles. It is chaotic, so one
//!     wrong bit anywhere grows until the final value is completely different (tests/flip_*), and the same
//!     operations in the same order give bit-identical results on every core (no fast-math, no contraction);
//!   - an integer multiply / shift / xor chain fed with every double's bits before each step;
//!   - a dependent walk through the buffer (load, xor, store), so the load/store units and L2 take part.
//!
//! The three loads differ in how hard they hit the core: `sse` is scalar SSE2 and leaves the clock at its
//! highest boost (where Curve Optimizer and undervolt offsets usually fail first); `avx2` uses 256-bit FMA;
//! `avx512` 512-bit FMA, the most current and heat and so the lowest clock. A chunk only has to equal the
//! same chunk of the same load computed on another core.
//!
//! `power` is different: no multiply chain, no memory walk, only the logistic map on 10 (AVX2) or 16
//! (AVX-512) independent vectors at once. That is enough independent work to keep both FMA pipes busy every
//! cycle, which is what draws the most current (the loads above wait on their dependency chains and the
//! buffer). It is the all-core load that runs into the package power limit. Before every step the raw bits
//! of each vector are added into a few integer accumulators, which are then stirred once per step by an
//! invertible xorshift-multiply (a few vector integer operations against 20-32 FMA / multiply ones). In
//! floating point the map is many-to-one near x = 0.5, so a one-ulp error can round away and two
//! trajectories merge again, and a plain sum would let a +1 / -1 pair of steps cancel; the stirred
//! accumulators keep any difference.

pub const BUF_WORDS: usize = 32 * 1024;  // 256 KiB: inside L2 on every core this targets
const R: f64 = 3.99;
const K1: u64 = 0xBF58_476D_1CE4_E5B9;
const K32: u64 = 0x9E37_79B9;  // 32-bit odd multiplier for the vector paths (mul_epu32)
/// Iterations per chunk, for every load: about 15 ms on a 3 GHz server core, so a chunk ends often enough to
/// check the stop flag and the deadline, and a transitions pause follows a short burst.
const ITERS: u32 = 3_000_000;
/// No fault injected (see `chunk_with`).
pub const NO_FLIP: u32 = u32::MAX;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Load { Sse, Avx2, Avx512, Power }

impl Load {
    pub fn parse(s: &str) -> Option<Load> {
        match s { "sse" => Some(Load::Sse), "avx2" => Some(Load::Avx2), "avx512" => Some(Load::Avx512), "power" => Some(Load::Power), _ => None }
    }
    pub fn name(self) -> &'static str {
        match self { Load::Sse => "sse", Load::Avx2 => "avx2", Load::Avx512 => "avx512", Load::Power => "power" }
    }
    pub fn supported(self) -> bool {
        #[cfg(target_arch = "x86_64")]
        {
            match self {
                Load::Sse => true,
                Load::Avx2 | Load::Power => is_x86_feature_detected!("avx2") && is_x86_feature_detected!("fma"),
                Load::Avx512 => is_x86_feature_detected!("avx512f"),
            }
        }
        #[cfg(not(target_arch = "x86_64"))]
        { self == Load::Sse }
    }
}

fn splitmix(s: &mut u64) -> u64 {
    *s = s.wrapping_add(0x9E37_79B9_7F4A_7C15);
    let mut z = *s;
    z = (z ^ (z >> 30)).wrapping_mul(K1);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^ (z >> 31)
}

/// A start value in [0.1, 0.9): away from the map's fixed points.
fn start(s: &mut u64) -> f64 { 0.1 + 0.8 * ((splitmix(s) >> 11) as f64 / (1u64 << 53) as f64) }

/// Fills the buffer; returns the seed state for the start values.
fn fill(seed: u64, buf: &mut [u64]) -> u64 {
    let mut s = seed ^ 0x6A09_E667_F3BC_C908;
    for w in buf.iter_mut() { *w = splitmix(&mut s); }
    s
}

fn fold(mut h: u64, words: impl Iterator<Item = u64>) -> u64 {
    for w in words { h = (h ^ w).wrapping_mul(K1).rotate_left(23); }
    h
}

/// One chunk. The caller has checked `load.supported()`.
pub fn chunk(load: Load, seed: u64, buf: &mut [u64]) -> u64 { chunk_with(load, seed, buf, NO_FLIP) }

/// `flip`: iteration at which one low bit of the first double is flipped (a simulated fault, tests only).
pub fn chunk_with(load: Load, seed: u64, buf: &mut [u64], flip: u32) -> u64 {
    assert_eq!(buf.len(), BUF_WORDS);
    match load {
        Load::Sse => sse(seed, buf, flip),
        #[cfg(target_arch = "x86_64")]
        // SAFETY: the CPU features were checked by Load::supported before any chunk runs.
        Load::Avx2 => unsafe { x86::avx2(seed, buf, flip) },
        #[cfg(target_arch = "x86_64")]
        Load::Avx512 => unsafe { x86::avx512(seed, buf, flip) },
        #[cfg(target_arch = "x86_64")]
        Load::Power => unsafe {
            if is_x86_feature_detected!("avx512f") { x86::power512(seed, buf, flip) } else { x86::power256(seed, buf, flip) }
        },
        #[cfg(not(target_arch = "x86_64"))]
        _ => unreachable!("vector loads are x86_64 only"),
    }
}

fn sse(seed: u64, buf: &mut [u64], flip: u32) -> u64 {
    let mut s = fill(seed, buf);
    let mut x = [0.0f64; 8];
    for v in x.iter_mut() { *v = start(&mut s); }
    let mut h = [splitmix(&mut s), splitmix(&mut s), splitmix(&mut s), splitmix(&mut s)];
    let mask = BUF_WORDS - 1;
    let mut idx = (s as usize) & mask;
    for it in 0..ITERS {
        if it == flip { x[0] = f64::from_bits(x[0].to_bits() ^ 1); }
        // The integer chain takes every double's bits before the map updates it: a wrong bit lands in the
        // chain even when the map's rounding would absorb it, and the chain (a bijection) never loses it.
        for (j, hj) in h.iter_mut().enumerate() {
            *hj = ((*hj ^ (*hj >> 29)).wrapping_mul(K1)).wrapping_add(x[j].to_bits() ^ x[j + 4].to_bits());
        }
        for v in x.iter_mut() { *v = R * (*v - *v * *v); }
        let w = buf[idx];
        buf[idx] = w ^ h[0];
        idx = ((w ^ h[1]) as usize) & mask;
    }
    let h0 = fold(0, x.iter().map(|v| v.to_bits()).chain(h));
    fold(h0, buf.iter().copied())
}

#[cfg(target_arch = "x86_64")]
mod x86 {
    use super::*;
    use std::arch::x86_64::*;

    #[target_feature(enable = "avx2,fma")]
    pub unsafe fn avx2(seed: u64, buf: &mut [u64], flip: u32) -> u64 {
        let mut s = fill(seed, buf);
        let r = _mm256_set1_pd(R);
        let k = _mm256_set1_epi64x(K32 as i64);
        let mut x = [_mm256_setzero_pd(); 4];
        for v in x.iter_mut() { *v = _mm256_set_pd(start(&mut s), start(&mut s), start(&mut s), start(&mut s)); }
        let mut h = [_mm256_set_epi64x(splitmix(&mut s) as i64, splitmix(&mut s) as i64, splitmix(&mut s) as i64, splitmix(&mut s) as i64); 2];
        h[1] = _mm256_xor_si256(h[1], _mm256_set1_epi64x(splitmix(&mut s) as i64));
        let mask = BUF_WORDS - 1;
        let mut idx = (s as usize) & mask;
        for it in 0..ITERS {
            if it == flip { x[0] = _mm256_castsi256_pd(_mm256_xor_si256(_mm256_castpd_si256(x[0]), _mm256_set_epi64x(0, 0, 0, 1))); }
            for (j, hj) in h.iter_mut().enumerate() {
                let m = _mm256_mul_epu32(_mm256_xor_si256(*hj, _mm256_srli_epi64::<29>(*hj)), k);
                let xb = _mm256_xor_si256(_mm256_castpd_si256(x[j]), _mm256_castpd_si256(x[j + 2]));
                *hj = _mm256_add_epi64(_mm256_add_epi64(m, _mm256_srli_epi64::<32>(*hj)), xb);
            }
            for v in x.iter_mut() { *v = _mm256_mul_pd(r, _mm256_fnmadd_pd(*v, *v, *v)); }
            let w = *buf.get_unchecked(idx);
            let h0 = _mm256_extract_epi64::<0>(h[0]) as u64;
            *buf.get_unchecked_mut(idx) = w ^ h0;
            idx = ((w ^ _mm256_extract_epi64::<1>(h[1]) as u64) as usize) & mask;
        }
        let mut words = [0u64; 24];
        for (i, v) in x.iter().enumerate() { _mm256_storeu_si256(words.as_mut_ptr().add(i * 4) as *mut __m256i, _mm256_castpd_si256(*v)); }
        for (i, v) in h.iter().enumerate() { _mm256_storeu_si256(words.as_mut_ptr().add(16 + i * 4) as *mut __m256i, *v); }
        fold(fold(0, words.iter().copied()), buf.iter().copied())
    }

    /// `power`: 10 independent 256-bit logistic maps. Each step is fnmadd + mul (about 7 cycles of latency);
    /// 10 vectors give 20 operations per step, more than two FMA pipes retire in that time, so they never idle.
    /// 10 maps + 2 accumulators + the constant fill the 16 AVX2 registers without spilling.
    #[target_feature(enable = "avx2,fma")]
    pub unsafe fn power256(seed: u64, buf: &mut [u64], flip: u32) -> u64 {
        let mut s = seed ^ 0x6A09_E667_F3BC_C908;
        let _ = buf;
        let r = _mm256_set1_pd(R);
        let mut x = [_mm256_setzero_pd(); 10];
        for v in x.iter_mut() { *v = _mm256_set_pd(start(&mut s), start(&mut s), start(&mut s), start(&mut s)); }
        let mut acc = [_mm256_setzero_si256(); 2];
        for it in 0..ITERS {
            if it == flip { x[0] = _mm256_castsi256_pd(_mm256_xor_si256(_mm256_castpd_si256(x[0]), _mm256_set_epi64x(0, 0, 0, 1))); }
            for (j, v) in x.iter_mut().enumerate() {
                acc[j & 1] = _mm256_add_epi64(acc[j & 1], _mm256_castpd_si256(*v));
                *v = _mm256_mul_pd(r, _mm256_fnmadd_pd(*v, *v, *v));
            }
            for a in acc.iter_mut() {
                let t = _mm256_xor_si256(*a, _mm256_srli_epi64::<29>(*a));
                *a = _mm256_add_epi64(t, _mm256_slli_epi64::<17>(t));  // t · (2^17 + 1): odd, invertible
            }
        }
        let mut words = [0u64; 48];
        for (i, v) in x.iter().enumerate() { _mm256_storeu_si256(words.as_mut_ptr().add(i * 4) as *mut __m256i, _mm256_castpd_si256(*v)); }
        for (i, v) in acc.iter().enumerate() { _mm256_storeu_si256(words.as_mut_ptr().add(40 + i * 4) as *mut __m256i, *v); }
        fold(0, words.iter().copied())
    }

    /// `power` with AVX-512: 16 independent 512-bit logistic maps and 4 accumulators (of the 32 registers).
    #[target_feature(enable = "avx512f")]
    pub unsafe fn power512(seed: u64, buf: &mut [u64], flip: u32) -> u64 {
        let mut s = seed ^ 0x6A09_E667_F3BC_C908;
        let _ = buf;
        let r = _mm512_set1_pd(R);
        let mut x = [_mm512_setzero_pd(); 16];
        for v in x.iter_mut() {
            let mut lanes = [0.0f64; 8];
            for l in lanes.iter_mut() { *l = start(&mut s); }
            *v = _mm512_loadu_pd(lanes.as_ptr());
        }
        let mut acc = [_mm512_setzero_si512(); 4];
        for it in 0..ITERS {
            if it == flip { x[0] = _mm512_castsi512_pd(_mm512_xor_si512(_mm512_castpd_si512(x[0]), _mm512_set_epi64(0, 0, 0, 0, 0, 0, 0, 1))); }
            for (j, v) in x.iter_mut().enumerate() {
                acc[j & 3] = _mm512_add_epi64(acc[j & 3], _mm512_castpd_si512(*v));
                *v = _mm512_mul_pd(r, _mm512_fnmadd_pd(*v, *v, *v));
            }
            for a in acc.iter_mut() {
                let t = _mm512_xor_si512(*a, _mm512_srli_epi64::<29>(*a));
                *a = _mm512_add_epi64(t, _mm512_slli_epi64::<17>(t));
            }
        }
        let mut words = [0u64; 160];
        for (i, v) in x.iter().enumerate() { _mm512_storeu_si512(words.as_mut_ptr().add(i * 8) as *mut __m512i, _mm512_castpd_si512(*v)); }
        for (i, v) in acc.iter().enumerate() { _mm512_storeu_si512(words.as_mut_ptr().add(128 + i * 8) as *mut __m512i, *v); }
        fold(0, words.iter().copied())
    }

    #[target_feature(enable = "avx512f")]
    pub unsafe fn avx512(seed: u64, buf: &mut [u64], flip: u32) -> u64 {
        let mut s = fill(seed, buf);
        let r = _mm512_set1_pd(R);
        let k = _mm512_set1_epi64(K32 as i64);
        let mut x = [_mm512_setzero_pd(); 4];
        for v in x.iter_mut() {
            let mut lanes = [0.0f64; 8];
            for l in lanes.iter_mut() { *l = start(&mut s); }
            *v = _mm512_loadu_pd(lanes.as_ptr());
        }
        let mut h = [_mm512_setzero_si512(); 2];
        for v in h.iter_mut() {
            let mut lanes = [0u64; 8];
            for l in lanes.iter_mut() { *l = splitmix(&mut s); }
            *v = _mm512_loadu_si512(lanes.as_ptr() as *const __m512i);
        }
        let mask = BUF_WORDS - 1;
        let mut idx = (s as usize) & mask;
        let lane0 = |v: __m512i| _mm_cvtsi128_si64(_mm512_castsi512_si128(v)) as u64;
        for it in 0..ITERS {
            if it == flip { x[0] = _mm512_castsi512_pd(_mm512_xor_si512(_mm512_castpd_si512(x[0]), _mm512_set_epi64(0, 0, 0, 0, 0, 0, 0, 1))); }
            for (j, hj) in h.iter_mut().enumerate() {
                let m = _mm512_mul_epu32(_mm512_xor_si512(*hj, _mm512_srli_epi64::<29>(*hj)), k);
                let xb = _mm512_xor_si512(_mm512_castpd_si512(x[j]), _mm512_castpd_si512(x[j + 2]));
                *hj = _mm512_add_epi64(_mm512_add_epi64(m, _mm512_srli_epi64::<32>(*hj)), xb);
            }
            for v in x.iter_mut() { *v = _mm512_mul_pd(r, _mm512_fnmadd_pd(*v, *v, *v)); }
            let w = *buf.get_unchecked(idx);
            *buf.get_unchecked_mut(idx) = w ^ lane0(h[0]);
            idx = ((w ^ lane0(h[1])) as usize) & mask;
        }
        let mut words = [0u64; 48];
        for (i, v) in x.iter().enumerate() { _mm512_storeu_si512(words.as_mut_ptr().add(i * 8) as *mut __m512i, _mm512_castpd_si512(*v)); }
        for (i, v) in h.iter().enumerate() { _mm512_storeu_si512(words.as_mut_ptr().add(32 + i * 8) as *mut __m512i, *v); }
        fold(fold(0, words.iter().copied()), buf.iter().copied())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn loads() -> Vec<Load> { [Load::Sse, Load::Avx2, Load::Avx512, Load::Power].into_iter().filter(|l| l.supported()).collect() }

    #[test]
    fn same_seed_same_result_other_seed_other_result() {
        let mut b = vec![0u64; BUF_WORDS];
        for l in loads() {
            let a = chunk(l, 7, &mut b);
            assert_eq!(a, chunk(l, 7, &mut b), "{l:?}");
            assert_ne!(a, chunk(l, 8, &mut b), "{l:?}");
        }
    }

    #[test]
    fn one_flipped_bit_changes_the_result() {
        let mut b = vec![0u64; BUF_WORDS];
        for l in loads() {
            let good = chunk(l, 3, &mut b);
            for flip in [0, 1000, 150_000] {
                assert_ne!(good, chunk_with(l, 3, &mut b, flip), "{l:?} flip at {flip}");
            }
        }
    }

    #[test]
    fn same_result_on_another_thread() {
        let mut b = vec![0u64; BUF_WORDS];
        for l in loads() {
            let here = chunk(l, 11, &mut b);
            let there = std::thread::spawn(move || chunk(l, 11, &mut vec![0u64; BUF_WORDS])).join().unwrap();
            assert_eq!(here, there, "{l:?}");
        }
    }

    /// `power`: a one-ulp error must show wherever it lands, although the map alone can round it away (two
    /// trajectories merge near x = 0.5). Both widths, every seed, early to the very last step.
    #[test]
    fn power_flip_anywhere_changes_the_result() {
        #[cfg(target_arch = "x86_64")]
        {
            if !Load::Power.supported() { return; }
            let mut b = vec![0u64; BUF_WORDS];
            let wide: [(&str, unsafe fn(u64, &mut [u64], u32) -> u64); 2] = [("256", x86::power256), ("512", x86::power512)];
            for (name, f) in wide {
                if name == "512" && !is_x86_feature_detected!("avx512f") { continue; }
                for seed in 0..8 {
                    let good = unsafe { f(seed, &mut b, NO_FLIP) };
                    for flip in [0, 1000, 1001, 1002, 77_777, ITERS / 2, ITERS - 100, ITERS - 2, ITERS - 1] {
                        assert_ne!(good, unsafe { f(seed, &mut b, flip) }, "{name}-bit seed {seed} flip at {flip}");
                    }
                    // Many more positions: the map alone misses a sizeable share of these.
                    let mut st = seed + 1;
                    for _ in 0..40 {
                        let flip = (splitmix(&mut st) % ITERS as u64) as u32;
                        assert_ne!(good, unsafe { f(seed, &mut b, flip) }, "{name}-bit seed {seed} flip at {flip}");
                    }
                }
            }
        }
    }

    #[test]
    fn names_round_trip() {
        for l in [Load::Sse, Load::Avx2, Load::Avx512, Load::Power] { assert_eq!(Load::parse(l.name()), Some(l)); }
        assert_eq!(Load::parse("avx"), None);
    }
}
