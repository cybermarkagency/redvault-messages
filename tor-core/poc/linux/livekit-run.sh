#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# LiveKit (installed by livekit-wsl.sh) for the relay tests: signalling on the WSL address, media only via an
# external TURN (the static-credential coturn), because LiveKit's own TURN refuses private-address peers, which a
# private test network is made of (production has a public address, so its own TURN is used there).
set -euo pipefail
IP="$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)"
cat > "$HOME/livekit/test.yaml" <<YAML
port: 7880
bind_addresses:
  - 0.0.0.0
rtc:
  tcp_port: 7881
  port_range_start: 50000
  port_range_end: 50100
  node_ip: $IP
  use_external_ip: false
  turn_servers:
    - host: $IP
      port: 3490
      protocol: udp
      username: lkuser
      credential: lkpass
keys:
  devkeyredvault: 0123456789abcdef0123456789abcdef0123456789abcdef
room:
  auto_create: true
  empty_timeout: 60
  departure_timeout: 20
  max_participants: 32
logging:
  level: info
  pion_level: warn
YAML
echo "LIVEKIT_URL=ws://$IP:7880" >&2
exec "$HOME/livekit/livekit-server" --config "$HOME/livekit/test.yaml"
