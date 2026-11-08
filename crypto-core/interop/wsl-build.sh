#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# The Linux half of build-all.sh, run inside WSL Ubuntu: the engine's tests and
# the C++ driver against the Linux static library (output: ~/rv-bin/cpp_driver).
set -e
export PATH="$HOME/.cargo/bin:$PATH"
CRATE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export CARGO_TARGET_DIR="$HOME/rv-target"
cd "$CRATE"
cargo test --release --locked 2>&1 | grep -E "test result|FAILED|panicked" || true
cargo build --release --features ffi --locked 2>&1 | tail -1
mkdir -p "$HOME/rv-bin"
g++ -std=c++20 -O2 -I "$CRATE/include" "$CRATE/interop/drivers/cpp_driver.cpp" "$CARGO_TARGET_DIR/release/libredvault_e2ee.a" \
  -lpthread -ldl -lm -o "$HOME/rv-bin/cpp_driver"
echo '{"op":"version"}' | "$HOME/rv-bin/cpp_driver"
