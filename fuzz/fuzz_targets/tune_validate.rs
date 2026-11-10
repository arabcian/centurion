//! tune-helper's request values: a JSON object {knob: value}; validation must never panic and
//! anything it accepts must be re-accepted unchanged.
#![no_main]
use centurion_helpers::tune;
use libfuzzer_sys::fuzz_target;
use serde_json::Value;

fuzz_target!(|data: &[u8]| {
    let Ok(Value::Object(m)) = serde_json::from_slice::<Value>(data) else { return };
    for (k, v) in &m {
        let Some(t) = tune::find(k) else { continue };
        if let Ok(s) = tune::validate(t, v) {
            assert_eq!(tune::validate(t, &Value::String(s.clone())), Ok(s));
        }
    }
});
