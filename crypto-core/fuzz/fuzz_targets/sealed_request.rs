// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! The gateway's internet-facing input (docs/tor-sealed-fallback.md): any bytes, as a sealed request
//! and as the inner Binary HTTP. It must never panic, and a refused input must stay refused.
#![no_main]

use libfuzzer_sys::fuzz_target;
use redvault_e2ee::sealed::{decode_request, Gateway};

fuzz_target!(|data: &[u8]| {
    let _ = decode_request(data);
    static GATEWAY: std::sync::OnceLock<Gateway> = std::sync::OnceLock::new();
    let gw = GATEWAY.get_or_init(|| {
        let (config, _) = Gateway::key_from_secret(1, b"redvault sealed test key, never used on the live gateway").unwrap();
        Gateway::new(vec![config]).unwrap()
    });
    let _ = gw.open_request(data);
});
