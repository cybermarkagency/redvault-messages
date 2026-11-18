#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems
#
# Builds the Tor Project's pluggable-transport programs that bridges need:
#   lyrebird          obfs4 and WebTunnel  (BSD-2-Clause)
#   snowflake-client  Snowflake            (BSD-3-Clause)
# from source, at pinned versions, through Go's module mirror (checksums are
# verified against sum.golang.org, so a tampered download fails the build).
#
# Usage: build-transports.sh TARGET OUT_DIR
#   TARGET: windows-amd64 | linux-amd64 | android-arm64 | android-arm | android-x86_64
#   Android targets need ANDROID_NDK_HOME (CGO through the NDK's clang, so the
#   programs use Android's own DNS resolver).
#
# Output names: lyrebird[.exe] and snowflake-client[.exe]; for Android
# liblyrebird.so and libsnowflake.so (the only way Android lets an app run a
# program it ships is from its native-library folder).
set -euo pipefail

LYREBIRD_MODULE=gitlab.torproject.org/tpo/anti-censorship/pluggable-transports/lyrebird
# No semver tags on the module mirror: pinned to main at 59ac7e40 (2026-11-04).
LYREBIRD_VERSION=v0.0.0-20261104192811-59ac7e400c38
SNOWFLAKE_MODULE=gitlab.torproject.org/tpo/anti-censorship/pluggable-transports/snowflake/v2
SNOWFLAKE_VERSION=v2.15.3

target=${1:?target}
out=${2:?out dir}
mkdir -p "$out"
out=$(cd "$out" && pwd)

export GOFLAGS=-trimpath
export GOPROXY=https://proxy.golang.org
export GONOSUMDB= GONOSUMCHECK= GOSUMDB=sum.golang.org
ldflags="-s -w -buildid="
exe=""
case "$target" in
  # -H=windowsgui: no console window pops up when the desktop app starts them
  # (Arti launches them without hiding one); stdin/stdout still work as pipes.
  windows-amd64) export GOOS=windows GOARCH=amd64 CGO_ENABLED=0; exe=.exe; ldflags="$ldflags -H=windowsgui" ;;
  linux-amd64)   export GOOS=linux GOARCH=amd64 CGO_ENABLED=0 ;;
  android-*)
    : "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}"
    api=26
    host=$(ls "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" | head -1)
    bin="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$host/bin"
    case "$target" in
      android-arm64)  export GOARCH=arm64; cc=aarch64-linux-android$api-clang ;;
      android-arm)    export GOARCH=arm GOARM=7; cc=armv7a-linux-androideabi$api-clang ;;
      android-x86_64) export GOARCH=amd64; cc=x86_64-linux-android$api-clang ;;
      *) echo "unknown target $target" >&2; exit 2 ;;
    esac
    [ -e "$bin/$cc" ] || [ -e "$bin/$cc.cmd" ] || { echo "no $cc in $bin" >&2; exit 2; }
    [ -e "$bin/$cc.cmd" ] && cc="$cc.cmd"
    export GOOS=android CGO_ENABLED=1 CC="$bin/$cc"
    # github.com/wlynxg/anet (Android network interfaces) links to a Go internal;
    # Go 1.23+ refuses that unless told otherwise, as Tor Browser's Android build does.
    # 16 KB pages, which Google Play requires.
    ldflags="$ldflags -checklinkname=0 -extldflags=-Wl,-z,max-page-size=16384"
    ;;
  *) echo "unknown target $target" >&2; exit 2 ;;
esac

# Each program builds from its own module, with the dependency versions its
# authors pinned in their go.mod/go.sum (not a merged set).
src() { go mod download -json "$1@$2" | grep '"Dir"' | cut -d'"' -f4 | sed 's#\\\\#/#g'; }
ly_dir=$(src "$LYREBIRD_MODULE" "$LYREBIRD_VERSION")
sf_dir=$(src "$SNOWFLAKE_MODULE" "$SNOWFLAKE_VERSION")
[ -n "$ly_dir" ] && [ -n "$sf_dir" ] || { echo "module download failed" >&2; exit 1; }

case "$target" in
  android-*) ly="$out/liblyrebird.so"; sf="$out/libsnowflake.so" ;;
  *)         ly="$out/lyrebird$exe";   sf="$out/snowflake-client$exe" ;;
esac
(cd "$ly_dir" && go build -mod=readonly -ldflags "$ldflags" -o "$ly" ./cmd/lyrebird)
(cd "$sf_dir" && go build -mod=readonly -ldflags "$ldflags" -o "$sf" ./client)
ls -l "$ly" "$sf"
