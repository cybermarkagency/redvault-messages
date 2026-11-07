#!/usr/bin/env bash
# Builds libredvault_tor.so exactly as the Linux desktop build does (the CMake target redvault_tor_build: cargo build
# --release --locked --lib --features ffi) inside the tree core-build.sh configured, then lists what the library
# exports and links. Run core-build.sh first.
set -euo pipefail
ROOT="${ROOT:-$HOME/aptroot}"; SR="$ROOT/sysroot"; WORK="$HOME/rv-linux"
export PATH="$SR/usr/bin:$HOME/.cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export LD_LIBRARY_PATH="$SR/usr/lib/x86_64-linux-gnu:$SR/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
cd "$WORK/build"
time cmake --build . --target redvault_tor_build 2>&1 | tail -5
lib="$WORK/build/tor-core-target/release/libredvault_tor.so"
ls -la "$lib"
echo "== exported C API"
nm -D --defined-only "$lib" | grep " rv_tor_" || true
echo "== shared library dependencies"
ldd "$lib" | sed 's/(0x.*//' | sort
