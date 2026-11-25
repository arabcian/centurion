//! Curve Optimizer workloads: the kinds of work that Prime95 Small FFT and y-cruncher use to expose an
//! unstable Curve Optimizer offset, each as a chunk whose 64-bit hash depends on every step.
//!
//!   - `fft-sse`: squaring a big number by double-precision FFT convolution (forward FFT, pointwise square,
//!     inverse FFT, round, carry), as Prime95's Small FFT torture test does. SSE2 only: the lightest load,
//!     so the core stays at its highest boost clock, where offsets fail first.
//!   - `fft-avx`: the same kernel built for the widest vectors this CPU has (AVX-512, else AVX2), with FMA:
//!     more current through the core, a lower clock, a different point on the curve.
//!   - `ntt`: the same squaring with a number-theoretic transform modulo the prime 2^64 - 2^32 + 1: 64x64→128
//!     bit integer multiplies, like y-cruncher's N63 / BKT tests load the integer multiplier.
//!
//! The FFT sizes (2K-64K points, 32 KiB-1 MiB) span L1 to L3, like Prime95's in-cache FFT lengths.
//! The hash takes the raw bits of the squared spectrum and of every value before rounding, so a wrong low
//! bit that rounding would hide is still caught. The same chunk gives the same hash on every core of a CPU (no reassociation,
//! no fast-math); results only have to agree with the same chunk computed on another core.

pub const NSEEDS: u64 = 8;
/// No fault injected (see `chunk_with`).
pub const NO_FLIP: u32 = u32::MAX;

/// log2 of the FFT length per seed: 2K..64K complex points.
const FFT_LOGS: [u32; NSEEDS as usize] = [11, 13, 15, 16, 12, 14, 16, 13];
/// log2 of the NTT length per seed: 1K..16K words.
const NTT_LOGS: [u32; NSEEDS as usize] = [10, 12, 14, 11, 13, 14, 12, 10];
/// Work per chunk, in butterflies (N·log2 N per transform pair): a chunk lasts a few ms, so a fluctuation
/// step can stop after one and the stop flag is seen quickly.
const FFT_BUDGET: u64 = 1 << 21;
const NTT_BUDGET: u64 = 1 << 21;
/// Balanced digits of 12 bits: the convolution of 64K digits stays far below 2^53.
const DIGIT_BITS: u32 = 12;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kernel { FftSse, FftAvx, Ntt }

/// The rotation order of the 40-second steady phase.
pub const KERNELS: [Kernel; 3] = [Kernel::FftSse, Kernel::FftAvx, Kernel::Ntt];

impl Kernel {
    pub fn name(self) -> &'static str {
        match self { Kernel::FftSse => "fft-sse", Kernel::FftAvx => "fft-avx", Kernel::Ntt => "ntt" }
    }
    pub fn supported(self) -> bool { self != Kernel::FftAvx || wide().is_some() }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Wide { Avx2, Avx512 }

fn wide() -> Option<Wide> {
    #[cfg(target_arch = "x86_64")]
    {
        if !is_x86_feature_detected!("fma") { return None; }
        if is_x86_feature_detected!("avx512f") { return Some(Wide::Avx512); }
        if is_x86_feature_detected!("avx2") { return Some(Wide::Avx2); }
    }
    None
}

/// What `fft-avx` runs on this CPU, for the start line.
pub fn wide_isa() -> &'static str {
    match wide() { Some(Wide::Avx512) => "avx512", Some(Wide::Avx2) => "avx2", None => "none" }
}

pub fn splitmix(s: &mut u64) -> u64 {
    *s = s.wrapping_add(0x9E37_79B9_7F4A_7C15);
    let mut z = *s;
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^ (z >> 31)
}

fn fold(h: u64, w: u64) -> u64 { (h ^ w).wrapping_mul(0xBF58_476D_1CE4_E5B9).rotate_left(23) }

/// Per-thread buffers and twiddle tables (built on first use of each size, then reused).
#[derive(Default)]
pub struct Scratch {
    re: Vec<f64>,
    im: Vec<f64>,
    digits: Vec<i64>,
    fft_tw: Vec<Option<(Vec<f64>, Vec<f64>)>>,
    ntt: Vec<u64>,
    ntt_tw: Vec<Option<(Vec<u64>, Vec<u64>)>>,
}

impl Scratch {
    pub fn new() -> Scratch { Scratch::default() }
}

/// One chunk. The caller has checked `kernel.supported()`.
pub fn chunk(k: Kernel, seed: u64, s: &mut Scratch) -> u64 { chunk_with(k, seed, s, NO_FLIP) }

/// `flip`: iteration at which one low bit is flipped right after the forward transform (tests only).
pub fn chunk_with(k: Kernel, seed: u64, s: &mut Scratch, flip: u32) -> u64 {
    let seed = seed % NSEEDS;
    match k {
        Kernel::FftSse => fft_chunk::<false>(seed, s, flip),
        #[cfg(target_arch = "x86_64")]
        // SAFETY: Kernel::supported checked these features before any chunk runs.
        Kernel::FftAvx => unsafe {
            match wide() {
                Some(Wide::Avx512) => fft_avx512(seed, s, flip),
                Some(Wide::Avx2) => fft_avx2(seed, s, flip),
                None => unreachable!("fft-avx needs AVX2/AVX-512 with FMA"),
            }
        },
        #[cfg(not(target_arch = "x86_64"))]
        Kernel::FftAvx => unreachable!("fft-avx is x86_64 only"),
        Kernel::Ntt => ntt_chunk(seed, s, flip),
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
unsafe fn fft_avx2(seed: u64, s: &mut Scratch, flip: u32) -> u64 { fft_chunk::<true>(seed, s, flip) }

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx512f,avx2,fma")]
unsafe fn fft_avx512(seed: u64, s: &mut Scratch, flip: u32) -> u64 { fft_chunk::<true>(seed, s, flip) }

// ── FFT ────────────────────────────────────────────────────────────────────────

/// Stage twiddles: the stage that combines halves of length h uses entries h..2h (e^{-iπj/h}).
fn fft_tables(log: u32) -> (Vec<f64>, Vec<f64>) {
    let n = 1usize << log;
    let (mut wr, mut wi) = (vec![1.0; n], vec![0.0; n]);
    let mut h = 1;
    while h < n {
        for j in 0..h {
            let (si, co) = (-std::f64::consts::PI * j as f64 / h as f64).sin_cos();
            wr[h + j] = co;
            wi[h + j] = si;
        }
        h *= 2;
    }
    (wr, wi)
}

#[inline(always)]
fn fma<const F: bool>(a: f64, b: f64, c: f64) -> f64 { if F { a.mul_add(b, c) } else { a * b + c } }

/// Forward decimation-in-frequency: natural order in, bit-reversed out.
#[inline(always)]
fn fft_forward<const F: bool>(re: &mut [f64], im: &mut [f64], wr: &[f64], wi: &[f64]) {
    let n = re.len();
    let mut h = n / 2;
    while h >= 1 {
        let (twr, twi) = (&wr[h..2 * h], &wi[h..2 * h]);
        for blk in (0..n).step_by(2 * h) {
            let (ar, br) = re[blk..blk + 2 * h].split_at_mut(h);
            let (ai, bi) = im[blk..blk + 2 * h].split_at_mut(h);
            for j in 0..h {
                let (xr, xi, yr, yi) = (ar[j], ai[j], br[j], bi[j]);
                let (dr, di) = (xr - yr, xi - yi);
                ar[j] = xr + yr;
                ai[j] = xi + yi;
                br[j] = fma::<F>(dr, twr[j], -(di * twi[j]));
                bi[j] = fma::<F>(dr, twi[j], di * twr[j]);
            }
        }
        h /= 2;
    }
}

/// Inverse decimation-in-time with conjugate twiddles: bit-reversed in, natural order out, scaled by n.
#[inline(always)]
fn fft_inverse<const F: bool>(re: &mut [f64], im: &mut [f64], wr: &[f64], wi: &[f64]) {
    let n = re.len();
    let mut h = 1;
    while h < n {
        let (twr, twi) = (&wr[h..2 * h], &wi[h..2 * h]);
        for blk in (0..n).step_by(2 * h) {
            let (ar, br) = re[blk..blk + 2 * h].split_at_mut(h);
            let (ai, bi) = im[blk..blk + 2 * h].split_at_mut(h);
            for j in 0..h {
                // b · conj(w)
                let tr = fma::<F>(br[j], twr[j], bi[j] * twi[j]);
                let ti = fma::<F>(bi[j], twr[j], -(br[j] * twi[j]));
                let (xr, xi) = (ar[j], ai[j]);
                ar[j] = xr + tr;
                ai[j] = xi + ti;
                br[j] = xr - tr;
                bi[j] = xi - ti;
            }
        }
        h *= 2;
    }
}

#[inline(always)]
fn fft_chunk<const F: bool>(seed: u64, s: &mut Scratch, flip: u32) -> u64 {
    let log = FFT_LOGS[seed as usize];
    let iters = (FFT_BUDGET / ((1u64 << log) * log as u64)).max(1) as u32;
    fft_square::<F>(seed, s, flip, log, iters)
}

/// A single 2K SSE FFT square for brief wakes, checked against its own per-seed references.
pub fn wake_chunk(seed: u64, s: &mut Scratch) -> u64 {
    fft_square::<false>(seed, s, NO_FLIP, 11, 1)
}

#[inline(always)]
fn fft_square<const F: bool>(seed: u64, s: &mut Scratch, flip: u32, log: u32, iters: u32) -> u64 {
    let n = 1usize << log;
    if s.fft_tw.len() <= log as usize { s.fft_tw.resize_with(log as usize + 1, || None); }
    let (wr, wi) = s.fft_tw[log as usize].get_or_insert_with(|| fft_tables(log));
    s.re.resize(n, 0.0);
    s.im.resize(n, 0.0);
    s.digits.resize(n, 0);
    let (re, im, digits) = (&mut s.re[..n], &mut s.im[..n], &mut s.digits[..n]);

    let base = 1i64 << DIGIT_BITS;
    let mut st = seed ^ 0x6A09_E667_F3BC_C908;
    for d in digits.iter_mut() { *d = (splitmix(&mut st) & (base as u64 - 1)) as i64 - base / 2; }
    let mut h = [splitmix(&mut st), splitmix(&mut st), splitmix(&mut st), splitmix(&mut st)];
    let scale = 1.0 / n as f64;
    for it in 0..iters {
        for j in 0..n { re[j] = digits[j] as f64; im[j] = 0.0; }
        fft_forward::<F>(re, im, wr, wi);
        if it == flip { re[0] = f64::from_bits(re[0].to_bits() ^ 1); }
        // The squared spectrum goes into the hash as well: a one-ulp error in one bin is spread over all n
        // outputs by the inverse transform and can round away there.
        for j in 0..n {
            let (a, b) = (re[j], im[j]);
            re[j] = fma::<F>(a, a, -(b * b));
            im[j] = 2.0 * a * b;
            h[j & 3] = fold(h[j & 3], re[j].to_bits() ^ im[j].to_bits().rotate_left(32));
        }
        fft_inverse::<F>(re, im, wr, wi);
        // Round and carry in balanced base 2^12 (cyclic: the top carry wraps to digit 0), minus 2 as in a
        // Lucas-Lehmer step so the number never settles. The hash takes every value's raw bits first.
        let mut carry = 0i64;
        for j in 0..n {
            let v = re[j] * scale;
            h[j & 3] = fold(h[j & 3], v.to_bits() ^ im[j].to_bits().rotate_left(32));
            let t = v.round() as i64 + carry;
            let d = ((t + base / 2) & (base - 1)) - base / 2;
            digits[j] = d;
            carry = (t - d) >> DIGIT_BITS;
        }
        digits[0] -= 2;
        let mut j = 0;
        while carry != 0 {
            let t = digits[j] + carry;
            let d = ((t + base / 2) & (base - 1)) - base / 2;
            digits[j] = d;
            carry = (t - d) >> DIGIT_BITS;
            j = (j + 1) % n;
        }
    }
    let mut out = fold(fold(fold(h[0], h[1]), h[2]), h[3]);
    for &d in digits.iter() { out = fold(out, d as u64); }
    out
}

// ── NTT modulo p = 2^64 - 2^32 + 1 ─────────────────────────────────────────────

const P: u64 = 0xFFFF_FFFF_0000_0001;
const EPS: u64 = 0xFFFF_FFFF;  // 2^64 mod p

#[inline(always)]
fn reduce(x: u128) -> u64 {
    let (lo, hi) = (x as u64, (x >> 64) as u64);
    let (hh, hl) = (hi >> 32, hi & EPS);
    let (mut t, borrow) = lo.overflowing_sub(hh);
    if borrow { t = t.wrapping_sub(EPS); }
    let (r, c) = t.overflowing_add(hl * EPS);
    let r = r.wrapping_add(EPS * c as u64);
    if r >= P { r - P } else { r }
}
#[inline(always)]
fn mulm(a: u64, b: u64) -> u64 { reduce(a as u128 * b as u128) }
#[inline(always)]
fn addm(a: u64, b: u64) -> u64 {
    let (s, c) = a.overflowing_add(b);
    if c || s >= P { s.wrapping_sub(P) } else { s }
}
#[inline(always)]
fn subm(a: u64, b: u64) -> u64 { if a >= b { a - b } else { a.wrapping_sub(b).wrapping_add(P) } }

fn powm(mut b: u64, mut e: u64) -> u64 {
    let mut r = 1;
    while e > 0 {
        if e & 1 == 1 { r = mulm(r, b); }
        b = mulm(b, b);
        e >>= 1;
    }
    r
}

/// Stage roots like `fft_tables`: entries h..2h are w^j and w^-j for a primitive 2h-th root w (7 generates the group).
fn ntt_tables(log: u32) -> (Vec<u64>, Vec<u64>) {
    let n = 1usize << log;
    let (mut f, mut b) = (vec![1u64; n], vec![1u64; n]);
    let mut h = 1;
    while h < n {
        let w = powm(7, (P - 1) / (2 * h as u64));
        let wi = powm(w, 2 * h as u64 - 1);
        for j in 1..h {
            f[h + j] = mulm(f[h + j - 1], w);
            b[h + j] = mulm(b[h + j - 1], wi);
        }
        h *= 2;
    }
    (f, b)
}

fn ntt_forward(x: &mut [u64], fw: &[u64]) {
    let n = x.len();
    let mut h = n / 2;
    while h >= 1 {
        let tw = &fw[h..2 * h];
        for blk in (0..n).step_by(2 * h) {
            let (a, b) = x[blk..blk + 2 * h].split_at_mut(h);
            for j in 0..h {
                let (u, v) = (a[j], b[j]);
                a[j] = addm(u, v);
                b[j] = mulm(subm(u, v), tw[j]);
            }
        }
        h /= 2;
    }
}

fn ntt_inverse(x: &mut [u64], bw: &[u64]) {
    let n = x.len();
    let mut h = 1;
    while h < n {
        let tw = &bw[h..2 * h];
        for blk in (0..n).step_by(2 * h) {
            let (a, b) = x[blk..blk + 2 * h].split_at_mut(h);
            for j in 0..h {
                let (u, v) = (a[j], mulm(b[j], tw[j]));
                a[j] = addm(u, v);
                b[j] = subm(u, v);
            }
        }
        h *= 2;
    }
}

fn ntt_chunk(seed: u64, s: &mut Scratch, flip: u32) -> u64 {
    let log = NTT_LOGS[seed as usize];
    let n = 1usize << log;
    if s.ntt_tw.len() <= log as usize { s.ntt_tw.resize_with(log as usize + 1, || None); }
    let (fw, bw) = s.ntt_tw[log as usize].get_or_insert_with(|| ntt_tables(log));
    s.ntt.resize(n, 0);
    let x = &mut s.ntt[..n];
    let mut st = seed ^ 0xBB67_AE85_84CA_A73B;
    for v in x.iter_mut() { *v = splitmix(&mut st) % P; }
    let iters = (NTT_BUDGET / (n as u64 * log as u64)).max(1) as u32;
    for it in 0..iters {
        ntt_forward(x, fw);
        if it == flip { x[0] ^= 1; }
        for v in x.iter_mut() { *v = mulm(*v, *v); }
        ntt_inverse(x, bw);
        x[0] = addm(x[0], 1);  // keeps the sequence away from fixed points such as all zeros
    }
    x.iter().fold(seed, |h, &v| fold(h, v))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn kernels() -> Vec<Kernel> { KERNELS.into_iter().filter(|k| k.supported()).collect() }

    #[test]
    fn same_seed_same_result_other_seed_other_result() {
        let mut s = Scratch::new();
        for k in kernels() {
            for seed in 0..NSEEDS {
                let a = chunk(k, seed, &mut s);
                assert_eq!(a, chunk(k, seed, &mut s), "{k:?} seed {seed}");
                assert_ne!(a, chunk(k, (seed + 1) % NSEEDS, &mut s), "{k:?} seed {seed}");
            }
        }
    }

    #[test]
    fn one_flipped_bit_changes_the_result() {
        let mut s = Scratch::new();
        for k in kernels() {
            for seed in 0..NSEEDS {
                let good = chunk(k, seed, &mut s);
                for flip in [0, 1] {
                    assert_ne!(good, chunk_with(k, seed, &mut s, flip), "{k:?} seed {seed} flip at {flip}");
                }
            }
        }
    }

    #[test]
    fn same_result_on_another_thread() {
        let mut s = Scratch::new();
        for k in kernels() {
            let here: Vec<u64> = (0..NSEEDS).map(|seed| chunk(k, seed, &mut s)).collect();
            let there = std::thread::spawn(move || { let mut s = Scratch::new(); (0..NSEEDS).map(|seed| chunk(k, seed, &mut s)).collect::<Vec<_>>() })
                .join().unwrap();
            assert_eq!(here, there, "{k:?}");
        }
    }

    /// The transforms are real ones (not just a hash): forward then inverse returns n·x.
    #[test]
    fn transforms_round_trip() {
        let log = 10;
        let n = 1usize << log;
        let (wr, wi) = fft_tables(log);
        let mut st = 1u64;
        let orig: Vec<f64> = (0..n).map(|_| (splitmix(&mut st) % 1000) as f64).collect();
        let (mut re, mut im) = (orig.clone(), vec![0.0; n]);
        fft_forward::<false>(&mut re, &mut im, &wr, &wi);
        fft_inverse::<false>(&mut re, &mut im, &wr, &wi);
        for j in 0..n { assert!((re[j] / n as f64 - orig[j]).abs() < 1e-9 && (im[j] / n as f64).abs() < 1e-9); }

        let (fw, bw) = ntt_tables(log);
        let orig: Vec<u64> = (0..n).map(|_| splitmix(&mut st) % P).collect();
        let mut x = orig.clone();
        ntt_forward(&mut x, &fw);
        ntt_inverse(&mut x, &bw);
        let ninv = powm(n as u64, P - 2);
        for j in 0..n { assert_eq!(mulm(x[j], ninv), orig[j]); }
    }

    /// `cargo test --release chunk_times -- --ignored --nocapture`: how long one chunk takes per kernel and seed.
    #[test]
    #[ignore]
    fn chunk_times() {
        let mut s = Scratch::new();
        for k in kernels() {
            let ms: Vec<String> = (0..NSEEDS).map(|seed| {
                chunk(k, seed, &mut s);
                let t = std::time::Instant::now();
                chunk(k, seed, &mut s);
                format!("{:.1}", t.elapsed().as_secs_f64() * 1e3)
            }).collect();
            println!("{} ms per chunk: {}", k.name(), ms.join(" "));
        }
    }

    #[test]
    fn modular_reduction_matches_u128() {
        let mut st = 5u64;
        for _ in 0..10_000 {
            let (a, b) = (splitmix(&mut st) % P, splitmix(&mut st) % P);
            assert_eq!(mulm(a, b) as u128, (a as u128 * b as u128) % P as u128);
            assert_eq!(addm(a, b) as u128, (a as u128 + b as u128) % P as u128);
            assert_eq!(subm(a, b) as u128, (a as u128 + P as u128 - b as u128) % P as u128);
        }
    }
}
