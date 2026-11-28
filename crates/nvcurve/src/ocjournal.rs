//! Curve journal line for the NVIDIA curve: centurion_helpers::ocjournal (same file and format, see there)
//! with kind "gpu_curve". Root only, never fatal.

pub use centurion_helpers::ocjournal::{fingerprint, FILE};

pub fn record(state: &str) { record_values(state, None) }

/// With the profile that was applied: its fingerprint always, the values themselves up to 8 KiB.
pub fn record_values(state: &str, values: Option<&serde_json::Value>) {
    centurion_helpers::ocjournal::record_values("gpu_curve", state, values)
}
