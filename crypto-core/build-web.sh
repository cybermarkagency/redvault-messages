#!/usr/bin/env bash
# The engine for the web app (ui/src/lib/e2eV3): WebAssembly plus its JS glue
# in crypto-core/pkg/web, which the web app loads only when the protocol-3
# preview is switched on. Needs Rust 1.99.1 with the wasm32 target and
# wasm-bindgen-cli 0.2.129 (the version the crate pins).
set -euo pipefail
CRATE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$HOME/.cargo/bin:$PATH"
cd "$CRATE"
RUSTFLAGS='--cfg getrandom_backend="wasm_js"' cargo build --release --locked --target wasm32-unknown-unknown --features wasm
wasm-bindgen --target web --out-dir "$CRATE/pkg/web" target/wasm32-unknown-unknown/release/redvault_e2ee.wasm
# The same module for Node (the web app's tests load it there).
wasm-bindgen --target nodejs --out-dir "$CRATE/pkg/node" target/wasm32-unknown-unknown/release/redvault_e2ee.wasm
ls -l "$CRATE/pkg/web/redvault_e2ee_bg.wasm"
