#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Builds and tests the desktop core (no Qt app, no calls) on Linux, inside WSL, with the user-space dependencies from
# aptroot.sh (cmake ninja-build libsodium-dev libcurl4-openssl-dev nlohmann-json3-dev catch2) and Rust from ~/.cargo.
# It covers what the Windows build can't: the Tor transport's socket guard on POSIX sockets, the SOCKS client, the
# outbox, the resumable Files transfers, and the Rust gateway (tor-core, libredvault_tor.so) built by CMake.
#
#   core-build.sh [ctest args...]       REPO=/mnt/c/.../Anonymous\ messaging\ app (default: found from this script)
set -euo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}"
ROOT="${ROOT:-$HOME/aptroot}"
SR="$ROOT/sysroot"
WORK="$HOME/rv-linux"
# A Linux-only PATH: WSL appends the Windows directories, and CMake derives search prefixes from PATH (it found MSYS2's curl).
export PATH="$SR/usr/bin:$HOME/.cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export LD_LIBRARY_PATH="$SR/usr/lib/x86_64-linux-gnu:$SR/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="$SR/usr/lib/x86_64-linux-gnu/pkgconfig:$SR/usr/share/pkgconfig:${PKG_CONFIG_PATH:-}"
export PKG_CONFIG_SYSROOT_DIR="$SR"
export CMAKE_PREFIX_PATH="$SR/usr"
export CPLUS_INCLUDE_PATH="$SR/usr/include:$SR/usr/include/x86_64-linux-gnu"
export C_INCLUDE_PATH="$SR/usr/include:$SR/usr/include/x86_64-linux-gnu"
export LIBRARY_PATH="$SR/usr/lib/x86_64-linux-gnu"

echo "== sources into $WORK (Linux filesystem: much faster than /mnt/c)"
mkdir -p "$WORK"
for part in desktop legal; do
  rm -rf "$WORK/$part"; mkdir -p "$WORK/$part"
  tar -C "$REPO/$part" --exclude=./build --exclude=./dist --exclude=./downloads -cf - . | tar -C "$WORK/$part" -xf -
done
rm -rf "$WORK/tor-core"; mkdir -p "$WORK/tor-core"
tar -C "$REPO/tor-core" --exclude=./target --exclude=./poc/results -cf - . | tar -C "$WORK/tor-core" -xf -
mkdir -p "$WORK/android/app/src/androidTest"
[ -d "$REPO/android/app/src/androidTest/assets" ] && cp -r "$REPO/android/app/src/androidTest/assets" "$WORK/android/app/src/androidTest/" || true

echo "== configure"
rm -rf "$WORK/build"   # a stale CMakeCache.txt would keep an earlier run's package locations
cmake --version | head -1
cmake -S "$WORK/desktop" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DREDVAULT_BUILD_APP=OFF -DREDVAULT_BUILD_CALLS=OFF -DREDVAULT_BUILD_TOOLS=OFF -DREDVAULT_BUILD_CLI=ON \
  -DREDVAULT_TOR=ON -DCARGO_TARGET_DIR_OVERRIDE="$HOME/rv-tor-target" 2>&1 | tail -15

echo "== build"
cmake --build "$WORK/build" -j 12 2>&1 | grep -E "error|FAILED|warning: unused|Linking|\[[0-9]+/[0-9]+\] Linking CXX executable" | tail -15
echo "== test"
cd "$WORK/build"
ctest -j 8 --output-on-failure "$@" 2>&1 | tail -25
