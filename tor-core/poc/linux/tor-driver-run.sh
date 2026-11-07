#!/usr/bin/env bash
# The desktop core's real end-to-end Tor run (poc/desktop-driver/tor_e2e_driver.cpp) on Linux: the Linux build of the core,
# talking through the Linux build of the Rust gateway (libredvault_tor.so, from tor-lib-build.sh), over the real Tor
# network, to the harness's onion service. Needs core-build.sh and tor-lib-build.sh to have run.
#
#   tor-driver-run.sh <onion-host.onion> <control-url>      e.g. http://172.27.16.1:18083 (forward.mjs makes that reachable)
set -euo pipefail
ONION="$1"; CONTROL="$2"
ROOT="${ROOT:-$HOME/aptroot}"; SR="$ROOT/sysroot"; WORK="$HOME/rv-linux"
export PATH="$SR/usr/bin:$HOME/.cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export LD_LIBRARY_PATH="$SR/usr/lib/x86_64-linux-gnu:$SR/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="$SR/usr/lib/x86_64-linux-gnu/pkgconfig:$SR/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SR"
export CMAKE_PREFIX_PATH="$SR/usr"
export CPLUS_INCLUDE_PATH="$SR/usr/include:$SR/usr/include/x86_64-linux-gnu"
export C_INCLUDE_PATH="$SR/usr/include:$SR/usr/include/x86_64-linux-gnu"
export LIBRARY_PATH="$SR/usr/lib/x86_64-linux-gnu"
LIBDIR="$WORK/build/tor-core-target/release"

DRIVER="$WORK/tor-core/poc/desktop-driver"
rm -rf "$DRIVER/build-linux"
cmake -S "$DRIVER" -B "$DRIVER/build-linux" -G Ninja -DCMAKE_BUILD_TYPE=Release -DRV_TOR_LIB_DIR="$LIBDIR" > /dev/null
cmake --build "$DRIVER/build-linux" 2>&1 | grep -E "error|FAILED|Linking CXX executable" | tail
STATE="$WORK/driver-state"; rm -rf "$STATE"; mkdir -p "$STATE"
echo "== running (real Tor, bootstrapping from scratch)"
time "$DRIVER/build-linux/tor_e2e_driver" --onion "$ONION" --state "$STATE" --control "$CONTROL"
