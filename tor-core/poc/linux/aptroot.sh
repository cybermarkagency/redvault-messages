#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Installs Ubuntu packages and the dependencies this machine lacks into a private folder, WITHOUT root and
# without touching the system: apt works out what's missing (read-only), each .deb is downloaded from the
# Ubuntu archive and checked against the SHA-256 apt lists for it, then unpacked with dpkg -x.
#
#   aptroot.sh <package>...        (REFRESH=1 re-reads the package lists; ROOT=<dir> changes the folder)
#
# Afterwards:  export PATH=$ROOT/sysroot/usr/bin:$ROOT/sysroot/usr/sbin:$PATH
#              export LD_LIBRARY_PATH=$ROOT/sysroot/usr/lib/x86_64-linux-gnu
#              export PKG_CONFIG_PATH / CMAKE_PREFIX_PATH as needed (see sysroot-env.sh)
set -euo pipefail

ROOT="${ROOT:-$HOME/aptroot}"
mkdir -p "$ROOT/state/lists/partial" "$ROOT/cache/archives/partial" "$ROOT/debs" "$ROOT/sysroot"
APT=(-o "Dir::State=$ROOT/state" -o "Dir::Cache=$ROOT/cache" -o "Dir::State::status=/var/lib/dpkg/status" -o "Debug::NoLocking=1" -o "APT::Sandbox::User=")

if [ ! -f "$ROOT/.updated" ] || [ -n "${REFRESH:-}" ]; then
  apt-get "${APT[@]}" update -qq
  touch "$ROOT/.updated"
fi

# What would be downloaded for a real install: the packages not already installed, with their hashes.
apt-get "${APT[@]}" install --print-uris -qq -y --no-install-recommends "$@" > "$ROOT/uris.txt"
count=0
while IFS= read -r line; do
  url="$(echo "$line" | cut -d"'" -f2)"
  file="$(echo "$line" | cut -d' ' -f2)"
  hash="$(echo "$line" | awk '{print $4}')"
  [ -n "$url" ] || continue
  if [ ! -f "$ROOT/debs/$file" ]; then
    curl -fsSL -o "$ROOT/debs/$file" "$url"
  fi
  # Always SHA-256: from the --print-uris line when it has one, else from the package index (apt-cache show).
  case "$hash" in
    SHA256:*) want="${hash#SHA256:}" ;;
    *)
      name="${file%%_*}"
      want="$(apt-cache "${APT[@]}" show "$name" 2>/dev/null | python3 "$(dirname "$0")/pkgsha.py" "$file")"
      [ -n "$want" ] || { echo "no SHA-256 found for $file" >&2; exit 1; } ;;
  esac
  got="$(sha256sum "$ROOT/debs/$file" | cut -d' ' -f1)"
  [ "$got" = "$want" ] || { echo "HASH MISMATCH for $file" >&2; rm -f "$ROOT/debs/$file"; exit 1; }
  dpkg -x "$ROOT/debs/$file" "$ROOT/sysroot"
  count=$((count + 1))
  echo "  $file"
done < "$ROOT/uris.txt"
echo "unpacked $count packages into $ROOT/sysroot"

# A -dev package's libfoo.so points at the runtime library, which apt skips when the system already has it:
# the link then dangles in the sysroot and CMake falls back to the static .a. Point such links at the system copy.
fixed=0
for dir in "$ROOT/sysroot/usr/lib/x86_64-linux-gnu" "$ROOT/sysroot/usr/lib/x86_64-linux-gnu/mit-krb5" "$ROOT/sysroot/lib/x86_64-linux-gnu"; do
  [ -d "$dir" ] || continue
  while IFS= read -r link; do
    target="$(readlink "$link")"
    for sys in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu; do
      if [ -e "$sys/${target##*/}" ]; then ln -sfn "$sys/${target##*/}" "$link"; fixed=$((fixed + 1)); break; fi
    done
  done < <(find "$dir" -maxdepth 1 -xtype l)
done
echo "repointed $fixed dangling links at the system's runtime libraries"
