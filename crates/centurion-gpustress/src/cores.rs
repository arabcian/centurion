//! Core mode (docs/GPUSTRESS.md §5): runs the dependent arithmetic of core.wgsl and compares each run,
//! word by word on the GPU, with a reference run of the same work. A correct GPU gives the same bits
//! every time, so any difference is a fault; no result needs to be known in advance.
use crate::ctx::{Buf, Ctx, Mem};
use crate::device;
use crate::{emit, end, init_failed, vk_end, State, STOP};
use ash::vk;
use serde_json::json;
use std::time::{Duration, Instant};

const INVOCATIONS: u32 = 1 << 20;  // fills any current GPU
const GROUP: u32 = 256;
const BYTES: u64 = INVOCATIONS as u64 * 4 * 4;
const ROTATE: Duration = Duration::from_secs(60);  // the seed (and with it the reference) changes this often
const NO_PERTURB: u32 = u32::MAX;
const SPV: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/core.spv"));

struct Rig {
    ctx: Ctx,
    sets: [vk::DescriptorSet; 2],  // [0] burns into `cur`, [1] burns into `reference`; compare reads [0]
    layout: vk::PipelineLayout,
    burn: vk::Pipeline,
    compare: vk::Pipeline,
    params: Buf,
    found: Buf,
}

impl Rig {
    fn new(ctx: Ctx, cur: &Buf, reference: &Buf) -> Result<Rig, vk::Result> {
        let l = &ctx.limits;
        if l.max_compute_work_group_size[0] < GROUP || l.max_compute_work_group_invocations < GROUP || l.max_compute_work_group_count[0] < INVOCATIONS / GROUP {
            return Err(vk::Result::ERROR_FEATURE_NOT_PRESENT);
        }
        let (st, un) = (vk::DescriptorType::STORAGE_BUFFER, vk::DescriptorType::UNIFORM_BUFFER);
        let kinds = [st, un, st, st];
        let (dsl, layout) = ctx.layout(&kinds)?;
        let params = ctx.buffer(16, vk::BufferUsageFlags::UNIFORM_BUFFER, Mem::Host)?;
        let found = ctx.buffer(4 * 49, vk::BufferUsageFlags::STORAGE_BUFFER, Mem::Host)?;
        let sets = ctx.sets(dsl, &kinds, 2)?;
        ctx.bind(sets[0], &[(st, cur), (un, &params), (st, reference), (st, &found)]);
        ctx.bind(sets[1], &[(st, reference), (un, &params), (st, cur), (st, &found)]);
        let module = ctx.module(SPV)?;
        let burn = ctx.pipeline(layout, module, c"burn")?;
        let compare = ctx.pipeline(layout, module, c"compare")?;
        Ok(Rig { ctx, sets: [sets[0], sets[1]], layout, burn, compare, params, found })
    }

    fn dispatch(&self, pipeline: vk::Pipeline, set: vk::DescriptorSet) -> Result<f64, vk::Result> {
        let t = Instant::now();
        // SAFETY: recording into the command buffer `submit` has begun, with handles made by `new`.
        self.ctx.submit(|d, cmd| unsafe {
            d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::COMPUTE, pipeline);
            d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::COMPUTE, self.layout, 0, &[set], &[]);
            d.cmd_dispatch(cmd, INVOCATIONS / GROUP, 1, 1);
        })?;
        Ok(t.elapsed().as_secs_f64())
    }

    /// One run of the arithmetic, into the reference buffer or the current one. Returns how long it took.
    fn burn(&self, into_reference: bool, n: u32, seed: u32, perturb: u32) -> Result<f64, vk::Result> {
        for (k, v) in [n, seed, perturb, 0].iter().enumerate() { self.params.set(k, *v); }
        self.dispatch(self.burn, self.sets[into_reference as usize])
    }

    /// The current run against the reference: how many words differ and the first few.
    fn compare(&self) -> Result<(u32, Vec<[u64; 3]>), vk::Result> {
        self.found.set(0, 0);
        self.dispatch(self.compare, self.sets[0])?;
        Ok(self.found.mismatches())
    }

    /// Scales `n` so one dispatch takes about 100 ms, and never more than 500 ms: any timing between 50 ms
    /// and 500 ms is accepted, and the n returned is one that was timed. A timing at a small n is mostly
    /// launch overhead, so n grows at most 64x a round; it is capped so a bad timing cannot start a dispatch
    /// long enough to trip a driver watchdog.
    fn calibrate(&self, mut n: u32, seed: u32) -> Result<u32, vk::Result> {
        for _ in 0..6 {
            let t = self.burn(true, n, seed, NO_PERTURB)?;
            if (0.05..=0.5).contains(&t) { break; }
            n = (n as f64 * 0.1 / t.max(1e-6)).clamp(1.0, n as f64 * 64.0).min((1 << 20) as f64) as u32;
        }
        Ok(n)
    }
}

enum Halt { Deadline, Stopped, Unstable }

fn halt(deadline: Option<Instant>) -> Option<Halt> {
    if STOP.load(std::sync::atomic::Ordering::SeqCst) { Some(Halt::Stopped) }
    else if deadline.is_some_and(|d| Instant::now() >= d) { Some(Halt::Deadline) }
    else { None }
}

/// The third run has one invocation's input disturbed (fault-inject build only): word 12345 * 4.
fn perturbed(passes: u64) -> u32 {
    if cfg!(feature = "fault-inject") && passes == 2 {
        eprintln!("fault-inject: word {}", 12345 * 4);
        12345
    } else {
        NO_PERTURB
    }
}

/// Two runs of the same work must agree: the first is then the reference. Words that differ are errors; up
/// to 3 retries. False when no two runs agreed.
fn reference(rig: &Rig, n: u32, seed: u32, t0: Instant, st: &mut State) -> Result<bool, vk::Result> {
    for _ in 0..4 {
        rig.burn(true, n, seed, NO_PERTURB)?;
        rig.burn(false, n, seed, NO_PERTURB)?;
        let (count, first) = rig.compare()?;
        if count == 0 { return Ok(true); }
        st.wrong(t0, &mut 0, st.passes + 1, "core", count, &first);
    }
    Ok(false)
}

fn test(rig: &Rig, t0: Instant, seconds: u64, st: &mut State) -> Result<Halt, vk::Result> {
    // --seconds is the test time after start-up: the first calibration and reference do not count against it.
    let mut deadline = None;
    let mut n = 1024;
    for seed in 1u32.. {
        if let Some(h) = halt(deadline) { return Ok(h); }
        n = rig.calibrate(n, seed)?;  // clocks may have settled since the last seed
        if !reference(rig, n, seed, t0, st)? { return Ok(Halt::Unstable); }
        let deadline_now = *deadline.get_or_insert_with(|| Instant::now() + Duration::from_secs(seconds));
        let rotate_at = Instant::now() + ROTATE;
        while Instant::now() < rotate_at.min(deadline_now) {
            if let Some(h) = halt(deadline) { return Ok(h); }
            rig.burn(false, n, seed, perturbed(st.passes))?;
            let (count, first) = rig.compare()?;
            st.bytes_checked += BYTES;
            st.wrong(t0, &mut 0, st.passes + 1, "core", count, &first);
            st.passes += 1;
            st.progress(t0);
        }
    }
    unreachable!("the seed loop only ends through a return")
}

pub fn run(pci: &str, seconds: u64) -> i32 {
    let (_entry, instance) = device::open().unwrap_or_else(|e| init_failed(&e));
    let gpus = device::list(&instance).unwrap_or_else(|e| init_failed(&e));
    let gpu = device::find(&gpus, pci).unwrap_or_else(|e| init_failed(&e));
    let ctx = Ctx::new(&instance, gpu).unwrap_or_else(|e| init_failed(&format!("could not create the Vulkan device: {e}")));
    let (heap, _) = ctx.heap_budget();
    let alloc = || ctx.buffer(BYTES, vk::BufferUsageFlags::STORAGE_BUFFER, Mem::Device(heap));
    let (cur, reference) = (alloc(), alloc());
    let (Ok(cur), Ok(reference)) = (cur, reference) else { init_failed("could not allocate the output buffers") };
    let rig = Rig::new(ctx, &cur, &reference).unwrap_or_else(|e| init_failed(&format!("could not set up the compute pipeline: {e}")));
    // bytes: what one run compares; there is no chunking in this mode.
    emit(&json!({"ev": "start", "mode": "core", "pci": gpu.pci, "name": gpu.name, "driver": gpu.driver, "bytes": BYTES, "chunks": 1}));

    let t0 = Instant::now();
    let mut st = State::default();
    let outcome = test(&rig, t0, seconds, &mut st);
    let t = t0.elapsed().as_secs_f64();
    match outcome {
        Ok(Halt::Stopped) => end("stopped", t, st.passes, st.errors, "stopped"),
        Ok(Halt::Unstable) => end("fail", t, st.passes, st.errors, "no two runs of the same work agreed"),
        Ok(Halt::Deadline) if st.errors > 0 => end("fail", t, st.passes, st.errors, &format!("{} wrong word(s)", st.errors)),
        Ok(Halt::Deadline) => end("pass", t, st.passes, st.errors, "no errors"),
        Err(e) => vk_end(e, t, st.passes, st.errors),
    }
}
