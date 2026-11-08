// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Mutated group message bodies, epochs and distribution ids at a member device: a clean refusal
//! or the original plaintext, never a panic.
#![no_main]

#[path = "common.rs"]
mod common;

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use libfuzzer_sys::fuzz_target;
use serde_json::json;

fuzz_target!(|data: &[u8]| {
    let w = common::world();
    let mut message = w.group.clone();
    match data.first().copied().unwrap_or(0) % 3 {
        0 => message["body"] = json!(B64.encode(&data[data.len().min(1)..])),
        1 => message["epoch"] = json!(u64::from_le_bytes({
            let mut b = [0u8; 8];
            for (i, x) in data.iter().skip(1).take(8).enumerate() {
                b[i] = *x;
            }
            b
        })),
        _ => message["distributionId"] = json!(String::from_utf8_lossy(&data[data.len().min(1)..]).to_string()),
    }
    let mut bob = w.bob.clone();
    let r = bob.op("groupSignalDecrypt", json!({ "sender": w.alice.identity, "message": message }));
    if r["ok"] == true {
        assert_eq!(r["plaintext"], "group", "a mutated group message opened to something else: {r}");
    } else {
        assert!(r["error"].is_string());
    }
});
