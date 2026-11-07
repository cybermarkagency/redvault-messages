#!/usr/bin/env bash
# Samples the device's socket tables every half second until stopped, for the
# Android leak check: procnet_to_connlog.py then keeps the app's own UID and
# check_relays.py checks every remote end against the Tor consensus.
#
#   socket-sampler.sh <adb> <serial> <output file>
set -u
ADB="$1"; SERIAL="$2"; OUT="$3"
: > "$OUT"
while true; do
  "$ADB" -s "$SERIAL" shell 'for f in tcp tcp6 udp udp6; do echo "== $f"; cat /proc/net/$f; done' >> "$OUT" 2>/dev/null
  sleep 0.5
done
