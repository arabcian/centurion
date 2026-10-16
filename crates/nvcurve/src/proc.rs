//! Child-process execution with a hard timeout and bounded output capture.

use std::io::Read;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

const MAX_OUTPUT: u64 = 64 * 1024;

pub struct Output {
    pub code: Option<i32>,
    pub stdout: String,
    pub stderr: String,
}

pub enum RunError {
    NotFound,
    Spawn(std::io::Error),
    Timeout,
}

fn drain<R: Read + Send + 'static>(r: Option<R>) -> std::thread::JoinHandle<String> {
    std::thread::spawn(move || {
        let mut v = Vec::new();
        if let Some(mut r) = r {
            let _ = (&mut r).take(MAX_OUTPUT).read_to_end(&mut v);
            let _ = std::io::copy(&mut r, &mut std::io::sink()); // keep the pipe drained
        }
        String::from_utf8_lossy(&v).trim().to_owned()
    })
}

/// Runs `bin args...` with a minimal fixed environment. Never uses a shell.
pub fn run(bin: &str, args: &[String], timeout: Duration) -> Result<Output, RunError> {
    let mut child = Command::new(bin)
        .args(args)
        .env_clear()
        .env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin")
        .env("LC_ALL", "C")
        .current_dir("/")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| if e.kind() == std::io::ErrorKind::NotFound { RunError::NotFound } else { RunError::Spawn(e) })?;
    let o = drain(child.stdout.take());
    let e = drain(child.stderr.take());
    let start = Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(s)) => break s,
            Ok(None) if start.elapsed() >= timeout => {
                let _ = child.kill();
                // Bounded reap: nvidia-smi on a dGPU that fell off the bus sits in D state and ignores
                // SIGKILL; an unbounded wait() turned the timeout into a hang of the whole helper.
                for _ in 0..50 { if !matches!(child.try_wait(), Ok(None)) { break; } std::thread::sleep(Duration::from_millis(20)); }
                return Err(RunError::Timeout);
            }
            Ok(None) => std::thread::sleep(Duration::from_millis(20)),
            Err(err) => return Err(RunError::Spawn(err)),
        }
    };
    Ok(Output {
        code: status.code(),
        stdout: o.join().unwrap_or_default(),
        stderr: e.join().unwrap_or_default(),
    })
}

/// First existing candidate that is root-owned, executable, not setuid and not
/// group/world-writable — and the same for every directory above the real path.
/// A user-owned directory would let the caller swap the binary after this check.
pub fn find_trusted(candidates: &[&'static str]) -> Option<&'static str> {
    use std::os::unix::fs::MetadataExt;
    candidates.iter().copied().find(|p| {
        let Ok(real) = std::fs::canonicalize(p) else { return false };
        let Ok(md) = std::fs::metadata(&real) else { return false };
        if !md.is_file() || md.uid() != 0 || md.mode() & 0o6022 != 0 || md.mode() & 0o111 == 0 {
            return false;
        }
        let mut dir = real.parent();
        while let Some(d) = dir {
            match std::fs::metadata(d) {
                Ok(m) if m.is_dir() && m.uid() == 0 && m.mode() & 0o022 == 0 => {}
                _ => return false,
            }
            dir = d.parent();
        }
        true
    })
}
