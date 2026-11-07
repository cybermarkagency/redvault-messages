#!/usr/bin/env bash
# Runs the real coturn (installed with aptroot.sh) the way production does: REST-style credentials from a
# shared secret ("use-auth-secret": username "<expiry>:<random>", credential = base64 HMAC-SHA1(secret, username)).
# Test-only values: realm, ports, no TLS. Prints the address to put in TURN_URLS.
#
#   coturn-run.sh <secret-file> [stale-nonce-seconds]       (stop with Ctrl-C / kill)
#
# Production sets stale-nonce=600 (deploy/install_coturn.sh): after that long coturn asks an allocation's client to
# authenticate again, and expired REST credentials then fail. Without it an existing allocation keeps working past
# its credentials' expiry, so a credential-renewal test needs it (use a short value to test in minutes).
set -euo pipefail
ROOT="${ROOT:-$HOME/aptroot}"
export LD_LIBRARY_PATH="$ROOT/sysroot/usr/lib/x86_64-linux-gnu:$ROOT/sysroot/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
IP="$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)"
SECRET="$(cat "$1")"
STALE="${2:+--stale-nonce=$2}"
echo "TURN_URLS=turn:$IP:3478" >&2
exec "$ROOT/sysroot/usr/bin/turnserver" -n --no-cli --log-file=stdout --simple-log \
  --listening-ip="$IP" --listening-port=3478 --relay-ip="$IP" --min-port=49160 --max-port=49400 \
  --use-auth-secret --static-auth-secret="$SECRET" --realm=redvault.test --fingerprint \
  --no-tls --no-dtls --no-software-attribute --verbose $STALE
