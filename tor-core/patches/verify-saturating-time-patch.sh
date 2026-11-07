#!/usr/bin/env bash
# Proves the vendored tor-core/patches/saturating-time is exactly the published
# crates.io saturating-time 0.5.0 plus saturating-time-0.5.0-windows.diff, and
# nothing else, so a reviewer only has to read the diff (about 8 lines).
#
#   bash tor-core/patches/verify-saturating-time-patch.sh
#
# Needs: bash, curl, git (for `git apply`), sha256sum, tar, python. It downloads
# the published 0.5.0 source (about 9 KB) from static.crates.io, checks it
# against the checksum crates.io publishes, applies the diff to a scratch copy,
# and compares that with the vendored crate (line endings ignored; PATCHED.md
# and the notes are the only extra file).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
VERSION=0.5.0
UA="redvault-patch-check"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "1. published checksum for saturating-time $VERSION"
WANT="$(curl -fsS -A "$UA" "https://crates.io/api/v1/crates/saturating-time/$VERSION" | python -c 'import json,sys; print(json.load(sys.stdin)["version"]["checksum"])')"
curl -fsSL -A "$UA" -o "$WORK/upstream.crate" "https://static.crates.io/crates/saturating-time/saturating-time-$VERSION.crate"
GOT="$(sha256sum "$WORK/upstream.crate" | cut -d' ' -f1)"
echo "   crates.io: $WANT"
echo "   downloaded: $GOT"
[ "$WANT" = "$GOT" ] || { echo "FAIL: the download doesn't match crates.io's checksum"; exit 1; }

echo "2. apply the diff to the published source"
mkdir "$WORK/applied"
tar -xzf "$WORK/upstream.crate" -C "$WORK/applied" --strip-components=1
( cd "$WORK/applied" && git apply --ignore-whitespace "$HERE/saturating-time-0.5.0-windows.diff" )

echo "3. compare with the vendored crate"
# Only PATCHED.md is extra here. crates.io's four packaging files aren't vendored and don't affect the build.
# Everything else (Cargo.toml, README.md, src/) must match exactly.
IGNORE=(-x PATCHED.md -x .cargo_vcs_info.json -x .gitignore -x Cargo.lock -x Cargo.toml.orig)
if diff -r --strip-trailing-cr "${IGNORE[@]}" "$WORK/applied" "$HERE/saturating-time" > "$WORK/difference.txt"; then
  echo "PASS: vendored saturating-time == crates.io $VERSION + the reviewed diff"
else
  cat "$WORK/difference.txt"
  echo "FAIL: the vendored crate differs from crates.io $VERSION plus the diff"
  exit 1
fi

echo "4. the diff touches only:"
grep -E '^\+\+\+ ' "$HERE/saturating-time-0.5.0-windows.diff" | sed 's/^+++ b\//   /'
echo "   changed lines: $(grep -cE '^[-+][^-+]' "$HERE/saturating-time-0.5.0-windows.diff") (including comments)"
