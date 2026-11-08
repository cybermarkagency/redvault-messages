#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# LiveKit with ONLY its built-in TURN (no external coturn), as production runs it (deploy/install_livekit.sh: turn.enabled,
# a domain, a UDP port), inside the netns lab's "srv" namespace (see netns-lab.sh), where its only address is PUBLIC_IP.
#
#   sudo ip netns exec srv livekit-turn-run.sh [no-turn]        (as root; stop with Ctrl-C / kill)
#   no-turn is the negative control: the same server with its TURN switched off
set -euo pipefail
PUBLIC_IP="${PUBLIC_IP:-198.51.100.10}"
HOME_DIR="${LK_HOME:-/home/winchester/livekit}"
TURN_ENABLED=true; [ "${1:-}" = "no-turn" ] && TURN_ENABLED=false
cat > "$HOME_DIR/turn-test.yaml" <<YAML
port: 7880
bind_addresses:
  - 0.0.0.0
rtc:
  tcp_port: 7881
  port_range_start: 50000
  port_range_end: 50100
  node_ip: $PUBLIC_IP
  use_external_ip: false
keys:
  devkeyredvault: 0123456789abcdef0123456789abcdef0123456789abcdef
room:
  auto_create: true
  empty_timeout: 60
  departure_timeout: 20
  max_participants: 32
turn:
  enabled: $TURN_ENABLED
  domain: $PUBLIC_IP
  udp_port: 3480
  relay_range_start: 50200
  relay_range_end: 50300
logging:
  level: debug
  pion_level: info
YAML
echo "LIVEKIT_URL=ws://$PUBLIC_IP:7880  (built-in TURN turn:$PUBLIC_IP:3480)" >&2
exec "$HOME_DIR/livekit-server" --config "$HOME_DIR/turn-test.yaml"
