#!/bin/bash
# Linux half of the proof of concept, run inside WSL Ubuntu: unit tests and the
# probe binaries (output in ~/rv-tor-target/debug).
set -e
export PATH="$HOME/.cargo/bin:$PATH"
CRATE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export CARGO_TARGET_DIR="$HOME/rv-tor-target"
cd "$CRATE"
cargo test --features ffi,test-hooks -j 6 2>&1 | grep -E "test result|FAILED|panicked|^error" || true
cargo build --features ffi,test-hooks,onion-host -j 6 2>&1 | grep -E "^error|warning: unused|Finished" || true
ls -la "$CARGO_TARGET_DIR/debug/rv-tor-probe" "$CARGO_TARGET_DIR/debug/rv-onion-host" "$CARGO_TARGET_DIR/debug/libredvault_tor.so"
