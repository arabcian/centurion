//! Line-based request + sample stream plumbing for the monitor helpers
//! (intel-uv-helper, ryzen-co-helper). One pkexec per monitoring session
//! instead of one per sample: the request is the first stdin line, samples go
//! out one JSON line each, and stdin EOF (tab hidden, GUI gone) ends it.

use serde_json::{json, Value};

/// The request: everything up to the first newline, or to EOF (the other
/// callers write one JSON object and close stdin). Raw read(2) on fd 0 — no
/// std buffering, so bytes after the newline stay in the pipe for the stream.
pub fn read_request_line(max: usize) -> Result<Value, Value> {
    let mut buf = Vec::with_capacity(256);
    let mut b = [0u8; 1];
    loop {
        let n = unsafe { libc::read(0, b.as_mut_ptr().cast(), 1) };
        if n < 0 {
            if std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted { continue; }
            return Err(json!({"ok": false, "error": "failed to read stdin"}));
        }
        if n == 0 || b[0] == b'\n' { break; }
        buf.push(b[0]);
        if buf.len() > max { return Err(json!({"ok": false, "error": "payload too large"})); }
    }
    let text = std::str::from_utf8(&buf).map_err(|e| json!({"ok": false, "error": format!("invalid JSON: {e}")}))?;
    serde_json::from_str(text).map_err(|e| json!({"ok": false, "error": format!("invalid JSON: {e}")}))
}

/// One line to stdout; false once the reader is gone (EPIPE: Rust ignores SIGPIPE).
pub fn emit_line(v: &Value) -> bool {
    use std::io::Write;
    let mut out = std::io::stdout().lock();
    writeln!(out, "{v}").and_then(|_| out.flush()).is_ok()
}

/// Waits up to `ms` for stdin. Returns None on EOF/hang-up (stop), else
/// whether a "clear" command arrived.
pub fn wait_stdin(ms: u64, pending: &mut Vec<u8>) -> Option<bool> {
    let end = std::time::Instant::now() + std::time::Duration::from_millis(ms);
    let mut clear = false;
    loop {
        let left = end.saturating_duration_since(std::time::Instant::now());
        if left.is_zero() { return Some(clear); }
        let mut p = libc::pollfd { fd: 0, events: libc::POLLIN, revents: 0 };
        let r = unsafe { libc::poll(&mut p, 1, left.as_millis() as libc::c_int) };
        if r < 0 { if std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted { continue; } return None; }
        if r == 0 { return Some(clear); }
        let mut b = [0u8; 256];
        let n = unsafe { libc::read(0, b.as_mut_ptr().cast(), b.len()) };
        if n <= 0 { return None; }  // EOF: the tab was hidden or the GUI exited
        pending.extend_from_slice(&b[..n as usize]);
        while let Some(i) = pending.iter().position(|&c| c == b'\n') {
            let line: Vec<u8> = pending.drain(..=i).collect();
            clear |= line.starts_with(b"clear");
        }
        if pending.len() > 4096 { pending.clear(); }  // no newline in sight: garbage, dropped
    }
}
