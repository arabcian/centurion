//! Keyboard lighting helper (Spectrum per-key RGB; 4-zone RGB keyboards).
//! Protocol and access model: see centurion_helpers::lighting. Tried as the
//! user first; the hidraw node is normally root-only, so it runs through pkexec.

use centurion_helpers::*;

fn main() {
    init();
    // Long-running: frames on stdin until EOF (see lighting::stream), not one request.
    if std::env::args().nth(1).as_deref() == Some("stream") {
        std::process::exit(finish(lighting::stream()));
    }
    let code = match read_request(lighting::MAX_REQUEST) {
        Ok(req) => finish(lighting::handle(&req)),
        Err(e) => finish(e),
    };
    std::process::exit(code);
}
