#!/bin/bash
# Linux test run inside WSL Ubuntu: the gateway's unit tests, the Windows-hang
# regression test, and the patched saturating-time crate's own tests.
export PATH="$HOME/.cargo/bin:$PATH"
export CARGO_TARGET_DIR="$HOME/rv-tor-target"
cd "$(dirname "${BASH_SOURCE[0]}")/.."
cargo test --features ffi,test-hooks -j 6 2>&1 | grep -E "test result|FAILED|panicked|^error"
cargo test -p saturating-time -j 6 2>&1 | grep -E "test result|FAILED|panicked|^error"
