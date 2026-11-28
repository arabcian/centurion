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

type Drained = (std::sync::Arc<std::sync::Mutex<Vec<u8>>>, std::sync::mpsc::Receiver<()>);

/// Reads `r` on a thread into a shared buffer (first MAX_OUTPUT bytes; the rest is drained so the pipe never
/// fills) and signals EOF. The buffer is shared so that output is not lost when EOF never comes.
fn drain<R: Read + Send + 'static>(r: Option<R>) -> Drained {
    let buf = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
    let (tx, rx) = std::sync::mpsc::channel();
    let shared = buf.clone();
    std::thread::spawn(move || {
        if let Some(mut r) = r {
            let mut chunk = [0u8; 4096];
            loop {
                match r.read(&mut chunk) {
                    Ok(0) => break,
                    Ok(n) => {
                        let mut b = shared.lock().unwrap_or_else(|p| p.into_inner());
                        let room = (MAX_OUTPUT as usize).saturating_sub(b.len());
                        b.extend_from_slice(&chunk[..n.min(room)]);
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                    Err(_) => break,
                }
            }
        }
        let _ = tx.send(());
    });
    (buf, rx)
}

/// What a drained stream printed. Waits for its EOF only until `deadline`: a child of the tool (or a tool
/// that closed nothing) can hold the pipe after the tool itself has exited.
fn collect(d: Drained, deadline: Instant) -> String {
    let _ = d.1.recv_timeout(deadline.saturating_duration_since(Instant::now()));
    let b = d.0.lock().unwrap_or_else(|p| p.into_inner());
    String::from_utf8_lossy(&b).trim().to_owned()
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
                // Bounded reap: nvidia-smi on a dGPU that fell off the bus sits in D state and ignores
                // SIGKILL; an unbounded wait() turned the timeout into a hang of the whole helper.
                centurion_helpers::kill_reap(&mut child);
                return Err(RunError::Timeout);
            }
            Ok(None) => std::thread::sleep(Duration::from_millis(20)),
            Err(err) => return Err(RunError::Spawn(err)),
        }
    };
    let deadline = Instant::now() + Duration::from_millis(500);
    Ok(Output { code: status.code(), stdout: collect(o, deadline), stderr: collect(e, deadline) })
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

#[cfg(test)]
mod tests {
    use super::*;
    /// A tool that exits while one of its own children still holds the output pipe: the result (and what it
    /// printed) must come back at once, not when that child lets go of the pipe.
    #[test]
    fn exit_is_not_held_up_by_an_inherited_pipe() {
        let t = Instant::now();
        let args = ["-c".to_owned(), "sleep 5 & echo hi; exit 3".to_owned()];
        let Ok(o) = run("/bin/sh", &args, Duration::from_secs(10)) else { panic!("sh did not run") };
        assert!(t.elapsed() < Duration::from_secs(3), "took {:?}", t.elapsed());
        assert_eq!(o.code, Some(3));
        assert_eq!(o.stdout, "hi");
    }
}
