//! `tune-profile-helper`: same source as `tune-helper`, role fixed by CARGO_BIN_NAME (see tune-helper.rs).
#[path = "tune-helper.rs"]
mod imp;

fn main() { imp::main() }
