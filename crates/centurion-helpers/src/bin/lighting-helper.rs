//! Keyboard lighting helper (Spectrum per-key RGB; 4-zone RGB keyboards).
//! Protocol and access model: see centurion_helpers::lighting. Runs as the user
//! when the udev rule grants the hidraw node, through pkexec otherwise.

use centurion_helpers::*;

fn main() {
    init();
    let code = match read_request(lighting::MAX_REQUEST) {
        Ok(req) => finish(lighting::handle(&req)),
        Err(e) => finish(e),
    };
    std::process::exit(code);
}
