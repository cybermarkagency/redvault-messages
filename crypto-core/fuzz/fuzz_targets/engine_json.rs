//! Any bytes as a request to the engine's single entry point: it must always answer JSON, never panic.
#![no_main]

use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if let Ok(text) = std::str::from_utf8(data) {
        let answer = redvault_e2ee::engine::handle(text);
        let parsed: serde_json::Value = serde_json::from_str(&answer).expect("the engine always answers JSON");
        assert!(parsed.get("ok").is_some());
    }
});
