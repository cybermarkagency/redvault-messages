# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

"""Leak check: every non-loopback address a test process talked to must be a
Tor relay listed in the consensus its gateway downloaded (or one of Arti's
built-in fallback directories, which are relays too).

python -I check_relays.py <consensus file> <connection log> [<fallback list>]

The connection log is what run-with-connection-log.ps1 (Windows) or the
LD_PRELOAD connect logger (Linux) wrote: lines "TCP|local|remote|port|state"
or "connect <ip> <port>". Prints a JSON report; exit 1 if any address isn't a relay.
"""
import ipaddress
import json
import sys


def relays_from_consensus(path):
    addresses = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            # microdesc consensus: r nick identity date time IP ORPort DirPort
            if parts[0] == "r" and len(parts) >= 8:
                addresses.add((parts[5], int(parts[6])))
            # extra OR addresses (IPv6): a [addr]:port
            elif parts[0] == "a" and len(parts) >= 2:
                host, _, port = parts[1].rpartition(":")
                addresses.add((host.strip("[]"), int(port)))
    return addresses


def fallbacks(path):
    """ "ip:port" strings in Arti's tor-dircommon data/fallback_dirs.rs."""
    import re
    found = set()
    with open(path, encoding="utf-8") as f:
        for host, port in re.findall(r'"\[?([0-9a-fA-F:.]+?)\]?:(\d+)"', f.read()):
            found.add((host, int(port)))
    return found


def connections(path):
    seen = []
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line.startswith("TCP|"):
                _, _local, remote, port, _state = line.split("|")
                seen.append((remote, int(port)))
            elif line.startswith("connect "):
                _, remote, port = line.split()[:3]
                seen.append((remote, int(port)))
    return seen


def main():
    consensus = relays_from_consensus(sys.argv[1])
    relay_ips = {ip for ip, _ in consensus}
    fallback = fallbacks(sys.argv[3]) if len(sys.argv) > 3 else set()
    report = {"consensusRelays": len(consensus), "fallbackDirs": len(fallback), "loopback": 0,
              "relays": [], "fallbackDirsContacted": [], "notRelays": []}
    for remote, port in sorted(set(connections(sys.argv[2]))):
        if remote in ("0.0.0.0", "::") or port == 0:
            continue
        ip = ipaddress.ip_address(remote)
        if ip.is_loopback:
            report["loopback"] += 1
        elif (remote, port) in consensus:
            report["relays"].append(f"{remote}:{port}")
        elif remote in relay_ips:
            report["relays"].append(f"{remote}:{port} (relay IP, other port)")
        elif (remote, port) in fallback:
            report["fallbackDirsContacted"].append(f"{remote}:{port}")
        else:
            report["notRelays"].append(f"{remote}:{port}")
    report["ok"] = not report["notRelays"]
    print(json.dumps(report, indent=1))
    sys.exit(0 if report["ok"] else 1)


if __name__ == "__main__":
    main()
