//! amdgpu-helper: pp_od_clk_voltage text (first line of input up to a NUL) and a JSON settings
//! object (the rest). Parsing and command planning must never panic.
#![no_main]
use centurion_helpers::amdgpu;
use libfuzzer_sys::fuzz_target;
use serde_json::Value;

fuzz_target!(|data: &[u8]| {
    let (text, json) = match data.iter().position(|&b| b == 0) {
        Some(i) => (&data[..i], &data[i + 1..]),
        None => (data, &[][..]),
    };
    let table = amdgpu::parse_od(&String::from_utf8_lossy(text));
    if let Ok(Value::Object(s)) = serde_json::from_slice::<Value>(json) {
        let _ = amdgpu::od_commands(&table, &s);
    }
});
