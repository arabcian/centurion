//! `sysadmin-config-helper`: same source as `sysadmin-helper`, role fixed by CARGO_BIN_NAME (see sysadmin-helper.rs).
#[path = "sysadmin-helper.rs"]
mod imp;

fn main() { imp::main() }
