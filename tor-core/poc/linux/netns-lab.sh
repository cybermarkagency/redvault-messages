#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# A small "internet" on this machine for LiveKit's built-in TURN, run as root in WSL (it only creates network namespaces
# and veth pairs inside this VM; `netns-lab.sh down` removes them):
#
#   ns srv   LiveKit, whose ONLY address is 198.51.100.10 (so it is both its real and its advertised address, as on a
#            server with a public IP on its interface; a 172.27.x socket advertised as 198.51.100.10 breaks the
#            TURN-relay -> SFU leg, because the SFU answers from the real address)
#   ns cli   the clients, at 198.51.100.2 (same subnet as srv) and, only for reaching the test API, 10.89.0.2
#   root     a forwarder on 10.89.0.1:18084 to the Windows-side API
#
# 198.51.100.0/24 (TEST-NET-2) is not private to LiveKit's TURN peer filter, which refuses RFC 1918 peers.
set -euo pipefail
if [ "${1:-up}" = "down" ]; then
  ip netns del srv 2>/dev/null || true; ip netns del cli 2>/dev/null || true; ip link del vr 2>/dev/null || true
  ip link del vs 2>/dev/null || true; ip addr del 198.51.100.10/32 dev lo 2>/dev/null || true
  echo "lab removed"; exit 0
fi
"$0" down >/dev/null
ip netns add srv; ip netns add cli
ip link add vs netns srv type veth peer name vc netns cli
ip -n srv link set lo up; ip -n srv addr add 198.51.100.10/24 dev vs; ip -n srv link set vs up
ip -n cli link set lo up; ip -n cli addr add 198.51.100.2/24 dev vc; ip -n cli link set vc up
ip link add vr type veth peer name vca netns cli
ip addr add 10.89.0.1/30 dev vr; ip link set vr up
ip -n cli addr add 10.89.0.2/30 dev vca; ip -n cli link set vca up
echo "lab up"; ip -n cli route | sed 's/^/  cli: /'
ip netns exec cli ping -c1 -W2 198.51.100.10 | tail -2
