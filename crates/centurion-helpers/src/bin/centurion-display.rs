//! centurion-display — internal-panel refresh rate for Scenes. Runs as the user (never through pkexec).
//!
//!   centurion-display list              {"ok", "backend", "output", "resolution", "rates": [Hz…], "current"}
//!   centurion-display set-refresh HZ    {"ok", "message"} — closest offered rate at the current resolution
//!
//! One JSON line on stdout; exit 0 on success, 1 on failure.

use centurion_helpers::display;
use serde_json::json;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let v = match args.first().map(String::as_str) {
        Some("list") => display::list_json(),
        Some("set-refresh") => match args.get(1).and_then(|s| s.parse::<f64>().ok()) {
            Some(hz) => match display::set_refresh(hz) {
                Ok(m) => json!({"ok": true, "message": m}),
                Err(e) => json!({"ok": false, "error": e}),
            },
            None => json!({"ok": false, "error": "usage: centurion-display set-refresh HZ"}),
        },
        _ => {
            eprintln!("usage: centurion-display list | set-refresh HZ");
            std::process::exit(2);
        }
    };
    println!("{v}");
    std::process::exit(if v["ok"] == true { 0 } else { 1 });
}
