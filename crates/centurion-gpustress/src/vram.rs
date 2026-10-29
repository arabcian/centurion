//! VRAM mode (docs/GPUSTRESS.md §4): fills the device-local memory with patterns whose value depends only
//! on (pattern, global word index, pass seed) and reads them back, counting every wrong word.
use crate::ctx::{Buf, Ctx, Mem};
use crate::device;
use crate::{emit, end, init_failed, vk_end, State, STOP};
use ash::vk;
use serde_json::json;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

const MIB: u64 = 1 << 20;
const MIN_BYTES: u64 = 64 * MIB;   // less than this tests too little to mean anything
const KEEP_FREE: u64 = 256 * MIB;  // the desktop is using this GPU too
const CHUNK_BYTES: u64 = 256 * MIB;
const GROUP_WORDS: u64 = 256 * 4;  // one workgroup of vram.wgsl: 256 invocations x 4 words
const PATTERNS: [&str; 5] = ["addr", "walk1", "walk0", "solid", "random"];
const SPV: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/vram.spv"));

// The patterns in Rust: the fault injection and the unit test need the expected value on the host, and
// the test runs vram.wgsl to prove both copies agree. The shader only has 32-bit integers, so the global
// word index (u64) travels as two halves and addr / random mix hash(high half) in: words 2^32 apart (16 GiB)
// still differ. walk1 / walk0 depend on the index mod 32 only, which 2^32 does not change.
#[cfg(any(test, feature = "fault-inject"))]
fn hash(x: u32) -> u32 {
    let mut h = x;
    h ^= h >> 16;
    h = h.wrapping_mul(0x85eb_ca6b);
    h ^= h >> 13;
    h = h.wrapping_mul(0xc2b2_ae35);
    h ^ (h >> 16)
}

#[cfg(any(test, feature = "fault-inject"))]
fn value(pattern: u32, word: u64, seed: u32, phase: u32) -> u32 {
    let (index, hi) = (word as u32, (word >> 32) as u32);
    match pattern {
        0 => index ^ hash(seed) ^ hash(hi),
        1 => 1u32 << (index.wrapping_add(seed) % 32),
        2 => !(1u32 << (index.wrapping_add(seed) % 32)),
        3 => if seed.wrapping_add(phase) & 1 == 1 { u32::MAX } else { 0 },
        _ => hash(index ^ hash(seed.wrapping_add(0x9e37_79b9)) ^ hash(hi)),
    }
}

/// 2D dispatch size for `words` words, None when the device cannot run 256-wide groups or that many.
fn dims(l: &vk::PhysicalDeviceLimits, words: u64) -> Option<(u32, u32)> {
    let groups = words.div_ceil(GROUP_WORDS).max(1);
    let x = groups.min(l.max_compute_work_group_count[0] as u64);
    let y = groups.div_ceil(x);
    (y <= l.max_compute_work_group_count[1] as u64 && l.max_compute_work_group_size[0] >= 256 && l.max_compute_work_group_invocations >= 256)
        .then_some((x as u32, y as u32))
}

struct Rig {
    ctx: Ctx,
    chunks: Vec<Buf>,
    sets: Vec<vk::DescriptorSet>,
    stride: u32,  // words per chunk in the global word index (every chunk but the last is this big)
    layout: vk::PipelineLayout,
    fill: vk::Pipeline,
    verify: vk::Pipeline,
    params: Buf,
    found: Buf,  // mismatch count, then the first 16 as (index, expected, actual)
}

impl Rig {
    fn new(ctx: Ctx, chunks: Vec<Buf>, stride: u32) -> Result<Rig, vk::Result> {
        let biggest = chunks.iter().map(|c| c.size / 4).max().unwrap_or(0);
        dims(&ctx.limits, biggest).ok_or(vk::Result::ERROR_FEATURE_NOT_PRESENT)?;
        let kinds = [vk::DescriptorType::STORAGE_BUFFER, vk::DescriptorType::UNIFORM_BUFFER, vk::DescriptorType::STORAGE_BUFFER];
        let (dsl, layout) = ctx.layout(&kinds)?;
        let params = ctx.buffer(32, vk::BufferUsageFlags::UNIFORM_BUFFER, Mem::Host)?;
        let found = ctx.buffer(4 * 49, vk::BufferUsageFlags::STORAGE_BUFFER, Mem::Host)?;
        let sets = ctx.sets(dsl, &kinds, chunks.len() as u32)?;
        for (s, c) in sets.iter().zip(&chunks) {
            ctx.bind(*s, &[(kinds[0], c), (kinds[1], &params), (kinds[2], &found)]);
        }
        let module = ctx.module(SPV)?;
        let fill = ctx.pipeline(layout, module, c"fill")?;
        let verify = ctx.pipeline(layout, module, c"verify")?;
        Ok(Rig { ctx, chunks, sets, stride, layout, fill, verify, params, found })
    }

    /// One submission: `fill` or `verify` over chunk `i`. Returns how many words were wrong and the first few.
    fn run(&self, i: usize, verify: bool, pattern: u32, seed: u32, phase: u32) -> Result<(u32, Vec<[u64; 3]>), vk::Result> {
        let words = (self.chunks[i].size / 4) as u32;
        let (x, y) = dims(&self.ctx.limits, words as u64).ok_or(vk::Result::ERROR_FEATURE_NOT_PRESENT)?;
        let base = i as u64 * self.stride as u64;
        for (k, v) in [pattern, seed, base as u32, words, phase, (base >> 32) as u32].iter().enumerate() { self.params.set(k, *v); }
        self.found.set(0, 0);
        // SAFETY: recording into the command buffer `submit` has begun, with handles made by `new`.
        self.ctx.submit(|d, cmd| unsafe {
            d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::COMPUTE, if verify { self.verify } else { self.fill });
            d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::COMPUTE, self.layout, 0, &[self.sets[i]], &[]);
            d.cmd_dispatch(cmd, x, y, 1);
        })?;
        // The shader reports the low half of each index; the offset in the chunk is below 2^32, so add it to the full base.
        let (n, mut first) = self.found.mismatches();
        for f in &mut first { f[0] = base + (f[0] as u32).wrapping_sub(base as u32) as u64; }
        Ok((n, first))
    }
}

enum Halt { Deadline, Stopped }

fn halt(t0: Instant, seconds: u64) -> Option<Halt> {
    if STOP.load(Ordering::SeqCst) { Some(Halt::Stopped) }
    else if t0.elapsed() >= Duration::from_secs(seconds) { Some(Halt::Deadline) }
    else { None }
}

/// Pass 3 only: after its write dispatch a fill flips one bit of one word (the last chunk, so the global
/// index is beyond the first chunk's range whenever there is more than one). The word and the value it
/// should hold go to stderr for the integration test to compare with the `error` line.
#[cfg(feature = "fault-inject")]
fn inject(rig: &Rig, pattern: u32, seed: u32, phase: u32) -> Result<(), vk::Result> {
    let last = rig.chunks.len() - 1;
    let off = 12345.min(rig.chunks[last].size / 4 - 1);
    let index = last as u64 * rig.stride as u64 + off;
    let want = value(pattern, index, seed, phase);
    eprintln!("fault-inject: word {index} expected 0x{want:08x}");
    let to_fill = vk::MemoryBarrier::default().src_access_mask(vk::AccessFlags::SHADER_WRITE).dst_access_mask(vk::AccessFlags::TRANSFER_WRITE);
    let from_fill = vk::MemoryBarrier::default().src_access_mask(vk::AccessFlags::TRANSFER_WRITE).dst_access_mask(vk::AccessFlags::SHADER_READ);
    // SAFETY: recording into the command buffer `submit` has begun.
    rig.ctx.submit(|d, cmd| unsafe {
        d.cmd_pipeline_barrier(cmd, vk::PipelineStageFlags::COMPUTE_SHADER, vk::PipelineStageFlags::TRANSFER,
            vk::DependencyFlags::empty(), &[to_fill], &[], &[]);
        d.cmd_fill_buffer(cmd, rig.chunks[last].buf, off * 4, 4, want ^ 1);
        d.cmd_pipeline_barrier(cmd, vk::PipelineStageFlags::TRANSFER, vk::PipelineStageFlags::COMPUTE_SHADER,
            vk::DependencyFlags::empty(), &[from_fill], &[], &[]);
    })
}

/// One pass: its pattern over every chunk (the solid pattern twice, with the inverse the second time).
fn pass(rig: &Rig, n: u64, t0: Instant, seconds: u64, st: &mut State) -> Result<Option<Halt>, vk::Result> {
    let pattern = ((n - 1) % PATTERNS.len() as u64) as u32;
    let seed = n as u32;
    let mut lines = 0;
    for phase in 0..if pattern == 3 { 2 } else { 1 } {
        for i in 0..rig.chunks.len() {
            if let Some(h) = halt(t0, seconds) { return Ok(Some(h)); }
            rig.run(i, false, pattern, seed, phase)?;
            st.progress(t0);
        }
        #[cfg(feature = "fault-inject")]
        if n == 3 && phase == 0 { inject(rig, pattern, seed, phase)?; }
        for i in 0..rig.chunks.len() {
            if let Some(h) = halt(t0, seconds) { return Ok(Some(h)); }
            let (count, first) = rig.run(i, true, pattern, seed, phase)?;
            st.bytes_checked += rig.chunks[i].size;
            st.wrong(t0, &mut lines, n, PATTERNS[pattern as usize], count, &first);
            st.progress(t0);
        }
    }
    st.passes += 1;
    Ok(None)
}

pub fn run(pci: &str, percent: u32, seconds: u64) -> i32 {
    let (_entry, instance) = device::open().unwrap_or_else(|e| init_failed(&e));
    let gpus = device::list(&instance).unwrap_or_else(|e| init_failed(&e));
    let gpu = device::find(&gpus, pci).unwrap_or_else(|e| init_failed(&e));
    let ctx = Ctx::new(&instance, gpu).unwrap_or_else(|e| init_failed(&format!("could not create the Vulkan device: {e}")));

    let (heap, budget) = ctx.heap_budget();
    let target = (budget * percent as u64 / 100).min(budget.saturating_sub(KEEP_FREE)) & !(MIB - 1);
    let chunk = CHUNK_BYTES.min(ctx.limits.max_storage_buffer_range as u64).min(ctx.max_alloc) & !4095;
    if chunk == 0 { init_failed("the device reports no usable buffer size"); }
    // TRANSFER_DST only for the fault injection's fill.
    let usage = vk::BufferUsageFlags::STORAGE_BUFFER | if cfg!(feature = "fault-inject") { vk::BufferUsageFlags::TRANSFER_DST } else { vk::BufferUsageFlags::empty() };
    let mut chunks = Vec::new();
    let mut got = 0;
    while got < target {
        let size = chunk.min(target - got);
        match ctx.buffer(size, usage, Mem::Device(heap)) {
            Ok(b) => { got += size; chunks.push(b); }
            Err(vk::Result::ERROR_OUT_OF_DEVICE_MEMORY) => break,  // test what was obtained
            Err(e) => init_failed(&format!("could not allocate VRAM: {e}")),
        }
    }
    if got < MIN_BYTES { init_failed("could not allocate enough VRAM"); }

    let rig = Rig::new(ctx, chunks, (chunk / 4) as u32).unwrap_or_else(|e| init_failed(&format!("could not set up the compute pipeline: {e}")));
    emit(&json!({"ev": "start", "mode": "vram", "pci": gpu.pci, "name": gpu.name, "driver": gpu.driver, "bytes": got, "chunks": rig.chunks.len()}));

    let t0 = Instant::now();  // --seconds counts from here, after the start-up
    let mut st = State::default();
    let outcome = (|| loop {
        if let Some(h) = pass(&rig, st.passes + 1, t0, seconds, &mut st)? { return Ok::<Halt, vk::Result>(h); }
    })();
    let t = t0.elapsed().as_secs_f64();
    match outcome {
        Ok(Halt::Stopped) => end("stopped", t, st.passes, st.errors, "stopped"),
        Ok(Halt::Deadline) if st.errors > 0 => end("fail", t, st.passes, st.errors, &format!("{} wrong word(s)", st.errors)),
        Ok(Halt::Deadline) => end("pass", t, st.passes, st.errors, "no errors"),
        Err(e) => vk_end(e, t, st.passes, st.errors),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The Rust patterns must equal vram.wgsl: run the shader on a CPU device (lavapipe) and compare every word,
    /// then check that `verify` accepts the data and reports one corrupted word exactly.
    #[test]
    fn shader_matches_rust() {
        for pattern in [0, 4] {
            assert_ne!(value(pattern, 5, 1, 0), value(pattern, 5 + (1 << 32), 1, 0), "pattern {pattern}: words 2^32 apart must differ");
        }
        let Ok((_entry, instance)) = device::open() else { eprintln!("skipped: no Vulkan"); return; };
        let gpus = device::list(&instance).unwrap();
        let Some(gpu) = gpus.iter().find(|g| g.kind == "cpu") else { eprintln!("skipped: no CPU Vulkan device"); return; };
        let ctx = Ctx::new(&instance, gpu).unwrap();
        const WORDS: u32 = 1 << 16;
        // Not a power of two, and 2 x STRIDE is 200 words short of 2^32: chunk 2 straddles the 2^32 boundary and
        // chunk 3 lies above it, so the carry and a nonzero base_hi are both exercised.
        const STRIDE: u32 = 0x8000_0000 - 100;
        const CHUNKS: usize = 4;
        let chunks: Vec<Buf> = (0..CHUNKS).map(|_| ctx.buffer(WORDS as u64 * 4, vk::BufferUsageFlags::STORAGE_BUFFER, Mem::Host).unwrap()).collect();
        let rig = Rig::new(ctx, chunks, STRIDE).unwrap();
        for pattern in 0..5 {
            for seed in [1, 4, 5, 70_001] {
                for phase in 0..2 {
                    for i in 0..CHUNKS {
                        let base = i as u64 * STRIDE as u64;
                        rig.run(i, false, pattern, seed, phase).unwrap();
                        for w in 0..WORDS {
                            assert_eq!(rig.chunks[i].get(w as usize), value(pattern, base + w as u64, seed, phase),
                                "pattern {pattern} seed {seed} phase {phase} chunk {i} word {w}");
                        }
                        assert_eq!(rig.run(i, true, pattern, seed, phase).unwrap().0, 0);
                        let w = 4321 + i as u32;
                        let want = value(pattern, base + w as u64, seed, phase);
                        rig.chunks[i].set(w as usize, want ^ 0x10);
                        let (n, first) = rig.run(i, true, pattern, seed, phase).unwrap();
                        assert_eq!((n, first), (1, vec![[base + w as u64, want as u64, (want ^ 0x10) as u64]]));
                    }
                }
            }
        }
    }
}
