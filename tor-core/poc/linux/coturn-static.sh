#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# A second coturn with a fixed username and password, for LiveKit's `rtc.turn_servers` (LiveKit can only hand out static
# credentials for an external TURN server). The one-to-one calls use coturn-run.sh's REST-secret instance instead.
set -euo pipefail
ROOT="${ROOT:-$HOME/aptroot}"
export LD_LIBRARY_PATH="$ROOT/sysroot/usr/lib/x86_64-linux-gnu:$ROOT/sysroot/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
IP="$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)"
exec "$ROOT/sysroot/usr/bin/turnserver" -n --no-cli --log-file=stdout --simple-log \
  --listening-ip="$IP" --listening-port=3490 --relay-ip="$IP" --min-port=49500 --max-port=49700 \
  --lt-cred-mech --user=lkuser:lkpass --realm=redvault.test --fingerprint \
  --no-tls --no-dtls --no-software-attribute --verbose
