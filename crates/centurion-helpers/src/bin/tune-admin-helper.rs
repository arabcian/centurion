//! `tune-admin-helper`: same source as `tune-helper`, role fixed by CARGO_BIN_NAME (see tune-helper.rs). Serves only
//! ADMIN_OPS (boot preset, boot calibration, wake sources, wireless power lock, unlock PIN) through
//! com.centurion.tune.admin, which asks for the administrator password every time.
#[path = "tune-helper.rs"]
mod imp;

fn main() { imp::main() }
