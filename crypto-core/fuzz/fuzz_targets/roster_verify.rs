//! Arbitrary roster chains (any JSON), and a real roster with mutated fields, given to rosterVerify
//! on a device that knows the issuer: a clean refusal, never a panic. A mutated roster is never
//! accepted with members other than the signed ones.
#![no_main]

#[path = "common.rs"]
mod common;

use libfuzzer_sys::fuzz_target;
use serde_json::{json, Value};

fuzz_target!(|data: &[u8]| {
    let w = common::world();
    let mut device = w.alice.clone();
    // A fresh view of the group: alice's own state already has the roster, so use bob's known contacts
    // through a group id the device hasn't seen.
    let chain: Value = match serde_json::from_slice::<Value>(data) {
        Ok(v) if data.first() != Some(&b'!') => v,
        _ => {
            let mut r = w.roster.clone();
            let text = String::from_utf8_lossy(data).to_string();
            match data.len() % 4 {
                0 => r["members"] = json!([text]),
                1 => r["epoch"] = json!(data.len() as u64),
                2 => r["signature"] = json!(text),
                _ => r["prev"] = json!(text),
            }
            json!([r])
        }
    };
    let r = device.op("rosterVerify", json!({ "groupId": "fuzz-group", "chain": chain }));
    if r["ok"] == true {
        // Only a real, unmodified chain for this group id could pass; the roster above is for group "g".
        panic!("a fuzzed chain verified: {r}");
    }
    assert!(r["error"].is_string());
});
