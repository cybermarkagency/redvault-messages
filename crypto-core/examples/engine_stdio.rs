// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! The engine as a line-oriented process: one JSON request per input line, one JSON response
//! per output line. For tests that drive the real engine from another language (for example
//! server/src/test/routes/e2eSignal.engine.test.ts). Prints nothing else.
//!
//! Build: cargo build --features signal --example engine_stdio
use std::io::{self, BufRead, Write};

fn main() {
    let stdin = io::stdin();
    let mut out = io::stdout().lock();
    for line in stdin.lock().lines() {
        let Ok(line) = line else { break };
        if line.trim().is_empty() {
            continue;
        }
        let response = redvault_e2ee::engine::handle(&line);
        writeln!(out, "{response}").unwrap();
        out.flush().unwrap();
    }
}
