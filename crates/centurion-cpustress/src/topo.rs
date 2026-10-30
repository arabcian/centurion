//! Which CPUs are online, which logical CPUs share a physical core, and pinning the calling thread.

use std::path::Path;

const CPU_DIR: &str = "/sys/devices/system/cpu";

/// "0-3,8,10-11" → [0,1,2,3,8,10,11]. None on anything malformed.
pub fn parse_list(s: &str) -> Option<Vec<usize>> {
    let mut out = Vec::new();
    for part in s.trim().split(',').filter(|p| !p.is_empty()) {
        match part.split_once('-') {
            Some((a, b)) => {
                let (a, b): (usize, usize) = (a.trim().parse().ok()?, b.trim().parse().ok()?);
                if b < a || b - a > 4096 { return None; }
                out.extend(a..=b);
            }
            None => out.push(part.trim().parse().ok()?),
        }
    }
    out.sort_unstable();
    out.dedup();
    Some(out)
}

/// One physical core: its logical CPUs (SMT siblings), ascending. `cpus[0]` is the one the per-core test loads.
#[derive(Clone, Debug, PartialEq)]
pub struct Core { pub cpus: Vec<usize>, pub l3: Option<u32> }

fn read(p: impl AsRef<Path>) -> Option<String> { std::fs::read_to_string(p).ok() }

/// Online physical cores, ordered by their first logical CPU.
pub fn cores() -> Vec<Core> {
    let online = read(format!("{CPU_DIR}/online")).and_then(|s| parse_list(&s)).unwrap_or_default();
    let mut out: Vec<Core> = Vec::new();
    for &c in &online {
        let mut sib = read(format!("{CPU_DIR}/cpu{c}/topology/thread_siblings_list")).and_then(|s| parse_list(&s)).unwrap_or_else(|| vec![c]);
        sib.retain(|s| online.contains(s));
        if sib.first() != Some(&c) { continue; }  // listed with its first sibling
        let l3 = read(format!("{CPU_DIR}/cpu{c}/cache/index3/id")).and_then(|s| s.trim().parse().ok());
        out.push(Core { cpus: sib, l3 });
    }
    out
}

/// Pins the calling thread to `cpu` and checks that it now runs there.
pub fn pin(cpu: usize) -> Result<(), String> {
    // SAFETY: cpu_set_t is plain data; CPU_SET bounds-checks against its size.
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        libc::CPU_ZERO(&mut set);
        libc::CPU_SET(cpu, &mut set);
        if libc::sched_setaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &set) != 0 {
            return Err(format!("cannot pin to CPU {cpu}: {}", std::io::Error::last_os_error()));
        }
        if libc::sched_getcpu() != cpu as i32 { return Err(format!("pinned to CPU {cpu} but running elsewhere")); }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lists() {
        assert_eq!(parse_list("0-3,8,10-11\n"), Some(vec![0, 1, 2, 3, 8, 10, 11]));
        assert_eq!(parse_list("5"), Some(vec![5]));
        assert_eq!(parse_list(""), Some(vec![]));
        assert_eq!(parse_list("3-1"), None);
        assert_eq!(parse_list("a"), None);
    }

    #[test]
    fn this_machine_has_cores() {
        let c = cores();
        assert!(!c.is_empty());
        assert!(c.windows(2).all(|w| w[0].cpus[0] < w[1].cpus[0]));
        assert!(pin(c[0].cpus[0]).is_ok());
    }
}
