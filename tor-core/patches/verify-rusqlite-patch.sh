#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Proves the vendored tor-core/patches/rusqlite is exactly the published
# crates.io rusqlite 0.40.2 plus rusqlite-0.40.2-sqlite-3.54.diff (2 lines in
# Cargo.toml), and nothing else.
#
#   bash tor-core/patches/verify-rusqlite-patch.sh
#
# Needs: bash, curl, git (for `git apply`), sha256sum, tar, python. Downloads
# the published 0.40.2 source (about 200 KB) from static.crates.io, checks it
# against the checksum crates.io publishes, applies the diff to a scratch copy
# and compares that with the vendored crate (line endings ignored).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CRATE=rusqlite
VERSION=0.40.2
DIFF="$HERE/rusqlite-0.40.2-sqlite-3.54.diff"
UA="redvault-patch-check"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "1. published checksum for $CRATE $VERSION"
WANT="$(curl -fsS -A "$UA" "https://crates.io/api/v1/crates/$CRATE/$VERSION" | python -c 'import json,sys; print(json.load(sys.stdin)["version"]["checksum"])')"
curl -fsSL -A "$UA" -o "$WORK/upstream.crate" "https://static.crates.io/crates/$CRATE/$CRATE-$VERSION.crate"
GOT="$(sha256sum "$WORK/upstream.crate" | cut -d' ' -f1)"
echo "   crates.io: $WANT"
echo "   downloaded: $GOT"
[ "$WANT" = "$GOT" ] || { echo "FAIL: the download doesn't match crates.io's checksum"; exit 1; }

echo "2. apply the diff to the published source"
mkdir "$WORK/applied"
tar -xzf "$WORK/upstream.crate" -C "$WORK/applied" --strip-components=1
( cd "$WORK/applied" && git apply --ignore-whitespace "$DIFF" )

echo "3. compare with the vendored crate"
# Only PATCHED.md is extra here; crates.io's packaging files aren't vendored and don't affect the build.
IGNORE=(-x PATCHED.md -x .cargo_vcs_info.json -x .gitignore -x Cargo.lock -x Cargo.toml.orig -x .cargo-ok)
if diff -r --strip-trailing-cr "${IGNORE[@]}" "$WORK/applied" "$HERE/$CRATE" > "$WORK/difference.txt"; then
  echo "PASS: vendored $CRATE == crates.io $VERSION + the reviewed diff"
else
  cat "$WORK/difference.txt"
  echo "FAIL: the vendored crate differs from crates.io $VERSION plus the diff"
  exit 1
fi

echo "4. the diff touches only:"
grep -E '^\+\+\+ ' "$DIFF" | sed 's/^+++ b\//   /'
echo "   changed lines: $(grep -cE '^[-+][^-+]' "$DIFF")"
