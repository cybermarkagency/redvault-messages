#!/usr/bin/env python3
"""Writes a CycloneDX 1.5 SBOM for crypto-core, with the given cargo features.

Source of truth: `cargo tree` (normal and build dependencies, every target).
Git dependencies keep their pinned commit in the PURL's vcs_url qualifier.

Usage (from crypto-core/):
    python tools/make_sbom.py --features "signal ffi jni" --out ../docs/signal-integration/sbom-crypto-core-signal.cdx.json
"""
import argparse
import datetime
import json
import re
import subprocess
import sys
import uuid

LINE = re.compile(r"^(?P<name>\S+) v(?P<version>\S+)(?: \((?P<source>[^)]*)\))?$")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--features", default="")
    ap.add_argument("--out", required=True)
    ap.add_argument("--root", default="redvault-e2ee", help="the crate the SBOM describes (redvault-tor for tor-core)")
    args = ap.parse_args()

    cmd = ["cargo", "tree", "-e", "normal,build", "--target", "all", "--prefix", "none", "--no-dedupe", "--format", "{p}\t{l}\t{r}"]
    if args.features:
        cmd += ["--features", args.features]
    text = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout

    components = {}
    root = None
    for raw in text.splitlines():
        parts = raw.split("\t")
        if len(parts) != 3:
            continue
        pkg, licence, repo = parts
        pkg = pkg.replace(" (*)", "").replace(" (proc-macro)", "").strip()
        m = LINE.match(pkg)
        if not m:
            continue
        name, version, source = m["name"], m["version"], m["source"] or ""
        if name == args.root:
            root = (name, version)
            continue
        purl = f"pkg:cargo/{name}@{version}"
        if source.startswith("http"):
            # e.g. https://github.com/signalapp/libsignal?tag=v0.106.2#85094258
            purl += "?vcs_url=" + source.replace("#", "%23")
        comp = {"type": "library", "name": name, "version": version, "purl": purl}
        if licence:
            comp["licenses"] = [{"expression": licence.replace("/", " OR ")}]
        if repo:
            comp["externalReferences"] = [{"type": "vcs", "url": repo}]
        components[purl] = comp

    if root is None:
        print(f"{args.root} not found in cargo tree output", file=sys.stderr)
        return 1
    sbom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{uuid.uuid4()}",
        "version": 1,
        "metadata": {
            "timestamp": datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat(),
            "tools": [{"name": "crypto-core/tools/make_sbom.py"}],
            "component": {
                "type": "library",
                "name": root[0],
                "version": root[1],
                "description": f"crypto-core with features: {args.features or '(none)'}",
            },
        },
        "components": sorted(components.values(), key=lambda c: (c["name"], c["version"])),
    }
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(sbom, f, indent=2)
        f.write("\n")
    print(f"{len(components)} components -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
