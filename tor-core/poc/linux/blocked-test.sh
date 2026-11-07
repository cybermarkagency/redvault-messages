#!/bin/bash
# Fail-closed test on Linux: the gateway is pointed at an unreachable directory
# (a censored/offline network). It must report "unavailable", refuse to carry
# the request, and the process must not connect anywhere but loopback.
# blocked-test.sh <onion host>
set -e
umask 077
ONION="$1"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN="$HOME/rv-tor-run/blocked"
rm -rf "$RUN"; mkdir -p "$RUN/state" "$RUN/cache"
chmod 700 "$HOME/rv-tor-run" "$RUN" "$RUN/state" "$RUN/cache"
gcc -shared -fPIC -O2 -o "$RUN/netlog.so" "$HERE/netlog.c" -ldl -lpthread
set +e
RV_NETLOG="$RUN/netlog.txt" LD_PRELOAD="$RUN/netlog.so" timeout 200 \
  "$HOME/rv-tor-target/debug/rv-tor-probe" --state "$RUN/state" --cache "$RUN/cache" \
  --target "$ONION:80" --wait 50 --get /api/conversations --blocked-network > "$RUN/probe.out" 2> "$RUN/probe.err"
echo "probe exit=$?"
set -e
cat "$RUN/probe.err" | cut -c1-300
cat "$RUN/probe.out"
touch "$RUN/netlog.txt"
echo "--- every connect() and lookup the process made:"
sort "$RUN/netlog.txt" | uniq -c
