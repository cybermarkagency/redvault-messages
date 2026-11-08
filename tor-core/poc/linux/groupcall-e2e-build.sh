#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Builds the desktop group-call test participant for Linux (groupcall-e2e/CMakeLists.txt) in ~/rv-linux. Needs aptroot.sh
# to have installed: cmake ninja-build libsodium-dev libcurl4-openssl-dev nlohmann-json3-dev protobuf-compiler libprotobuf-dev.
# Downloads LiveKit's prebuilt liblivekit_ffi.so (pinned version and SHA-256, checked by desktop/groupcall/CMakeLists.txt).
set -euo pipefail
REPO="${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}"
ROOT="${ROOT:-$HOME/aptroot}"; SR="$ROOT/sysroot"; WORK="$HOME/rv-linux"
export PATH="$SR/usr/bin:$HOME/.cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export LD_LIBRARY_PATH="$SR/usr/lib/x86_64-linux-gnu:$SR/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="$SR/usr/lib/x86_64-linux-gnu/pkgconfig:$SR/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SR"
export CMAKE_PREFIX_PATH="$SR/usr"
export CPLUS_INCLUDE_PATH="$SR/usr/include:$SR/usr/include/x86_64-linux-gnu"
export C_INCLUDE_PATH="$SR/usr/include:$SR/usr/include/x86_64-linux-gnu"
export LIBRARY_PATH="$SR/usr/lib/x86_64-linux-gnu"
for part in desktop; do
  rm -rf "$WORK/$part"; mkdir -p "$WORK/$part"
  tar -C "$REPO/$part" --exclude=./build --exclude=./dist --exclude=./downloads -cf - . | tar -C "$WORK/$part" -xf -
done
mkdir -p "$WORK/tor-core/poc/linux"; rm -rf "$WORK/tor-core/poc/linux/groupcall-e2e"
cp -r "$REPO/tor-core/poc/linux/groupcall-e2e" "$WORK/tor-core/poc/linux/"
rm -rf "$WORK/build-gc"
cmake -S "$WORK/tor-core/poc/linux/groupcall-e2e" -B "$WORK/build-gc" -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -12
cmake --build "$WORK/build-gc" -j 12 2>&1 | grep -E "error|FAILED|Linking CXX executable" | tail -15
ls -la "$WORK/build-gc/redvault_groupcall_e2e" "$WORK/build-gc/liblivekit_ffi.so"
