#!/usr/bin/env bash
# What the WSL distro offers for building and running server-side test tools, without root.
. /etc/os-release; echo "os: $PRETTY_NAME  kernel: $(uname -r)"
for t in gcc g++ clang cmake ninja make pkg-config cargo rustc curl python3 pip3 patchelf dpkg apt-get nginx turnserver go; do
  printf '%-12s' "$t"; command -v "$t" || echo "MISSING"
done
for p in libsodium-dev libcurl4-openssl-dev libssl-dev nlohmann-json3-dev catch2 libqrencode-dev qt6-base-dev coturn libevent-2.1-7t64; do
  printf '%-26s' "$p"; (dpkg -s "$p" 2>/dev/null | grep -m1 '^Version' | cut -d' ' -f2) || true; echo
done
echo "apt lists present: $(ls /var/lib/apt/lists 2>/dev/null | wc -l)"
echo "sources:"; ls /etc/apt/sources.list.d/ 2>/dev/null; grep -hE '^(URIs|Suites|Components)' /etc/apt/sources.list.d/*.sources 2>/dev/null | head -6
echo "wsl.conf / networking:"; cat /etc/wsl.conf 2>/dev/null | head -5; ip -4 -o addr show eth0 2>/dev/null | awk '{print $4}'
