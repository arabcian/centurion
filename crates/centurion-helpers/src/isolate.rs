//! Game CPU partition: a cgroup v2 cpuset partition ("root") that gives the
//! game's CCD to the game alone, without CPU hot-plug.
//!
//! A partition root takes its CPUs out of the parent's effective set, so every
//! other cgroup (desktop, services, user sessions) is confined to the rest of
//! the chip, while all CPUs stay online: Wine/Proton and nvidia-powerd see an
//! unchanged topology. "root" (not "isolated") keeps load balancing inside the
//! partition, which a multi-threaded game needs.
//!
//! The same cgroup can also (or only) carry a cpu.weight: the game's CPU share
//! against every other top-level cgroup (default weight 100), so background work
//! yields to it without being stopped.
//!
//! Needs the unified hierarchy at /sys/fs/cgroup with the cpuset (partition) or
//! cpu (weight) controller (OpenRC: rc_cgroup_mode="unified"). Created by tune-helper
//! for a running game session and removed by the full restore that ends game mode.

use serde_json::{json, Value};
use std::path::{Path, PathBuf};

pub const ROOT: &str = "/sys/fs/cgroup";
pub const CG: &str = "/sys/fs/cgroup/centurion-game";
const STATE: &str = "/run/centurion/tune/isolate.json";

fn read(p: &str) -> Option<String> { std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned()) }

fn write(p: &str, v: &str) -> Result<(), String> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    std::fs::OpenOptions::new().write(true).custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(p)
        .and_then(|mut f| f.write_all(v.as_bytes()))
        .map_err(|e| format!("{p}: {e}"))
}

fn cg(file: &str) -> String { format!("{CG}/{file}") }

/// Why the partition cannot be used here, if it cannot.
pub fn unsupported() -> Option<String> { unsupported_for("cpuset") }

/// Why controller `ctrl` (cpuset / cpu) cannot be used here, if it cannot.
fn unsupported_for(ctrl: &str) -> Option<String> {
    match read(&format!("{ROOT}/cgroup.controllers")) {
        None => Some("cgroup v2 is not mounted at /sys/fs/cgroup (OpenRC: rc_cgroup_mode=\"unified\")".into()),
        Some(c) if !c.split_whitespace().any(|x| x == ctrl) => Some(format!("the {ctrl} controller is not available in cgroup v2")),
        Some(_) => None,
    }
}

/// Current partition (readable by any user).
pub fn status() -> Value {
    if !Path::new(CG).is_dir() {
        return json!({"active": false, "unsupported": unsupported()});
    }
    json!({
        "active": true,
        "cpus": read(&cg("cpuset.cpus.effective")).unwrap_or_default(),
        "partition": read(&cg("cpuset.cpus.partition")).unwrap_or_default(),
        "procs": read(&cg("cgroup.procs")).map_or(0, |s| s.lines().count()),
        "weight": read(&cg("cpu.weight")),
        "uclamp_min": read(&cg("cpu.uclamp.min")),
        "uclamp_max": read(&cg("cpu.uclamp.max")),
        "memory_low": read(&cg("memory.low")),
    })
}

fn load_state() -> Value {
    crate::read_root_file(STATE, 64 * 1024).and_then(|s| serde_json::from_str(&s).ok()).unwrap_or_else(|| json!({}))
}

fn save_state(v: &Value) -> Result<(), String> { crate::write_root_file(STATE, v.to_string().as_bytes()) }

/// Creates (or keeps) the game cgroup: a partition over `cpus` (none if empty) and/or `weight` as its cpu.weight.
pub fn setup(cpus: &[usize], weight: Option<u32>, uclamp_min: Option<u32>, uclamp_max: Option<u32>, memory_low: Option<u64>) -> Result<(), String> {
    if cpus.is_empty() && weight.is_none() && uclamp_min.is_none() && uclamp_max.is_none() && memory_low.is_none() {
        return Err("no game cgroup policy requested".into());
    }
    if uclamp_min.is_some_and(|n| n > 1024) || uclamp_max.is_some_and(|n| n > 1024)
        || uclamp_min.unwrap_or(0) > uclamp_max.unwrap_or(1024) {
        return Err("uclamp must be within 0..1024 and minimum <= maximum".into());
    }
    if memory_low.is_some_and(|n| n > 64 * 1024 * 1024 * 1024) { return Err("memory.low exceeds 64 GiB".into()); }
    if !cpus.is_empty() { if let Some(e) = unsupported() { return Err(e); } }
    let cpu_needed = weight.is_some() || uclamp_min.is_some() || uclamp_max.is_some();
    if cpu_needed { if let Some(e) = unsupported_for("cpu") { return Err(e); } }
    if memory_low.is_some() { if let Some(e) = unsupported_for("memory") { return Err(e); } }
    // The cgroup has to expose these files; a missing kernel feature is not silently ignored.
    if (uclamp_min.is_some() || uclamp_max.is_some()) && !Path::new(&format!("{ROOT}/cpu.uclamp.min")).is_file() {
        return Err("CPU uclamp files not supported by this kernel".into());
    }
    if memory_low.is_some() && !Path::new(&format!("{ROOT}/memory.low")).is_file() {
        return Err("memory.low not supported by this kernel".into());
    }
    let list = crate::tune::fmt_cpu_list(cpus);
    let part_ok = cpus.is_empty() || (Path::new(CG).is_dir() && read(&cg("cpuset.cpus.partition")).as_deref() == Some("root")
        && read(&cg("cpuset.cpus")).map(|c| crate::tune::cpu_list(&c)) == Some(cpus.to_vec()));
    if part_ok
       && weight.map_or(true, |w| read(&cg("cpu.weight")) == Some(w.to_string()))
       && uclamp_min.map_or(true, |w| read(&cg("cpu.uclamp.min")) == Some(format!("{:.2}", w as f64 * 100.0 / 1024.0)))
       && uclamp_max.map_or(true, |w| read(&cg("cpu.uclamp.max")) == Some(format!("{:.2}", w as f64 * 100.0 / 1024.0)))
       && memory_low.map_or(true, |w| read(&cg("memory.low")) == Some(w.to_string())) { return Ok(()); }
    let mut st = load_state();
    for (ctrl, mark, need) in [("cpuset", "added", !cpus.is_empty()), ("cpu", "added_cpu", cpu_needed), ("memory", "added_memory", memory_low.is_some())] {
        if !need || st.get(mark).is_some() { continue; }
        let sub = read(&format!("{ROOT}/cgroup.subtree_control")).unwrap_or_default();
        let added = !sub.split_whitespace().any(|c| c == ctrl);
        st[mark] = json!(added);
        save_state(&st)?;  // recorded before the change, so a crash still undoes it
        if added { write(&format!("{ROOT}/cgroup.subtree_control"), &format!("+{ctrl}"))?; }
    }
    match std::fs::create_dir(CG) {
        Ok(()) => {}
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {}
        Err(e) => return Err(format!("{CG}: {e}")),
    }
    if !part_ok {
        let r = write(&cg("cpuset.cpus"), &list).and_then(|_| write(&cg("cpuset.cpus.partition"), "root"));
        let part = read(&cg("cpuset.cpus.partition")).unwrap_or_default();
        if r.is_err() || part != "root" {
            let why = r.err().unwrap_or_else(|| format!("kernel reports '{part}'"));
            let _ = teardown();
            return Err(format!("partition over {list} refused: {why}"));
        }
    }
    if let Some(w) = weight {
        if let Err(e) = write(&cg("cpu.weight"), &w.to_string()) { let _ = teardown(); return Err(format!("cpu.weight {w} refused: {e}")); }
    }
    // cgroup uclamp uses 0.00..100.00 percent, not the sched_setattr 0..1024 representation.
    // Set max before min when raising min; lower min before max when lowering max.
    if uclamp_max.is_some() {
        // A previously raised minimum can make lowering max fail with EINVAL.
        // This game cgroup is empty until join(), so reset its minimum first.
        if let Err(e) = write(&cg("cpu.uclamp.min"), "0") { let _ = teardown(); return Err(format!("cpu.uclamp.min reset: {e}")); }
    }
    if let Some(v) = uclamp_max { if let Err(e) = write(&cg("cpu.uclamp.max"), &format!("{:.2}", v as f64 * 100.0 / 1024.0)) { let _ = teardown(); return Err(format!("cpu.uclamp.max: {e}")); } }
    if let Some(v) = uclamp_min { if let Err(e) = write(&cg("cpu.uclamp.min"), &format!("{:.2}", v as f64 * 100.0 / 1024.0)) { let _ = teardown(); return Err(format!("cpu.uclamp.min: {e}")); } }
    if let Some(v) = memory_low { if let Err(e) = write(&cg("memory.low"), &v.to_string()) { let _ = teardown(); return Err(format!("memory.low: {e}")); } }
    Ok(())
}

/// cgroup of `pid` as a directory under /sys/fs/cgroup.
fn cgroup_of(pid: i32) -> Option<PathBuf> {
    let s = std::fs::read_to_string(format!("/proc/{pid}/cgroup")).ok()?;
    let rel = s.lines().find_map(|l| l.strip_prefix("0::"))?;
    if rel.split('/').any(|c| c == "..") { return None; }
    let p = PathBuf::from(format!("{ROOT}{rel}"));
    p.join("cgroup.procs").is_file().then_some(p)
}

/// Moves `pid` (and its future children) into the partition.
pub fn join(pid: i32) -> Result<(), String> {
    if !Path::new(CG).is_dir() { return Err("game partition not set up".into()); }
    let mut st = load_state();
    if st.get("origin").is_none() {
        if let Some(o) = cgroup_of(pid).filter(|o| o != Path::new(CG)) {
            st["origin"] = json!(o.display().to_string());
            save_state(&st)?;
        }
    }
    write(&cg("cgroup.procs"), &pid.to_string())
}

/// Moves everything left in the partition back and removes it. Ok(true) = removed.
pub fn teardown() -> Result<bool, String> {
    let st = load_state();
    let existed = Path::new(CG).is_dir();
    if existed {
        let origin = st["origin"].as_str().map(PathBuf::from)
            .filter(|o| o.starts_with(ROOT) && o.as_path() != Path::new(CG) && o.join("cgroup.procs").is_file())
            .unwrap_or_else(|| PathBuf::from(ROOT));
        let dest = origin.join("cgroup.procs").display().to_string();
        for _ in 0..20 {
            let pids = read(&cg("cgroup.procs")).unwrap_or_default();
            if pids.is_empty() { break; }
            for pid in pids.lines() {
                if write(&dest, pid).is_err() { let _ = write(&format!("{ROOT}/cgroup.procs"), pid); }
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        let _ = write(&cg("cpuset.cpus.partition"), "member");
        let mut removed = false;
        for _ in 0..20 {
            if std::fs::remove_dir(CG).is_ok() { removed = true; break; }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        if !removed { return Err(format!("{CG} could not be removed (tasks still exiting?)")); }
    }
    if st["added"] == true { let _ = write(&format!("{ROOT}/cgroup.subtree_control"), "-cpuset"); }
    if st["added_cpu"] == true { let _ = write(&format!("{ROOT}/cgroup.subtree_control"), "-cpu"); }
    if st["added_memory"] == true { let _ = write(&format!("{ROOT}/cgroup.subtree_control"), "-memory"); }
    let _ = std::fs::remove_file(STATE);
    Ok(existed)
}
