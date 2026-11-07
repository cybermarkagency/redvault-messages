#!/usr/bin/env bash
# Installs LiveKit's server for Linux into ~/livekit (no root), checking the download against the SHA-256 listed in
# the same release's checksums.txt. Test use only.
set -euo pipefail
V="${LIVEKIT_VERSION:-1.13.10}"
D="$HOME/livekit"; mkdir -p "$D"; cd "$D"
base="https://github.com/livekit/livekit/releases/download/v$V"
curl -fsSL -o checksums.txt "$base/checksums.txt"
curl -fsSL -o lk.tar.gz "$base/livekit_${V}_linux_amd64.tar.gz"
want="$(grep "livekit_${V}_linux_amd64.tar.gz" checksums.txt | awk '{print $1}')"
got="$(sha256sum lk.tar.gz | awk '{print $1}')"
echo "listed:     $want"; echo "downloaded: $got"
[ "$want" = "$got" ] || { echo "CHECKSUM MISMATCH" >&2; exit 1; }
tar -xzf lk.tar.gz livekit-server
./livekit-server --version
