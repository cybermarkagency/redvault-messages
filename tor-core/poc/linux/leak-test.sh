#!/bin/bash
# Linux leak test (inside WSL Ubuntu): runs rv-tor-probe under the netlog.so
# shim and checks every connect() against the consensus the gateway used.
#
# leak-test.sh <name> <onion host> [--blocked-network]
# Output: ~/rv-tor-run/<name>/{probe.out,probe.err,netlog.txt,relaycheck.json}
set -e
umask 077
NAME="$1"; ONION="$2"; shift 2
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CRATE="$(cd "$HERE/../.." && pwd)"
RUN="$HOME/rv-tor-run/$NAME"
rm -rf "$RUN"; mkdir -p "$RUN/state" "$RUN/cache"
chmod 700 "$HOME/rv-tor-run" "$RUN" "$RUN/state" "$RUN/cache"
gcc -shared -fPIC -O2 -o "$RUN/netlog.so" "$HERE/netlog.c" -ldl -lpthread

set +e
RV_NETLOG="$RUN/netlog.txt" LD_PRELOAD="$RUN/netlog.so" \
  "$HOME/rv-tor-target/debug/rv-tor-probe" --state "$RUN/state" --cache "$RUN/cache" \
  --target "$ONION:80" --wait 150 --get /health --get /api/auth/me --get /api/conversations "$@" \
  > "$RUN/probe.out" 2> "$RUN/probe.err"
echo "probe exit=$?"
set -e
cat "$RUN/probe.out"
tail -1 "$RUN/probe.err" | cut -c1-400
echo "--- netlog summary"
touch "$RUN/netlog.txt"
echo "connects: $(grep -c '^connect ' "$RUN/netlog.txt" || true)"
echo "name lookups: $(grep -c '^getaddrinfo ' "$RUN/netlog.txt" || true)"
echo "dns datagrams: $(grep -c '^dns-sendto ' "$RUN/netlog.txt" || true)"
echo "ipv6 connects: $(grep '^connect ' "$RUN/netlog.txt" | grep -c ':' || true)"
grep '^getaddrinfo\|^dns-sendto' "$RUN/netlog.txt" | sort | uniq -c | head || true
CONSENSUS="$(ls "$RUN"/cache/dir_blobs/con_microdesc_* 2>/dev/null | head -1 || true)"
FALLBACKS="$(ls -d "$HOME"/.cargo/registry/src/*/tor-dircommon-0.47.0/data/fallback_dirs.rs | head -1)"
if [ -n "$CONSENSUS" ]; then
  python3 -I "$HERE/../check_relays.py" "$CONSENSUS" "$RUN/netlog.txt" "$FALLBACKS" | tee "$RUN/relaycheck.json"
else
  echo "no consensus downloaded; connects:"; grep '^connect ' "$RUN/netlog.txt" | sort | uniq -c
fi
