//! Short benchmarks for centurion-calibrate. Every figure comes from a workload that
//! is gone afterwards: the heap probe is a child process (its memory - THP
//! bloat included - is returned when it exits), the I/O files are unlinked
//! O_TMPFILEs, and the caller drops caches + compacts between runs. Nothing
//! depends on how much memory the rest of the system happens to use.

use crate::calib::{Metric, Sample};
use serde_json::{json, Value};
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

const MIB: usize = 1 << 20;

pub fn p99(v: &mut Vec<f64>) -> Option<f64> {
    if v.is_empty() { return None; }
    v.sort_by(|a, b| a.total_cmp(b));
    Some(v[((v.len() as f64 * 0.99) as usize).min(v.len() - 1)])
}

/// Latency tail of one window: the mean of the worst 1 % of the samples, at least 5 (all of
/// a smaller sample) - the expected shortfall at p99. A single order statistic of a short
/// window jitters far more than the effects being measured (p99 of 40 fsyncs is simply the
/// maximum); the mean of the worst few keeps the tail's meaning at a fraction of the noise.
pub fn tail(v: &mut Vec<f64>) -> Option<f64> {
    if v.is_empty() { return None; }
    v.sort_by(|a, b| b.total_cmp(a));
    let k = ((v.len() as f64 * 0.01).ceil() as usize).max(5).min(v.len());
    Some(v[..k].iter().sum::<f64>() / k as f64)
}

fn anon(len: usize) -> Option<&'static mut [u8]> {
    let p = unsafe { libc::mmap(std::ptr::null_mut(), len, libc::PROT_READ | libc::PROT_WRITE,
                                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS, -1, 0) };
    (p != libc::MAP_FAILED).then(|| unsafe { std::slice::from_raw_parts_mut(p as *mut u8, len) })
}
fn unmap(s: &mut [u8]) { unsafe { libc::munmap(s.as_mut_ptr() as *mut libc::c_void, s.len()); } }

/// Timer wake-up overshoot p99 (µs) of a 500 µs sleep loop while another
/// thread churns anonymous memory (faults, THP allocation, reclaim paths).
/// The sleeper is a fresh thread in each of 3 sub-windows: where the scheduler
/// happens to place it (V-Cache or frequency CCD, SMT sibling of a busy core)
/// is averaged over instead of deciding the whole run.
pub fn wake_p99_us(dur: Duration) -> Option<f64> {
    let stop = Arc::new(AtomicBool::new(false));
    let s2 = stop.clone();
    let churn = std::thread::spawn(move || {
        while !s2.load(Ordering::Relaxed) {
            if let Some(m) = anon(64 * MIB) {
                for i in (0..m.len()).step_by(4096) { m[i] = 1; }
                unmap(m);
            }
        }
    });
    let mut lat = Vec::with_capacity(4096);
    for _ in 0..3 {
        let part = std::thread::spawn(move || {
            let mut v = Vec::with_capacity(1024);
            let t0 = Instant::now();
            while t0.elapsed() < dur / 3 {
                let a = Instant::now();
                std::thread::sleep(Duration::from_micros(500));
                v.push((a.elapsed().as_nanos() as f64 / 1000.0 - 500.0).max(0.0));
            }
            v
        });
        lat.extend(part.join().unwrap_or_default());
    }
    stop.store(true, Ordering::Relaxed);
    let _ = churn.join();
    tail(&mut lat)
}

fn smaps_kb(field: &str) -> Option<f64> {
    std::fs::read_to_string("/proc/self/smaps_rollup").ok()?.lines()
        .find_map(|l| l.strip_prefix(field)?.trim().trim_end_matches("kB").trim().parse().ok())
}

/// Anonymous mapping of `len` bytes starting on a 2 MiB boundary (so THP can back all of it).
fn anon_aligned(len: usize) -> Option<&'static mut [u8]> {
    const H: usize = 2 * MIB;
    let raw = anon(len + H)?;
    let p = raw.as_mut_ptr() as usize;
    let a = (p + H - 1) & !(H - 1);
    unsafe {
        if a > p { libc::munmap(p as *mut libc::c_void, a - p); }
        let end = p + len + H;
        if end > a + len { libc::munmap((a + len) as *mut libc::c_void, end - a - len); }
        Some(std::slice::from_raw_parts_mut(a as *mut u8, len))
    }
}

/// Dependent random loads over `m` (one per 4 KiB page, pages visited in one random cycle,
/// the line inside each page varied): ns per load. The chain defeats prefetch, so every
/// step pays DRAM latency plus the page walk a TLB miss costs; huge pages shrink the walk.
fn chase(m: &mut [u8], steps: usize, seed: u64) -> Option<f64> {
    let pages = m.len() / 4096;
    if pages < 64 { return None; }
    let n = pages;
    let mut x = seed | 1;
    let mut rnd = || { x ^= x << 13; x ^= x >> 7; x ^= x << 17; x };
    let mut perm: Vec<u32> = (0..n as u32).collect();
    for i in (1..n).rev() { let j = (rnd() % i as u64) as usize; perm.swap(i, j); }  // Sattolo: one cycle
    let slot = |p: u32| p as usize * 4096 + ((p as usize * 7) & 63) * 64;
    let words = m.as_mut_ptr() as *mut u64;
    for i in 0..n { unsafe { *words.add(slot(perm[i]) / 8) = perm[(i + 1) % n] as u64; } }
    let mut at = perm[0];
    for _ in 0..n.min(steps / 4) { at = unsafe { std::ptr::read_volatile(words.add(slot(at) / 8)) } as u32; }  // warm-up
    let t = Instant::now();
    for _ in 0..steps { at = unsafe { std::ptr::read_volatile(words.add(slot(at) / 8)) } as u32; }
    std::hint::black_box(at);
    Some(t.elapsed().as_nanos() as f64 / steps as f64)
}

/// Faults `m` in, timing each 2 MiB: (p99 µs, total ms).
fn fault_in(m: &mut [u8]) -> (Option<f64>, f64) {
    let mut per = Vec::with_capacity(m.len() / (2 * MIB) + 1);
    let t0 = Instant::now();
    for chunk in (0..m.len()).step_by(2 * MIB) {
        let a = Instant::now();
        for i in (chunk..(chunk + 2 * MIB).min(m.len())).step_by(4096) { m[i] = 1; }
        per.push(a.elapsed().as_nanos() as f64 / 1000.0);
    }
    (tail(&mut per), t0.elapsed().as_secs_f64() * 1000.0)
}

/// Runs inside the disposable child (`centurion-calibrate __probe`): sparse heap footprint, dense
/// fault latency, memcpy bandwidth, and what THP does to TLB reach and fault cost for a plain
/// heap, a heap that opts in (MADV_HUGEPAGE) and shared memory (memfd). Prints one JSON line.
pub fn probe_main(heap_mib: usize) -> Value {
    let heap: usize = heap_mib.clamp(64, 1024) * MIB;
    let steps = heap / 1024;  // 512 MiB -> 512 k dependent loads per heap (~50 ms)
    let Some(h) = anon_aligned(heap) else { return json!({"error": "mmap failed"}) };
    // Sparse: one byte per 64 KiB - what a fragmented heap looks like. With THP
    // "always" every touched 2 MiB range may become a whole huge page.
    let base = smaps_kb("Rss:").unwrap_or(0.0);
    for i in (0..heap).step_by(64 * 1024) { h[i] = 1; }
    let sparse_rss = (smaps_kb("Rss:").unwrap_or(0.0) - base).max(0.0) / 1024.0;
    let thp_kb = smaps_kb("AnonHugePages:").unwrap_or(0.0);
    unmap(h);
    // Dense, plain: fault a fresh heap in, timing each 2 MiB.
    let Some(d) = anon_aligned(heap) else { return json!({"error": "mmap failed"}) };
    let (fault_p99, alloc_ms) = fault_in(d);
    // Huge-page coverage of the dense heap (THP success under fragmentation).
    let thp_pct = smaps_kb("AnonHugePages:").unwrap_or(0.0) * 1024.0 * 100.0 / heap as f64;
    // Bandwidth: copy between two halves.
    let (a, b) = d.split_at_mut(heap / 2);
    let t1 = Instant::now();
    let rounds = 6;
    for r in 0..rounds { if r % 2 == 0 { b.copy_from_slice(a) } else { a.copy_from_slice(b) } }
    let gbs = (rounds * heap / 2) as f64 / t1.elapsed().as_secs_f64() / 1e9;
    std::hint::black_box(&d[heap - 1]);
    let tlb_rand = chase(d, steps, 0x5EED_0001);
    unmap(d);
    // Opted in: MADV_HUGEPAGE heap (thp.enabled=madvise|always, defrag decides the stalls).
    let (mut fault_huge, mut tlb_huge) = (None, None);
    if let Some(m) = anon_aligned(heap) {
        unsafe { libc::madvise(m.as_mut_ptr() as *mut libc::c_void, heap, libc::MADV_HUGEPAGE); }
        fault_huge = fault_in(m).0;
        tlb_huge = chase(m, steps, 0x5EED_0002);
        unmap(m);
    }
    // Shared memory (memfd, not madvised): thp.shmem_enabled.
    let mut shm_rand = None;
    let len = heap / 2;
    let fd = unsafe { libc::memfd_create(b"centurion-probe\0".as_ptr() as *const libc::c_char, 0) };
    if fd >= 0 {
        if unsafe { libc::ftruncate(fd, len as libc::off_t) } == 0 {
            let p = unsafe { libc::mmap(std::ptr::null_mut(), len, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_SHARED, fd, 0) };
            if p != libc::MAP_FAILED {
                let m = unsafe { std::slice::from_raw_parts_mut(p as *mut u8, len) };
                let _ = fault_in(m);
                shm_rand = chase(m, steps / 2, 0x5EED_0003);
                unmap(m);
            }
        }
        unsafe { libc::close(fd); }
    }
    json!({"probe_rss_mib": sparse_rss, "anon_huge_kb": thp_kb, "fault_p99_us": fault_p99, "alloc_ms": alloc_ms, "membw_gbs": gbs,
           "thp_pct": thp_pct, "tlb_rand_ns": tlb_rand, "tlb_huge_ns": tlb_huge, "fault_huge_p99_us": fault_huge, "shm_rand_ns": shm_rand})
}

/// Runs the probe in a child process (`exe __probe`) and folds it into `s`.
pub fn probe_child(exe: &std::path::Path, heap_mib: usize, s: &mut Sample) -> Result<(), String> {
    let out = std::process::Command::new(exe).arg("__probe").arg(heap_mib.to_string()).output().map_err(|e| format!("probe: {e}"))?;
    let v: Value = serde_json::from_slice(&out.stdout).map_err(|_| "probe: bad output".to_string())?;
    if let Some(e) = v["error"].as_str() { return Err(format!("probe: {e}")); }
    for (m, k) in [(Metric::ProbeRssMib, "probe_rss_mib"), (Metric::FaultP99Us, "fault_p99_us"), (Metric::AllocMs, "alloc_ms"),
                   (Metric::MemBwGbs, "membw_gbs"), (Metric::ThpPct, "thp_pct"), (Metric::TlbRandNs, "tlb_rand_ns"),
                   (Metric::TlbHugeNs, "tlb_huge_ns"), (Metric::FaultHugeP99Us, "fault_huge_p99_us"), (Metric::ShmRandNs, "shm_rand_ns")] {
        if let Some(x) = v[k].as_f64() { s.insert(m, x); }
    }
    Ok(())
}

/// CPU time used by everything that is not this measurement: user-space processes other than
/// this one and its children (kernel threads - kswapd, kcompactd, kworkers - are part of what
/// the knobs change and stay out). `snapshot` then `busy_cpus(&snap, secs)` = CPUs kept busy.
pub struct Foreign(std::collections::HashMap<u32, u64>);

impl Foreign {
    pub fn snapshot(skip: &[u32]) -> Foreign {
        let me = std::process::id();
        let mut m = std::collections::HashMap::new();
        let Ok(rd) = std::fs::read_dir("/proc") else { return Foreign(m) };
        for e in rd.flatten() {
            let Some(pid) = e.file_name().to_str().and_then(|s| s.parse::<u32>().ok()) else { continue };
            if pid == me || skip.contains(&pid) { continue; }
            let Ok(st) = std::fs::read_to_string(format!("/proc/{pid}/stat")) else { continue };
            let Some(rest) = st.rfind(')').map(|i| &st[i + 1..]) else { continue };
            let f: Vec<&str> = rest.split_whitespace().collect();
            if f.len() < 13 { continue; }
            let num = |i: usize| f[i].parse::<u64>().unwrap_or(0);
            if num(6) & 0x0020_0000 != 0 || num(1) as u32 == me || skip.contains(&(num(1) as u32)) { continue; }  // PF_KTHREAD, our children
            m.insert(pid, num(11) + num(12));
        }
        Foreign(m)
    }
    /// CPUs the others kept busy between `self` and now.
    pub fn busy_cpus(&self, skip: &[u32], secs: f64) -> f64 {
        let hz = unsafe { libc::sysconf(libc::_SC_CLK_TCK) }.max(1) as f64;
        let now = Foreign::snapshot(skip);
        let ticks: u64 = now.0.iter().map(|(p, t)| t.saturating_sub(self.0.get(p).copied().unwrap_or(0))).sum();
        ticks as f64 / hz / secs.max(0.1)
    }
}

fn tmpfile(dir: &str, direct: bool) -> std::io::Result<std::fs::File> {
    std::fs::OpenOptions::new().read(true).write(true).mode(0o600)
        .custom_flags(libc::O_TMPFILE | libc::O_CLOEXEC | if direct { libc::O_DIRECT } else { 0 }).open(dir)
}

fn dev_split(dev: u64) -> (u64, u64) { (((dev >> 8) & 0xfff) | ((dev >> 32) & !0xfff), (dev & 0xff) | ((dev >> 12) & !0xff)) }

/// Whole disk (nvme0n1, sda, ...) holding `dir`'s file system: the mount from mountinfo
/// (btrfs and overlay report an anonymous st_dev), through device-mapper / md slaves (LUKS,
/// LVM, RAID: the first member) and partitions. Err for tmpfs, network and other file systems
/// without a local disk: the storage benchmark would measure RAM or the network there.
pub fn disk_of(dir: &str) -> Result<String, String> {
    let path = std::fs::canonicalize(dir).map_err(|e| format!("{dir}: {e}"))?;
    let mi = std::fs::read_to_string("/proc/self/mountinfo").map_err(|e| format!("mountinfo: {e}"))?;
    let unesc = |s: &str| s.replace("\\040", " ").replace("\\011", "\t").replace("\\134", "\\");
    let mut best: Option<(usize, String, String, String)> = None;
    for l in mi.lines() {
        let Some((pre, post)) = l.split_once(" - ") else { continue };
        let f: Vec<&str> = pre.split_whitespace().collect();
        let g: Vec<&str> = post.split_whitespace().collect();
        if f.len() < 5 || g.len() < 2 { continue; }
        let mp = unesc(f[4]);
        if !path.starts_with(&mp) { continue; }
        // Later lines win on the same mount point (stacked mounts).
        if best.as_ref().map_or(true, |b| mp.len() >= b.0) { best = Some((mp.len(), f[2].to_owned(), g[0].to_owned(), unesc(g[1]))); }
    }
    let (_, majmin, fstype, source) = best.ok_or_else(|| format!("{dir}: no mount found"))?;
    if ["tmpfs", "ramfs", "devtmpfs", "overlay", "nfs", "nfs4", "cifs", "smb3", "9p", "virtiofs", "zfs", "squashfs", "fuse"].contains(&fstype.as_str()) {
        return Err(format!("{dir} is on {fstype}, not on a local disk"));
    }
    let mut mm = majmin.clone();
    if mm.starts_with("0:") {
        use std::os::unix::fs::MetadataExt;
        let rdev = std::fs::metadata(&source).map_err(|_| format!("{dir}: {fstype} on {source}, no block device"))?.rdev();
        let (a, b) = dev_split(rdev);
        if a == 0 { return Err(format!("{dir}: {fstype} on {source}, no block device")); }
        mm = format!("{a}:{b}");
    }
    let mut node = std::fs::canonicalize(format!("/sys/dev/block/{mm}")).map_err(|_| format!("{dir}: block device {mm} not in sysfs"))?;
    for _ in 0..8 {
        if node.join("partition").is_file() { node = node.parent().map(|p| p.to_path_buf()).unwrap_or(node); continue; }
        let slave = std::fs::read_dir(node.join("slaves")).ok().and_then(|d| d.flatten().map(|e| e.file_name()).min());
        match slave {
            Some(sl) => node = std::fs::canonicalize(std::path::Path::new("/sys/class/block").join(sl)).map_err(|e| e.to_string())?,
            None => break,
        }
    }
    let name = node.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    if crate::tune::block_devs().iter().any(|d| d.file_name().map_or(false, |n| n.to_string_lossy() == name)) { Ok(name) }
    else { Err(format!("{dir} is on {name}, which the Storage rows do not cover")) }
}

/// Busy CPU time of the whole machine (user, system, irq, softirq; µs) from /proc/stat:
/// block-layer completion work runs in interrupt context, outside any process's own time.
fn busy_cpu_us() -> Option<f64> {
    let st = std::fs::read_to_string("/proc/stat").ok()?;
    let f: Vec<u64> = st.lines().next()?.split_whitespace().skip(1).filter_map(|x| x.parse().ok()).collect();
    if f.len() < 7 { return None; }
    let hz = unsafe { libc::sysconf(libc::_SC_CLK_TCK) }.max(1) as f64;
    Some((f[0] + f[1] + f[2] + f[5] + f[6]) as f64 * 1e6 / hz)
}

/// 4 KiB O_DIRECT reads at random page offsets of `fd` (queue depth 1) until `max_n` reads,
/// `until` or `stop`: latency of each in µs.
fn rand_reads(fd: i32, pages: u64, max_n: usize, until: Instant, seed: u64, stop: Option<&AtomicBool>) -> Vec<f64> {
    let layout = std::alloc::Layout::from_size_align(4096, 4096).unwrap();
    let p = unsafe { std::alloc::alloc(layout) };
    if p.is_null() || pages == 0 { return Vec::new(); }
    let mut x = seed | 1;
    let mut lat = Vec::with_capacity(max_n.min(1 << 16));
    while lat.len() < max_n && Instant::now() < until && !stop.map_or(false, |s| s.load(Ordering::Relaxed)) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        let off = (x % pages) * 4096;
        let a = Instant::now();
        if unsafe { libc::pread(fd, p as *mut libc::c_void, 4096, off as libc::off_t) } != 4096 { break; }
        lat.push(a.elapsed().as_nanos() as f64 / 1000.0);
    }
    unsafe { std::alloc::dealloc(p, layout) };
    lat
}

/// Storage suite for the Storage rows and the dirty window: `size` bytes on `dir`'s disk.
///  * streaming buffered write + fsync, a 4 KiB fsync prober every 10 ms alongside
///    (WriteMbs; FsyncP99Ms = what a save waits for behind a big write);
///  * cold sequential read in 128 KiB reads (ReadMbs: read-ahead, merging);
///  * 4 KiB O_DIRECT random reads at queue depth 1 (RandReadP99Us);
///  * the same from several threads at once (IopsK; IoCpuUs = busy CPU per read, interrupt
///    time included: accounting, entropy hook, merge lookups, completion placement);
///  * random reads while a second writer streams and commits (MixedReadP99Us: asset loading
///    during a download or a shader-cache write - writeback throttling, scheduler, queue depth);
///  * random first touches of a cold mmap of the file (MmapFaultP99Us; RaFootprintMib = page
///    cache the fault read-around brought in, i.e. what read-ahead costs a program that maps
///    a big pack and touches it sparsely).
/// Every file is an unlinked O_TMPFILE: nothing is left behind.
pub fn io(dir: &str, size: usize, s: &mut Sample) -> Result<(), String> {
    let size = size.max(16 * MIB) / MIB * MIB;
    let mut f = tmpfile(dir, false).map_err(|e| format!("{dir}: {e}"))?;
    let stop = Arc::new(AtomicBool::new(false));
    let s2 = stop.clone();
    let d2 = dir.to_owned();
    let prober = std::thread::spawn(move || {
        let mut lat = Vec::new();
        let Ok(mut g) = tmpfile(&d2, false) else { return lat };
        let buf = [7u8; 4096];
        while !s2.load(Ordering::Relaxed) {
            let _ = g.seek(SeekFrom::Start(0));
            let a = Instant::now();
            if g.write_all(&buf).is_err() || g.sync_data().is_err() { break; }
            lat.push(a.elapsed().as_secs_f64() * 1000.0);
            std::thread::sleep(Duration::from_millis(10));
        }
        lat
    });
    let mut buf = vec![0u8; MIB];
    let mut x: u64 = 0x2545_F491_4F6C_DD1D;
    for b in buf.iter_mut() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; *b = x as u8; }
    let t0 = Instant::now();
    let mut res = Ok(());
    for _ in 0..size / MIB { if let Err(e) = f.write_all(&buf) { res = Err(format!("write: {e}")); break; } }
    if res.is_ok() { res = f.sync_all().map_err(|e| format!("fsync: {e}")); }
    let wsecs = t0.elapsed().as_secs_f64();
    stop.store(true, Ordering::Relaxed);
    let mut fl = prober.join().unwrap_or_default();
    res?;
    s.insert(Metric::WriteMbs, size as f64 / MIB as f64 / wsecs);
    if let Some(p) = tail(&mut fl) { s.insert(Metric::FsyncP99Ms, p); }
    // Cold read: clean pages dropped from the cache, 128 KiB reads so read-ahead matters.
    unsafe { libc::posix_fadvise(f.as_raw_fd(), 0, 0, libc::POSIX_FADV_DONTNEED); }
    let _ = f.seek(SeekFrom::Start(0));
    let mut rb = vec![0u8; 128 * 1024];
    let t1 = Instant::now();
    let mut got = 0usize;
    while got < size { match f.read(&mut rb) { Ok(0) | Err(_) => break, Ok(n) => got += n } }
    s.insert(Metric::ReadMbs, got as f64 / MIB as f64 / t1.elapsed().as_secs_f64());
    let pages = (size / 4096) as u64;
    // Direct reads through /proc/self/fd (the file has no name). Without O_DIRECT (some file
    // systems refuse it) only the buffered figures are taken.
    if let Ok(g) = std::fs::OpenOptions::new().read(true).custom_flags(libc::O_DIRECT).open(format!("/proc/self/fd/{}", f.as_raw_fd())) {
        let fd = g.as_raw_fd();
        let mut lat = rand_reads(fd, pages, 1500, Instant::now() + Duration::from_secs(2), 0x5EED_0101, None);
        if let Some(v) = tail(&mut lat) { s.insert(Metric::RandReadP99Us, v); }
        // Parallel: completion work, accounting and lock contention scale with the request rate.
        let threads = (std::thread::available_parallelism().map_or(4, |n| n.get()) / 2).clamp(2, 16);
        let (c0, t2) = (busy_cpu_us(), Instant::now());
        let until = t2 + Duration::from_millis(600);
        let hs: Vec<_> = (0..threads as u64).map(|i| std::thread::spawn(move || rand_reads(fd, pages, usize::MAX, until, 0x5EED_0200 + i, None).len())).collect();
        let n: usize = hs.into_iter().map(|h| h.join().unwrap_or(0)).sum();
        let (c1, secs) = (busy_cpu_us(), t2.elapsed().as_secs_f64());
        if n > 0 {
            s.insert(Metric::IopsK, n as f64 / secs / 1000.0);
            if let (Some(a), Some(b)) = (c0, c1) { if b > a { s.insert(Metric::IoCpuUs, (b - a) / n as f64); } }
        }
        // Mixed: a second writer streams 1 MiB writes and commits every 16 MiB (a download or
        // installer) while the reader keeps reading; only reads issued while it writes count.
        let wstop = Arc::new(AtomicBool::new(false));
        let written = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let (ws, wn, d3, wbuf) = (wstop.clone(), written.clone(), dir.to_owned(), buf.clone());
        let wmax = (size / 2).max(32 * MIB);
        let writer = std::thread::spawn(move || {
            let Ok(mut b) = tmpfile(&d3, false) else { ws.store(true, Ordering::Relaxed); return };
            let mut done = 0usize;
            while done < wmax && !ws.load(Ordering::Relaxed) {
                if b.write_all(&wbuf).is_err() { break; }
                done += MIB;
                wn.store(done, Ordering::Relaxed);
                if done % (16 * MIB) == 0 && b.sync_data().is_err() { break; }
            }
            let _ = b.sync_data();
            ws.store(true, Ordering::Relaxed);
        });
        let t3 = Instant::now();
        while written.load(Ordering::Relaxed) < 16 * MIB && !wstop.load(Ordering::Relaxed) && t3.elapsed() < Duration::from_millis(500) {
            std::thread::sleep(Duration::from_millis(1));
        }
        let mut mixed = rand_reads(fd, pages, 4000, Instant::now() + Duration::from_secs(3), 0x5EED_0300, Some(&wstop));
        wstop.store(true, Ordering::Relaxed);
        let _ = writer.join();
        if mixed.len() >= 20 { if let Some(v) = tail(&mut mixed) { s.insert(Metric::MixedReadP99Us, v); } }
    }
    // Cold mmap, sparse touches: each fault reads around it as far as read-ahead allows.
    unsafe { libc::posix_fadvise(f.as_raw_fd(), 0, 0, libc::POSIX_FADV_DONTNEED); }
    let p = unsafe { libc::mmap(std::ptr::null_mut(), size, libc::PROT_READ, libc::MAP_SHARED, f.as_raw_fd(), 0) };
    if p != libc::MAP_FAILED {
        let mut lat = Vec::with_capacity(256);
        let mut acc = 0u8;
        for _ in 0..256 {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            let off = (x % pages) as usize * 4096;
            let a = Instant::now();
            acc ^= unsafe { std::ptr::read_volatile((p as *const u8).add(off)) };
            lat.push(a.elapsed().as_nanos() as f64 / 1000.0);
        }
        std::hint::black_box(acc);
        if let Some(v) = tail(&mut lat) { s.insert(Metric::MmapFaultP99Us, v); }
        let mut vec = vec![0u8; size / 4096];
        if unsafe { libc::mincore(p, size, vec.as_mut_ptr()) } == 0 {
            s.insert(Metric::RaFootprintMib, vec.iter().filter(|b| **b & 1 != 0).count() as f64 * 4096.0 / MIB as f64);
        }
        unsafe { libc::munmap(p, size); }
    }
    Ok(())
}

/// Power source: battery discharge rate (whole machine) or the RAPL package
/// counter (CPU only; device power states are invisible there).
pub fn power_source() -> &'static str {
    if battery_uw().is_some() { "battery" } else if !Rapl::new().zones.is_empty() { "rapl" } else { "none" }
}

pub fn battery_uw() -> Option<f64> {
    for e in std::fs::read_dir("/sys/class/power_supply").ok()?.flatten() {
        let p = e.path();
        let rd = |n: &str| std::fs::read_to_string(p.join(n)).ok().map(|s| s.trim().to_owned());
        if rd("type").as_deref() != Some("Battery") || rd("status").as_deref() != Some("Discharging") { continue; }
        // Some firmware reports the discharge as a negative current/power.
        if let Some(w) = rd("power_now").and_then(|v| v.parse::<f64>().ok()) { return Some(w.abs()); }
        let (i, v) = (rd("current_now")?.parse::<f64>().ok()?, rd("voltage_now")?.parse::<f64>().ok()?);
        return Some((i * v / 1e6).abs());
    }
    None
}

/// RAPL package energy counters. Only package domains of the MSR interface: on Intel
/// `intel-rapl-mmio:0` is the same package again and `psys` (platform) already contains
/// it, so summing every top-level zone counted the CPU two or three times. The counters
/// wrap (AMD: every ~10 min at 100 W), which is corrected per zone.
struct Rapl { zones: Vec<(std::path::PathBuf, u64)> }

impl Rapl {
    fn new() -> Rapl { Rapl::from_dir(std::path::Path::new("/sys/class/powercap")) }
    fn from_dir(root: &std::path::Path) -> Rapl {
        let all: Vec<std::path::PathBuf> = std::fs::read_dir(root).into_iter().flatten().flatten().map(|e| e.path())
            .filter(|p| p.file_name().map_or(false, |n| n.to_string_lossy().matches(':').count() == 1) && p.join("energy_uj").exists()).collect();
        let name = |p: &std::path::Path| std::fs::read_to_string(p.join("name")).unwrap_or_default().trim().to_owned();
        let msr_pkg: Vec<std::path::PathBuf> = all.iter().filter(|p| name(p).starts_with("package")
            && p.file_name().map_or(false, |n| n.to_string_lossy().starts_with("intel-rapl:"))).cloned().collect();
        let pick = if !msr_pkg.is_empty() { msr_pkg } else { all.into_iter().filter(|p| name(p) != "psys").take(1).collect() };
        Rapl { zones: pick.into_iter().map(|p| {
            let range = std::fs::read_to_string(p.join("max_energy_range_uj")).ok().and_then(|s| s.trim().parse().ok()).unwrap_or(0);
            (p, range)
        }).collect() }
    }
    fn read(&self) -> Option<Vec<u64>> {
        self.zones.iter().map(|(p, _)| std::fs::read_to_string(p.join("energy_uj")).ok()?.trim().parse().ok()).collect()
    }
    /// Joules between two readings.
    fn joules(&self, a: &[u64], b: &[u64]) -> Option<f64> {
        let mut uj = 0u64;
        for ((x, y), (_, range)) in a.iter().zip(b).zip(&self.zones) {
            uj += if y >= x { y - x } else if *range > *x { range - x + y } else { return None };
        }
        Some(uj as f64 / 1e6)
    }
    /// Average watts while `f` runs, and its result.
    fn watts<T>(&self, f: impl FnOnce() -> T) -> (T, Option<f64>) {
        let (e0, t0) = (self.read(), Instant::now());
        let r = f();
        let (e1, secs) = (self.read(), t0.elapsed().as_secs_f64());
        let w = match (e0, e1) { (Some(a), Some(b)) if !self.zones.is_empty() && secs > 0.0 => self.joules(&a, &b).map(|j| j / secs), _ => None };
        (r, w.filter(|w| *w > 0.0))
    }
}

/// Average idle watts over `dur` (after the caller let things settle). `battery` is the
/// source chosen when the session started: a charger plugged in or pulled out mid-session
/// would otherwise switch between whole-machine and package power from one run to the next.
pub fn idle_w(dur: Duration, battery: bool) -> Option<f64> { idle_w_with(dur, battery, None) }

/// `idle_w` with the battery reading's measured update period (see `battery_cadence`).
pub fn idle_w_with(dur: Duration, battery: bool, period: Option<f64>) -> Option<f64> {
    if let (true, Some(p)) = (battery, period) { return idle_w_battery(dur, p); }
    if battery {
        let mut v = Vec::new();
        let t0 = Instant::now();
        while t0.elapsed() < dur {
            v.push(battery_uw()? / 1e6);
            std::thread::sleep(Duration::from_millis(250));
        }
        return (!v.is_empty()).then(|| v.iter().sum::<f64>() / v.len() as f64);
    }
    Rapl::new().watts(|| std::thread::sleep(dur)).1
}

// ── measurement conditions: die temperature, display, battery cadence ──────

/// The CPU's own temperature sensor: k10temp Tctl (AMD), zenpower Tdie/Tctl, coretemp's
/// package sensor (Intel); else the x86_pkg_temp thermal zone. Found once.
fn cpu_temp_path() -> Option<&'static std::path::PathBuf> {
    static P: std::sync::OnceLock<Option<std::path::PathBuf>> = std::sync::OnceLock::new();
    P.get_or_init(|| {
        let rd = |p: &std::path::Path| std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned());
        let mut best: Option<(u8, std::path::PathBuf)> = None;
        for e in std::fs::read_dir("/sys/class/hwmon").into_iter().flatten().flatten() {
            let d = e.path();
            let name = rd(&d.join("name")).unwrap_or_default();
            for i in 1..=16 {
                let input = d.join(format!("temp{i}_input"));
                if !input.is_file() { continue; }
                let label = rd(&d.join(format!("temp{i}_label"))).unwrap_or_default();
                let rank = match (name.as_str(), label.as_str()) {
                    ("k10temp", "Tctl") | ("k10temp", "") => 3,
                    ("zenpower", "Tdie") | ("zenpower", "Tctl") => 2,
                    ("coretemp", l) if l.starts_with("Package") => 3,
                    _ => 0,
                };
                if rank > 0 && best.as_ref().map_or(true, |b| rank > b.0) { best = Some((rank, input)); }
            }
        }
        best.map(|b| b.1).or_else(|| {
            std::fs::read_dir("/sys/class/thermal").into_iter().flatten().flatten().map(|e| e.path())
                .find(|z| rd(&z.join("type")).as_deref() == Some("x86_pkg_temp")).map(|z| z.join("temp"))
        })
    }).as_ref()
}

/// CPU temperature in °C (None without a sensor).
pub fn cpu_temp_c() -> Option<f64> {
    let v: f64 = std::fs::read_to_string(cpu_temp_path()?).ok()?.trim().parse().ok()?;
    Some(v / 1000.0)
}

/// Waits (at most `max`) until the die is no warmer than `target` °C, or has stopped cooling
/// (less than 0.15 °C/s over the last second: a loaded phase sits at its own equilibrium, and
/// waiting longer only wastes the budget). Returns the temperature then and the seconds waited.
pub fn thermal_gate(target: f64, max: Duration) -> (Option<f64>, f64) {
    let t0 = Instant::now();
    let mut hist: std::collections::VecDeque<(f64, f64)> = std::collections::VecDeque::new();
    loop {
        let Some(t) = cpu_temp_c() else { return (None, t0.elapsed().as_secs_f64()) };
        let now = t0.elapsed().as_secs_f64();
        hist.push_back((now, t));
        while hist.front().map_or(false, |(a, _)| now - a > 1.05) { hist.pop_front(); }
        let cooling = match (hist.front(), hist.len() >= 4) { (Some((a, ta)), true) if now - a >= 0.9 => (ta - t) / (now - a) >= 0.15, _ => true };
        if t <= target || !cooling || t0.elapsed() >= max { return (Some(t), now); }
        std::thread::sleep(Duration::from_millis(250));
    }
}

/// Keeps every CPU busy for `dur` (a heat soak before a "hot" run).
pub fn preheat(dur: Duration) {
    let threads = std::thread::available_parallelism().map_or(1, |n| n.get());
    let hs: Vec<_> = (0..threads).map(|i| std::thread::spawn(move || { std::hint::black_box(spin_for(dur, 77 + i as u64)); })).collect();
    for h in hs { let _ = h.join(); }
}

/// What the display is doing: backlight level of every panel and the DPMS state of every
/// internal connector. The power of a run depends on it (a blanked panel saves watts, and on AC
/// the iGPU's display engine shows in the package power), so a run measured under another
/// display state than its session's reference runs is noted and counts less.
pub fn display_state() -> String {
    let rd = |p: std::path::PathBuf| std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned());
    let mut parts: Vec<String> = Vec::new();
    for e in std::fs::read_dir("/sys/class/backlight").into_iter().flatten().flatten() {
        if let Some(b) = rd(e.path().join("actual_brightness")) { parts.push(format!("{}={b}", e.file_name().to_string_lossy())); }
    }
    for e in std::fs::read_dir("/sys/class/drm").into_iter().flatten().flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        if !(n.contains("-eDP-") || n.contains("-LVDS-") || n.contains("-DSI-")) { continue; }
        if let Some(d) = rd(e.path().join("dpms")) { parts.push(format!("{n}:{d}")); }
    }
    parts.sort();
    parts.join(",")
}

/// One battery reading: (seconds since `t0`, watts discharged).
pub type BatSample = (f64, f64);

/// Update period of the firmware's discharge reading (s): the median spacing of its changes. The
/// EC refreshes `power_now` far more slowly than anyone reads it (0.5-5 s), so a 2 s window
/// polled every 250 ms held one or two real readings - one of them often from the run before.
/// None = no change seen (a flat reading: the caller assumes a slow cadence).
pub fn cadence_of(samples: &[BatSample]) -> Option<f64> {
    let mut changes: Vec<f64> = Vec::new();
    for w in samples.windows(2) { if (w[1].1 - w[0].1).abs() > 1e-6 { changes.push(w[1].0); } }
    if changes.len() < 3 { return None; }
    let mut gaps: Vec<f64> = changes.windows(2).map(|w| w[1] - w[0]).collect();
    crate::calib::median(&mut gaps).map(|g| g.clamp(0.1, 15.0))
}

/// Mean of the readings that arrived as fresh updates, skipping the first `skip` updates (they
/// may straddle the knob change). None without at least one counted update.
pub fn mean_of_updates(samples: &[BatSample], skip: usize) -> Option<f64> {
    let mut ups: Vec<f64> = Vec::new();
    for w in samples.windows(2) { if (w[1].1 - w[0].1).abs() > 1e-6 { ups.push(w[1].1); } }
    let used: Vec<f64> = ups.into_iter().skip(skip).collect();
    if used.is_empty() { return None; }
    Some(used.iter().sum::<f64>() / used.len() as f64)
}

/// Measures the battery reading's update period (s) on a quiet machine, sampling for at most `max`.
pub fn battery_cadence(max: Duration) -> Option<f64> {
    let t0 = Instant::now();
    let mut v: Vec<BatSample> = Vec::new();
    while t0.elapsed() < max {
        v.push((t0.elapsed().as_secs_f64(), battery_uw()? / 1e6));
        if let Some(c) = cadence_of(&v) { if v.len() > 8 && t0.elapsed().as_secs_f64() > 4.0 * c + 0.5 { return Some(c); } }
        std::thread::sleep(Duration::from_millis(50));
    }
    cadence_of(&v)
}

/// Idle watts on battery: waits for fresh updates of the reading (the first one is dropped: it
/// may average over the knob change) and averages at least three of them, and at least `dur`.
pub fn idle_w_battery(dur: Duration, period: f64) -> Option<f64> {
    let t0 = Instant::now();
    let need = Duration::from_secs_f64((4.2 * period).max(dur.as_secs_f64()) + 0.3);
    let cap = need + Duration::from_secs_f64(3.0 * period + 1.0);
    let mut v: Vec<BatSample> = Vec::new();
    let poll = Duration::from_millis(((period * 1000.0 / 8.0) as u64).clamp(40, 250));
    loop {
        v.push((t0.elapsed().as_secs_f64(), battery_uw()? / 1e6));
        let ups = v.windows(2).filter(|w| (w[1].1 - w[0].1).abs() > 1e-6).count();
        if (t0.elapsed() >= need && ups >= 4) || t0.elapsed() >= cap { break; }
        std::thread::sleep(poll);
    }
    // A reading that never changed (EC holds it, or a perfectly flat load): its plain mean.
    mean_of_updates(&v, 1).or_else(|| (!v.is_empty()).then(|| v.iter().map(|x| x.1).sum::<f64>() / v.len() as f64))
}

/// Root: drop clean caches and compact memory so one run does not inherit
/// the previous run's page cache or fragmentation.
pub fn settle() {
    unsafe { libc::sync(); }
    let _ = std::fs::write("/proc/sys/vm/drop_caches", "3");
    let _ = std::fs::write("/proc/sys/vm/compact_memory", "1");
    std::thread::sleep(Duration::from_millis(500));
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cpu_benches() {
        assert!(cpu_single(Duration::from_millis(100)) > 0.0);
        assert!(cpu_multi(Duration::from_millis(100)).0 > 0.0);
        assert!(pingpong_p99_us(200).unwrap() > 0.0);
    }

    #[test]
    fn io_and_wake_produce_metrics() {
        let mut s = Sample::new();
        io("/tmp", 16 << 20, &mut s).unwrap();
        assert!(s[&Metric::WriteMbs] > 0.0 && s[&Metric::ReadMbs] > 0.0);
        assert!(s.get(&Metric::RaFootprintMib).map_or(false, |m| *m > 0.0) && s.contains_key(&Metric::MmapFaultP99Us));
        assert!(wake_p99_us(Duration::from_millis(200)).is_some());
        assert!(frame_tail_us(Duration::from_millis(120)).map_or(false, |v| v > 0.0));
        let (t, m) = frame_us(Duration::from_millis(120)).unwrap();
        assert!(m > 0.0 && t >= m, "the tail is never below the median");
        assert_eq!(p99(&mut vec![]), None);
        assert_eq!(tail(&mut vec![]), None);
        // Worst 1 %, at least 5: 1000 samples -> the 10 largest; 8 samples -> the 5 largest.
        let mut v: Vec<f64> = (1..=1000).map(|x| x as f64).collect();
        assert!((tail(&mut v).unwrap() - 995.5).abs() < 1e-9);
        let mut w: Vec<f64> = (1..=8).map(|x| x as f64).collect();
        assert!((tail(&mut w).unwrap() - 6.0).abs() < 1e-9);
    }

    #[test]
    fn disk_of_refuses_ram_file_systems() {
        // /dev/shm is tmpfs on every Linux system that has it.
        if std::path::Path::new("/dev/shm").is_dir() { assert!(disk_of("/dev/shm").is_err()); }
        assert_eq!(dev_split((259u64 << 8) | 3), (259, 3));
        assert_eq!(dev_split(((300u64 & 0xfff) << 8) | ((1000u64 & 0xff) | ((1000u64 & !0xff) << 12))), (300, 1000));
    }

    #[test]
    fn rapl_counts_the_package_once_and_survives_wrap() {
        let root = std::env::temp_dir().join(format!("centurion-rapl-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let zone = |dir: &str, name: &str| {
            let d = root.join(dir);
            std::fs::create_dir_all(&d).unwrap();
            std::fs::write(d.join("name"), format!("{name}\n")).unwrap();
            std::fs::write(d.join("energy_uj"), "1000\n").unwrap();
            std::fs::write(d.join("max_energy_range_uj"), "65532610987\n").unwrap();
        };
        // Intel laptop layout: MSR package, its core subzone, platform (psys), and the same package over MMIO.
        zone("intel-rapl:0", "package-0"); zone("intel-rapl:0:0", "core"); zone("intel-rapl:1", "psys"); zone("intel-rapl-mmio:0", "package-0");
        let r = Rapl::from_dir(&root);
        assert_eq!(r.zones.iter().map(|z| z.0.file_name().unwrap().to_string_lossy().into_owned()).collect::<Vec<_>>(), vec!["intel-rapl:0"]);
        // The counter wrapped between the two readings.
        assert_eq!(r.joules(&[65_532_000_000], &[1_389_013]), Some(2.0));
        assert_eq!(r.joules(&[5_000_000], &[7_000_000]), Some(2.0));
        // Only an MMIO zone: it is used; psys alone is never taken as package power.
        let _ = std::fs::remove_dir_all(&root);
        zone("intel-rapl-mmio:0", "package-0"); zone("intel-rapl:1", "psys");
        assert_eq!(Rapl::from_dir(&root).zones.len(), 1);
        assert!(Rapl::from_dir(&root).zones[0].0.ends_with("intel-rapl-mmio:0"));
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn game_loop_contention_and_jobs_produce_metrics() {
        assert!(unit() >= 50_000 && unit() == unit(), "measured once");
        let g = game_loop(Duration::from_millis(240)).expect("frames");
        let (med, tl) = (g.med_ms, g.tail_ms);
        assert!(g.jitter_us >= 1.0 && g.jitter_us.is_finite());
        assert!(med > 0.0 && tl >= med);
        let c = contended(Duration::from_millis(120));
        assert!(c.rate > 0.0 && c.frame_tail.map_or(false, |v| v > 0.0));
        // The test binary is not centurion-calibrate: a child that fails to run the job yields None, never a figure.
        assert_eq!(jobs_per_sec(std::path::Path::new("/nonexistent/centurion-calibrate"), Duration::from_millis(50)), None);
        job_main(10_000);
    }

    #[test]
    fn cache_loop_walks_one_cycle_and_produces_frames() {
        let c = Chain::new(1 << 20, 7).expect("chain");
        // One cycle over every line: a full lap returns to the start, no shorter walk does.
        assert_eq!(c.walk(5, c.lines as u64), 5);
        assert_ne!(c.walk(5, c.lines as u64 / 2), 5);
        assert!((8 * MIB..=64 * MIB).contains(&cache_set_bytes()) && cache_set_bytes() % MIB == 0);
        assert!(cache_unit() >= 1000 && cache_unit() == cache_unit(), "measured once");
        let (med, tl) = cache_loop(Duration::from_millis(300)).expect("frames");
        assert!(med > 0.0 && tl >= med);
        assert_eq!(pacing_us(&[vec![1.0, 1.5, 1.0], vec![2.0]]), Some(500.0));
    }

    #[test]
    fn battery_cadence_and_fresh_updates() {
        // EC updates every 2 s, polled every 0.25 s: changes at 2, 4, 6, 8.
        let s: Vec<BatSample> = (0..40).map(|i| { let t = i as f64 * 0.25; (t, 10.0 + (t / 2.0).floor()) }).collect();
        assert!((cadence_of(&s).unwrap() - 2.0).abs() < 1e-9);
        // Updates carry 11, 12, 13, 14; the first one (may straddle the change) is skipped.
        assert!((mean_of_updates(&s, 1).unwrap() - 13.0).abs() < 1e-9);
        assert!(cadence_of(&s[..10]).is_none(), "one change is no cadence");
        assert_eq!(mean_of_updates(&[(0.0, 5.0), (1.0, 5.0)], 1), None);
        assert!(display_state().len() < 4096);
        if let Some(t) = cpu_temp_c() { assert!((-20.0..150.0).contains(&t)); }
    }

    #[test]
    fn sub_runs_pool_their_samples() {
        assert!(cpu_single(Duration::from_millis(40)) > 0.0);
        let (rate, _) = cpu_multi(Duration::from_millis(40));
        assert!(rate > 0.0);
        assert!(pingpong_p99_us(200).is_some(), "4 bursts of 58 rounds, warm-up dropped");
    }
}

// ── load phase ─────────────────────────────────────────────────────────────

pub fn meminfo_kb(field: &str) -> Option<u64> {
    std::fs::read_to_string("/proc/meminfo").ok()?.lines()
        .find_map(|l| l.strip_prefix(field)?.trim().trim_end_matches("kB").trim().parse().ok())
}

pub fn vmstat(field: &str) -> u64 {
    std::fs::read_to_string("/proc/vmstat").unwrap_or_default().lines()
        .filter(|l| l.starts_with(field)).filter_map(|l| l.split_whitespace().nth(1)?.parse::<u64>().ok()).sum()
}

/// The ballast child (`centurion-calibrate __ballast <MiB> <threads> <floor MiB> [perforate]`):
/// fills anonymous memory in 64 MiB blocks until it holds <MiB> or MemAvailable falls to
/// <floor>, prints "ready <MiB held>", then keeps re-faulting one block after another
/// (continuous reclaim / compaction / fault work) and runs <threads> integer spinners.
/// `perforate` = 1: every 4th block is small pages with every other page freed again - free
/// memory with no free 2 MiB block in it, re-made now and then because compaction heals it:
/// what huge-page allocation (THP defrag, khugepaged, compaction knobs) meets on a machine
/// that has been up for days. It is the OOM killer's first choice (oom_score_adj 1000) and
/// dies with its parent.
pub fn ballast_main(target_mib: usize, threads: usize, floor_mib: u64, perforate: bool) -> ! {
    unsafe { libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL); }
    let _ = std::fs::write("/proc/self/oom_score_adj", "1000");
    const BLOCK: usize = 64 * MIB;
    let holey = |k: usize| perforate && k % 4 == 3;
    let fill = |k: usize| -> Option<&'static mut [u8]> {
        let b = anon(BLOCK)?;
        if holey(k) { unsafe { libc::madvise(b.as_mut_ptr() as *mut libc::c_void, BLOCK, libc::MADV_NOHUGEPAGE); } }
        for i in (0..BLOCK).step_by(4096) { b[i] = 1; }
        if holey(k) {
            for i in (0..BLOCK).step_by(8192) { unsafe { libc::madvise(b.as_mut_ptr().add(i) as *mut libc::c_void, 4096, libc::MADV_DONTNEED); } }
        }
        Some(b)
    };
    let mut blocks: Vec<&'static mut [u8]> = Vec::new();
    while blocks.len() * 64 < target_mib {
        if meminfo_kb("MemAvailable:").map_or(true, |kb| kb / 1024 <= floor_mib) { break; }
        let Some(b) = fill(blocks.len()) else { break };
        blocks.push(b);
    }
    let held: usize = (0..blocks.len()).map(|k| if holey(k) { 32 } else { 64 }).sum();
    println!("ready {held}");
    let _ = std::io::stdout().flush();
    for t in 0..threads {
        std::thread::spawn(move || {
            let mut x = 0x9E37_79B9u64 ^ t as u64;
            loop { for _ in 0..1_000_000 { x ^= x << 13; x ^= x >> 7; x ^= x << 17; } std::hint::black_box(x); }
        });
    }
    let (mut i, mut h) = (0usize, 0usize);
    loop {
        let n = blocks.len();
        if n > 0 {
            // Plain blocks churn every tick; a perforated one is re-made every 8th.
            let k = if perforate && i % 8 == 7 && n >= 4 { h += 1; ((h - 1) % (n / 4)) * 4 + 3 }
                    else { let mut k = i % n; while holey(k) && n > 1 { i += 1; k = i % n; } k };
            unmap(blocks[k]);
            match fill(k) { Some(b) => blocks[k] = b, None => { blocks.swap_remove(k); } }
            i += 1;
        }
        std::thread::sleep(Duration::from_millis(40));
    }
}

/// CPU package watts over a closure's run (RAPL), and its result.
pub fn with_pkg_power<T>(f: impl FnOnce() -> T) -> (T, Option<f64>) { Rapl::new().watts(f) }

// ── CPU / scheduler ───────────────────────────────────────────────────────────

fn spin(iters: u64, seed: u64) -> u64 {
    let mut x = seed | 1;
    for _ in 0..iters { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
    x
}

/// Work of one thread for `dur`: (iterations, seconds actually spent).
fn spin_for(dur: Duration, seed: u64) -> (u64, f64) {
    let (t0, mut n) = (Instant::now(), 0u64);
    while t0.elapsed() < dur { std::hint::black_box(spin(200_000, n ^ seed)); n += 200_000; }
    (n, t0.elapsed().as_secs_f64())
}

/// Integer work per second on one thread: 4 fresh threads one after another, so the
/// result is what a new thread typically gets (placement by the scheduler's core
/// ranking) rather than wherever the calibrator's own thread happened to sit.
pub fn cpu_single(dur: Duration) -> f64 {
    let (mut n, mut secs) = (0u64, 0.0);
    for i in 0..4u64 {
        let (a, b) = std::thread::spawn(move || spin_for(dur / 4, i)).join().unwrap_or((0, 0.0));
        n += a; secs += b;
    }
    if secs > 0.0 { n as f64 / secs } else { 0.0 }
}

/// Integer work per second on every logical CPU, and work per joule (RAPL).
pub fn cpu_multi(dur: Duration) -> (f64, Option<f64>) {
    let threads = std::thread::available_parallelism().map_or(1, |n| n.get());
    let (total, w) = with_pkg_power(|| {
        // Each thread's own rate: threads start a little apart and overrun `dur` by up to one chunk.
        let hs: Vec<_> = (0..threads).map(|i| std::thread::spawn(move || {
            let (n, secs) = spin_for(dur, i as u64);
            if secs > 0.0 { n as f64 / secs } else { 0.0 }
        })).collect();
        hs.into_iter().map(|h| h.join().unwrap_or(0.0)).sum::<f64>()
    });
    let rate = total;
    (rate, w.filter(|w| *w > 0.0).map(|w| rate / w))
}

/// Round trip p99 (µs) of two threads waking each other through pipes:
/// scheduler wake-up + context-switch latency. 4 bursts, each with a fresh pair:
/// whether a pair lands on one core, two cores of a CCD or two CCDs changes the
/// latency several-fold, and one placement must not decide the whole run. The
/// first rounds of a burst (threads starting up) are not counted.
pub fn pingpong_p99_us(rounds: usize) -> Option<f64> {
    let mut lat = Vec::with_capacity(rounds);
    for _ in 0..4 {
        let burst = std::thread::spawn(move || pingpong_burst(rounds / 4 + 8)).join().ok().flatten()?;
        lat.extend(burst.into_iter().skip(8));
    }
    tail(&mut lat)
}

fn pingpong_burst(rounds: usize) -> Option<Vec<f64>> {
    let mut a = [0i32; 2];
    let mut b = [0i32; 2];
    unsafe { if libc::pipe(a.as_mut_ptr()) != 0 || libc::pipe(b.as_mut_ptr()) != 0 { return None; } }
    let (ar, aw, br, bw) = (a[0], a[1], b[0], b[1]);
    let echo = std::thread::spawn(move || {
        let mut c = [0u8; 1];
        for _ in 0..rounds {
            if unsafe { libc::read(ar, c.as_mut_ptr() as *mut libc::c_void, 1) } != 1 { break; }
            if unsafe { libc::write(bw, c.as_ptr() as *const libc::c_void, 1) } != 1 { break; }
        }
    });
    let mut lat = Vec::with_capacity(rounds);
    let c = [1u8; 1];
    let mut r = [0u8; 1];
    for _ in 0..rounds {
        let t = Instant::now();
        if unsafe { libc::write(aw, c.as_ptr() as *const libc::c_void, 1) } != 1 { break; }
        if unsafe { libc::read(br, r.as_mut_ptr() as *mut libc::c_void, 1) } != 1 { break; }
        lat.push(t.elapsed().as_nanos() as f64 / 1000.0);
    }
    let _ = echo.join();
    unsafe { for fd in [ar, aw, br, bw] { libc::close(fd); } }
    Some(lat)
}

fn mono_ns() -> i64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts); }
    ts.tv_sec as i64 * 1_000_000_000 + ts.tv_nsec as i64
}

/// Work of one frame: fixed (never scaled to the clock), so a core that is still ramping up
/// takes visibly longer. About 0.3 ms at 5 GHz.
const FRAME_WORK: u64 = 250_000;
const FRAME_NS: i64 = 4_000_000;

fn sleep_until(ns: i64) {
    let t = libc::timespec { tv_sec: (ns / 1_000_000_000) as libc::time_t, tv_nsec: (ns % 1_000_000_000) as libc::c_long };
    unsafe { libc::clock_nanosleep(libc::CLOCK_MONOTONIC, libc::TIMER_ABSTIME, &t, std::ptr::null_mut()); }
}

/// Fixed work over a working set: `iters` of integer work, then one pass over `buf`.
fn work(iters: u64, seed: u64, buf: &[u8]) -> u64 {
    let mut acc = spin(iters, seed);
    for j in (0..buf.len()).step_by(64) { acc = acc.wrapping_add(unsafe { std::ptr::read_volatile(buf.as_ptr().add(j)) } as u64); }
    acc
}

/// One thread's 240 Hz frame loop for `dur`: deadline -> work done, µs per frame. A missed
/// deadline is recorded and the loop re-aligns to the next period.
fn frame_part(dur: Duration, seed: u64) -> Vec<f64> {
    let buf = vec![1u8; 256 * 1024];
    let mut v = Vec::with_capacity(256);
    let mut next = mono_ns() + FRAME_NS;
    let end = next + dur.as_nanos() as i64;
    let mut acc = seed;
    while next < end {
        sleep_until(next);
        acc = work(FRAME_WORK, acc, &buf);
        let done = mono_ns();
        v.push((done - next).max(0) as f64 / 1000.0);
        next += FRAME_NS;
        if done >= next { next += ((done - next) / FRAME_NS + 1) * FRAME_NS; }
    }
    std::hint::black_box(acc);
    v
}

/// Burst response (µs): a thread wakes every 4 ms (240 Hz) on an absolute deadline, runs a
/// fixed piece of work over a 256 KiB working set and records deadline -> work done. That is
/// what a compositor or a light game thread meets: timer wake-up, C-state exit and how fast
/// the clock comes up for a short burst after idling (EPP, boost, idle governor, wake-up
/// latency QoS). Returns (tail, median): the tail is the stutter, the median the typical
/// response - it moves with the clock a mostly idle core is given, at a fraction of the
/// tail's noise. Three fresh threads one after another, so one placement does not decide
/// the run.
pub fn frame_us(dur: Duration) -> Option<(f64, f64)> {
    let mut all = Vec::with_capacity(512);
    for i in 0..3u64 {
        all.extend(std::thread::spawn(move || frame_part(dur / 3, i)).join().unwrap_or_default());
    }
    let med = crate::calib::median(&mut all.clone())?;
    Some((tail(&mut all)?, med))
}

pub fn frame_tail_us(dur: Duration) -> Option<f64> { frame_us(dur).map(|x| x.0) }

static UNIT: std::sync::OnceLock<u64> = std::sync::OnceLock::new();

/// `spin` iterations one core does per millisecond at its full clock: the best of 12 short
/// windows after a warm-up, measured once per process. centurion-calibrate calls it before it
/// touches any knob, so the game loop's and the jobs' work is the same in every run of a
/// session (work that scaled with the clock would hide exactly what EPP and boost change).
pub fn unit() -> u64 {
    *UNIT.get_or_init(|| {
        std::hint::black_box(spin_for(Duration::from_millis(60), 1));
        let mut best = 0.0f64;
        for i in 0..12u64 {
            let (n, secs) = spin_for(Duration::from_millis(8), i);
            if secs > 0.0 { best = best.max(n as f64 / secs); }
        }
        ((best / 1000.0) as u64).max(50_000)
    })
}

const GAME_NS: i64 = 8_000_000;
const GAME_WORKERS: usize = 3;

/// Frame gate of the game loop: (frame number, workers finished, stop).
struct Gate { m: std::sync::Mutex<(u64, usize, bool)>, go: std::sync::Condvar, done: std::sync::Condvar }

/// One thread set running gated 125 Hz frames for `dur`: on every deadline the calling thread
/// opens the gate, does `main()`, waits until each of the GAME_WORKERS job threads has done
/// its `job()` once, and records deadline -> all done (ms). The first `skip` frames start the
/// threads up (and fill their caches) and are not counted; a missed deadline is recorded and
/// the loop re-aligns to the next period.
fn gated_frames<M: FnMut(), J: FnMut() + Send + 'static>(dur: Duration, skip: u64, mut main: M, mk_job: impl Fn(usize) -> J) -> Vec<f64> {
    let gate = Arc::new(Gate { m: std::sync::Mutex::new((0, 0, false)), go: std::sync::Condvar::new(), done: std::sync::Condvar::new() });
    let workers: Vec<_> = (0..GAME_WORKERS).map(|w| {
        let g = gate.clone();
        let mut job = mk_job(w);
        std::thread::spawn(move || {
            let mut seen = 0u64;
            loop {
                {
                    let mut st = g.m.lock().unwrap();
                    while st.0 == seen && !st.2 { st = g.go.wait(st).unwrap(); }
                    if st.2 { break; }
                    seen = st.0;
                }
                job();
                let mut st = g.m.lock().unwrap();
                st.1 += 1;
                g.done.notify_one();
            }
        })
    }).collect();
    let mut v = Vec::with_capacity(64);
    let mut next = mono_ns() + GAME_NS;
    let end = next + dur.as_nanos() as i64;
    let mut frame = 0u64;
    while next < end {
        sleep_until(next);
        frame += 1;
        { let mut st = gate.m.lock().unwrap(); st.0 = frame; st.1 = 0; gate.go.notify_all(); }
        main();
        { let mut st = gate.m.lock().unwrap(); while st.1 < GAME_WORKERS { st = gate.done.wait(st).unwrap(); } }
        let done = mono_ns();
        if frame > skip { v.push((done - next).max(0) as f64 / 1e6); }
        next += GAME_NS;
        if done >= next { next += ((done - next) / GAME_NS + 1) * GAME_NS; }
    }
    { let mut st = gate.m.lock().unwrap(); st.2 = true; gate.go.notify_all(); }
    for h in workers { let _ = h.join(); }
    v
}

fn game_part(dur: Duration, seed: u64, unit: u64) -> Vec<f64> {
    let buf = vec![1u8; 1024 * 1024];
    let mut acc = seed;
    let v = gated_frames(dur, 2, || { acc = work(unit * 48 / 10, acc, &buf); }, |w| {  // 4.8 ms at full clock: the 60 % busy main thread
        // The job's working set is first touched by its own thread.
        let (mut buf, mut acc): (Option<Vec<u8>>, u64) = (None, seed ^ (w as u64 + 1));
        move || {
            let b = buf.get_or_insert_with(|| vec![1u8; 256 * 1024]);
            acc = work(unit * 24 / 10, acc, b);  // 2.4 ms at full clock: a 30 % busy job thread
            std::hint::black_box(acc);
        }
    });
    std::hint::black_box(acc);
    v
}

/// What `game_loop` measured: typical frame time and its tail (ms), frame pacing (µs) and the
/// CPU package power the loop drew (RAPL; None without it).
pub struct Game { pub med_ms: f64, pub tail_ms: f64, pub jitter_us: f64, pub watts: Option<f64> }

/// Frame pacing (µs): the mean change of the frame time from one frame to the next, inside each
/// thread set. The tail is the mean of the five worst frames of a window; this figure uses every
/// frame, so it carries the same stutter at a fraction of the noise.
fn pacing_us(parts: &[Vec<f64>]) -> Option<f64> {
    let (mut s, mut n) = (0.0f64, 0usize);
    for p in parts { for w in p.windows(2) { s += (w[1] - w[0]).abs(); n += 1; } }
    (n > 0).then(|| (s / n as f64 * 1000.0).max(1.0))
}

/// Game loop: a 125 Hz frame whose main thread is about 60 % busy (4.8 ms of fixed work
/// over a 1 MiB working set) and hands a 2.4 ms job to each of three worker threads at the
/// start of every frame; the frame ends when the main thread and all jobs are done. That is
/// the load a CPU-bound game puts on the machine and the one the light frame loop (7 % busy)
/// and the sustained spins (100 % busy, one thread or all) both miss: cores that are busy but
/// never saturated, where EPP, per-core EPP boost (it acts on cores more than half busy), core
/// boost, idle states and wake-up placement of the job threads decide the frame time. The
/// work per frame is fixed, so the package power over the loop is the cost of those frames:
/// a setting that buys the same frames with fewer watts shows up there. Three fresh thread
/// sets one after another.
pub fn game_loop(dur: Duration) -> Option<Game> {
    let unit = unit();
    let (parts, watts) = with_pkg_power(|| (0..3u64).map(|i| {
        std::thread::spawn(move || game_part(dur / 3, 0x6A3E + i, unit)).join().unwrap_or_default()
    }).collect::<Vec<Vec<f64>>>());
    let jitter_us = pacing_us(&parts)?;
    let mut all = parts.concat();
    let med_ms = crate::calib::median(&mut all.clone())?;
    Some(Game { med_ms, tail_ms: tail(&mut all)?, jitter_us, watts })
}

// ── cache-bound game loop ─────────────────────────────────────────────────────

/// A working set walked by dependent loads: one 4-byte link in every 64-byte line, all lines on
/// one random cycle - a prefetcher cannot follow it, and every line is visited before any is
/// visited again, so the set's whole size has to stay cached. Read-only once built: threads on
/// one L3 share its lines, threads on another L3 need their own copy of them.
struct Chain { p: *mut u8, len: usize, lines: u32 }
unsafe impl Send for Chain {}
unsafe impl Sync for Chain {}
impl Drop for Chain { fn drop(&mut self) { unsafe { libc::munmap(self.p as *mut libc::c_void, self.len); } } }

impl Chain {
    fn new(bytes: usize, seed: u64) -> Option<Chain> {
        let n = bytes / 64;
        if n < 1024 || n > u32::MAX as usize { return None; }
        let m = anon(bytes)?;
        let p = m.as_mut_ptr();
        let mut x = seed | 1;
        let mut rnd = || { x ^= x << 13; x ^= x >> 7; x ^= x << 17; x };
        let mut perm: Vec<u32> = (0..n as u32).collect();
        for i in (1..n).rev() { let j = (rnd() % i as u64) as usize; perm.swap(i, j); }
        for i in 0..n { unsafe { (p.add(perm[i] as usize * 64) as *mut u32).write(perm[(i + 1) % n]); } }
        Some(Chain { p, len: bytes, lines: n as u32 })
    }
    #[inline]
    fn walk(&self, mut at: u32, steps: u64) -> u32 {
        for _ in 0..steps { at = unsafe { std::ptr::read_volatile(self.p.add(at as usize * 64) as *const u32) }; }
        at
    }
}

/// Working set of the cache-bound loop: half the largest L3 of this machine (8..64 MiB). On a
/// V-Cache part (96 + 32 MiB) that is 48 MiB: inside the V-Cache die's L3, more than the
/// frequency die's; with equal L3s it fits either, and a thread on the other one still has to
/// fetch everything again.
pub fn cache_set_bytes() -> usize {
    let l3 = crate::tune::ccx_groups().iter().map(|g| g.l3_kib).max().unwrap_or(0) as usize * 1024;
    (if l3 == 0 { 16 * MIB } else { l3 / 2 }).clamp(8 * MIB, 64 * MIB) / MIB * MIB
}

fn pin(cpus: &[usize]) {
    if cpus.is_empty() { return; }
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        for &c in cpus { if c < libc::CPU_SETSIZE as usize { libc::CPU_SET(c, &mut set); } }
        libc::sched_setaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &set);
    }
}

static CACHE_UNIT: std::sync::OnceLock<u64> = std::sync::OnceLock::new();

/// Dependent loads per millisecond over the cache set where it fits best: one thread pinned to
/// each L3 domain in turn walks a full lap (the set is then in that domain's caches) and times
/// six further stretches; the best stretch of the best domain counts. Measured once per
/// process - centurion-calibrate calls it before it touches any knob - so the cache-bound loop's
/// work per frame is the same in every run of a session. 0 = no memory for the set.
pub fn cache_unit() -> u64 {
    *CACHE_UNIT.get_or_init(|| {
        let Some(chain) = Chain::new(cache_set_bytes(), 0xC0FFEE).map(Arc::new) else { return 0 };
        let mut domains: Vec<Vec<usize>> = crate::tune::ccx_groups().into_iter().map(|g| g.cpus).collect();
        if domains.is_empty() { domains.push(Vec::new()); }
        let mut best = 0.0f64;
        for cpus in domains {
            let c = chain.clone();
            let r = std::thread::spawn(move || {
                pin(&cpus);
                let n = c.lines as u64;
                let mut at = c.walk(0, n);
                let mut best = 0.0f64;
                for _ in 0..6 {
                    let t = Instant::now();
                    at = c.walk(at, n / 8);
                    let secs = t.elapsed().as_secs_f64();
                    if secs > 0.0 { best = best.max((n / 8) as f64 / secs); }
                }
                std::hint::black_box(at);
                best
            }).join().unwrap_or(0.0);
            best = best.max(r);
        }
        ((best / 1000.0) as u64).max(1000)
    })
}

fn cache_part(dur: Duration, seed: u64, unit: u64, chain: Arc<Chain>) -> Vec<f64> {
    let n = chain.lines as u64;
    // Four walkers a quarter lap apart (another offset per thread set).
    let start = |k: u64| ((n / 4 * k + n / 16 * seed) % n) as u32;
    let mut at = start(0);
    let c = chain.clone();
    let v = gated_frames(dur, 3, || { at = c.walk(at, unit * 24 / 10); }, |w| {  // 2.4 ms where the set is cached
        let (c, mut at) = (chain.clone(), start(w as u64 + 1));
        move || { at = c.walk(at, unit * 12 / 10); std::hint::black_box(at); }  // 1.2 ms
    });
    std::hint::black_box(at);
    v
}

/// Cache-bound game loop (ms): the game loop's frame - 125 Hz, a main thread and three job
/// threads - but the work is dependent loads over one shared set of half the largest L3
/// (`cache_set_bytes`), the fixed count sized where the set is cached (`cache_unit`: 2.4 ms for
/// the main thread, 1.2 ms per job). The game loop and every other CPU probe work inside L2,
/// so they could not tell a 96 MiB V-Cache die from a 32 MiB frequency die except by its
/// clock, and a spin loop always prefers the clock. Here the frame time is what a game's
/// simulation or draw-call thread pays for where its threads sit: all four on the L3 that
/// holds the set, on the smaller L3, split over two L3s that each have to hold it, or freshly
/// migrated. That is the ground the cache-aware scheduler knobs (LLC balancing and its
/// tolerances), the V-Cache preference, preferred-core ranking, wake-up placement, buddy and
/// migration-cost settings, workqueue scope and THP (the set is a plain heap) act on.
/// Returns (median frame time, tail). Three fresh thread sets over one freshly built set.
pub fn cache_loop(dur: Duration) -> Option<(f64, f64)> {
    let unit = cache_unit();
    if unit == 0 { return None; }
    let chain = Arc::new(Chain::new(cache_set_bytes(), 0x5EED_CAFE)?);
    let mut all = Vec::with_capacity(96);
    for i in 0..3u64 {
        let c = chain.clone();
        all.extend(std::thread::spawn(move || cache_part(dur / 3, i, unit, c)).join().unwrap_or_default());
    }
    let med = crate::calib::median(&mut all.clone())?;
    Some((med, tail(&mut all)?))
}

/// What `contended` measured: all-thread work per second, work per joule (RAPL) and the
/// frame loop's tail (µs) while every CPU was taken.
pub struct Contended { pub rate: f64, pub eff: Option<f64>, pub frame_tail: Option<f64> }

/// Every CPU taken: 1.5 runnable threads per logical CPU spin over a small working set
/// while the 240 Hz frame loop runs next to them. The spinners' total is the all-thread work
/// figure (and, with RAPL, work per joule); the frame loop's tail is what an interactive
/// thread gets when it has to preempt its way onto a CPU - the only place where the
/// scheduler knobs (slice, preemption, wake-up placement, migration cost, BORE) act at all:
/// on a machine with free CPUs they have nothing to decide.
pub fn contended(dur: Duration) -> Contended {
    let cpus = std::thread::available_parallelism().map_or(1, |n| n.get());
    let threads = cpus + cpus / 2;
    let ((rate, lat), w) = with_pkg_power(|| {
        let hogs: Vec<_> = (0..threads).map(|i| std::thread::spawn(move || {
            let buf = vec![1u8; 128 * 1024];
            let (t0, mut n, mut acc) = (Instant::now(), 0u64, i as u64);
            while t0.elapsed() < dur { acc = work(100_000, acc ^ n, &buf); n += 100_000; }
            std::hint::black_box(acc);
            let secs = t0.elapsed().as_secs_f64();
            if secs > 0.0 { n as f64 / secs } else { 0.0 }
        })).collect();
        let mut lat = Vec::with_capacity(256);
        for i in 0..2u64 {
            lat.extend(std::thread::spawn(move || frame_part(dur / 2, 0xB5 + i)).join().unwrap_or_default());
        }
        (hogs.into_iter().map(|h| h.join().unwrap_or(0.0)).sum::<f64>(), lat)
    });
    let mut lat = lat;
    Contended { rate, eff: w.filter(|w| *w > 0.0).map(|w| rate / w), frame_tail: tail(&mut lat) }
}

/// The short job (`centurion-calibrate __job <iters>`): a fresh 8 MiB heap touched page by page,
/// then `iters` of integer work - a compiler or a shader-compile child in miniature.
pub fn job_main(iters: u64) {
    if let Some(m) = anon(8 * MIB) {
        for i in (0..m.len()).step_by(4096) { m[i] = 1; }
        std::hint::black_box(work(iters, 7, &m[..256 * 1024]));
    }
}

/// Short jobs per second: half the CPUs (2..12) each keep starting `exe __job` and waiting
/// for it. A build, a game launcher or a shader cache is thousands of processes that live a
/// few milliseconds: exec, page faults of a fresh heap, 2 ms of work on a core that was
/// idle, exit. Sustained spins see none of it - where a new task is placed, how fast its
/// core's clock comes up, what a fault costs (THP, reclaim under pressure).
pub fn jobs_per_sec(exe: &std::path::Path, dur: Duration) -> Option<f64> {
    let iters = (unit() * 2).to_string();
    let spawn = |exe: &std::path::Path, iters: &str| std::process::Command::new(exe).arg("__job").arg(iters)
        .stdin(std::process::Stdio::null()).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null())
        .status().map_or(false, |s| s.success());
    if !spawn(exe, &iters) { return None; }  // also brings the binary back into the page cache
    let threads = (std::thread::available_parallelism().map_or(4, |n| n.get()) / 2).clamp(2, 12);
    let t0 = Instant::now();
    let hs: Vec<_> = (0..threads).map(|_| {
        let (exe, iters) = (exe.to_path_buf(), iters.clone());
        std::thread::spawn(move || { let mut n = 0u64; while t0.elapsed() < dur { if !spawn(&exe, &iters) { break; } n += 1; } n })
    }).collect();
    let n: u64 = hs.into_iter().map(|h| h.join().unwrap_or(0)).sum();
    let secs = t0.elapsed().as_secs_f64();
    (n > 0 && secs > 0.0).then(|| n as f64 / secs)
}
