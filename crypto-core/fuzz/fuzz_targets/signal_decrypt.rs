// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Mutated Signal message bodies (first and later messages) given to the receiving device: a clean
//! refusal or the original plaintext, never a panic, and never another plaintext.
#![no_main]

#[path = "common.rs"]
mod common;

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use libfuzzer_sys::fuzz_target;
use serde_json::json;

fuzz_target!(|data: &[u8]| {
    let w = common::world();
    let (template, expected) = if data.first().is_some_and(|b| b & 1 == 1) { (&w.first, "first") } else { (&w.later, "later") };
    let mut message = template.clone();
    message["body"] = json!(B64.encode(&data[data.len().min(1)..]));
    if data.get(1).is_some_and(|b| b & 1 == 1) {
        message["type"] = json!(if template["type"] == "prekey" { "signal" } else { "prekey" });
    }
    let mut bob = w.bob.clone();
    let r = bob.op("signalDecrypt", json!({ "sender": w.alice.identity, "certificate": w.alice_cert, "message": message }));
    if r["ok"] == true {
        assert_eq!(r["plaintext"], expected, "a mutated message opened to something else: {r}");
    } else {
        assert!(r["error"].is_string());
    }
});
