//! AMD GPU tuning helper: overclock / undervolt (overdrive table), power
//! limit, power profile, performance level and PMFW fan settings of one
//! amdgpu card. Protocol and write order: see centurion_helpers::amdgpu.
//!   {"op": "apply", "card": "card1", "settings": {...}}
//!   {"op": "reset", "card": "card1"}

use centurion_helpers::*;

fn main() {
    init();
    let code = match read_request(amdgpu::MAX_REQUEST) {
        // Both ops write the card (overdrive, power limit, fan): wait for a running calibration.
        Ok(req) => finish(refuse_during_calibration().unwrap_or_else(|| amdgpu::handle(&req))),
        Err(e) => finish(e),
    };
    std::process::exit(code);
}
