# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

"""Turns /proc/net/{tcp,tcp6,udp,udp6} samples (from socket-sampler.sh) into the
connection log check_relays.py reads ("kind|local|remote|port|state"), keeping
only sockets owned by one Android app UID.

    python -I procnet_to_connlog.py <samples file> <uid> > connections.txt

Every distinct (kind, remote, port, state) seen while the app ran is listed
once; loopback and unconnected sockets are listed too, so the relay check can
count them.
"""
import ipaddress
import sys

STATES = {"01": "Established", "02": "SynSent", "03": "SynRecv", "04": "FinWait1", "05": "FinWait2", "06": "TimeWait",
          "07": "Close", "08": "CloseWait", "09": "LastAck", "0A": "Listen", "0B": "Closing"}


def addr(hexaddr):
    host, port = hexaddr.split(":")
    raw = bytes.fromhex(host)
    if len(raw) == 4:
        ip = ipaddress.IPv4Address(raw[::-1])
    else:
        # Four little-endian 32-bit words.
        words = b"".join(raw[i:i + 4][::-1] for i in range(0, 16, 4))
        ip = ipaddress.IPv6Address(words)
        if ip.ipv4_mapped:
            ip = ip.ipv4_mapped
    return str(ip), int(port, 16)


def main():
    path, uid = sys.argv[1], sys.argv[2]
    seen = {}
    kind = "TCP"
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("== "):
            kind = "UDP" if "udp" in line else "TCP"
            continue
        parts = line.split()
        if len(parts) < 10 or ":" not in parts[1] or parts[0] == "sl":
            continue
        if parts[7] != uid:
            continue
        local, _ = addr(parts[1])
        remote, port = addr(parts[2])
        state = STATES.get(parts[3], parts[3]) if kind == "TCP" else "Bound"
        seen[(kind, local, remote, port, state)] = True
    print("kind|local|remote|port|state")
    for kind, local, remote, port, state in seen:
        print(f"{kind}|{local}|{remote}|{port}|{state}")


if __name__ == "__main__":
    main()
