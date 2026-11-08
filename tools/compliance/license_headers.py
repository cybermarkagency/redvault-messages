#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems
"""
Puts the AGPL licence line and copyright line at the top of every source file
of the published apps (the folders tools/compliance/make_source_release.py
publishes). Never touches the proprietary server, third-party code (LiveKit's
protocol files, the Lucide icons, the Gradle wrapper), generated files or
data files (JSON, XML, images, fonts, Markdown).

  python tools/compliance/license_headers.py          # add missing headers
  python tools/compliance/license_headers.py --check  # list files without one, exit 1 if any

Standard library only. Keeps each file's line endings and UTF-8 BOM.
"""
import argparse, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_source_release import INCLUDE, EXCLUDED  # noqa: E402

SPDX = "SPDX-License-Identifier: AGPL-3.0-only"
COPYRIGHT = "Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems"

SLASH = {".kt", ".kts", ".java", ".cpp", ".hpp", ".h", ".c", ".rs", ".ts", ".tsx",
         ".js", ".mjs", ".mts", ".qml"}
HASH = {".py", ".sh", ".ps1", ".toml", ".yml", ".yaml", ".cmake"}
HASH_NAMES = {"CMakeLists.txt"}
BLOCK = {".css"}

# Not ours, or not a place for a notice.
SKIP_PREFIXES = (
    "desktop/third_party/",          # LiveKit (Apache-2.0), notice in THIRD_PARTY_NOTICES
    "desktop/app/icons/lucide/",     # Lucide icons (ISC)
    "android/gradle/wrapper/",       # Gradle wrapper (Apache-2.0)
    "android/gradlew",               # Gradle wrapper scripts (Apache-2.0)
)


def style(path: str):
    name = path.rsplit("/", 1)[-1]
    ext = "." + name.rsplit(".", 1)[-1] if "." in name else ""
    if name in HASH_NAMES or ext in HASH:
        return "#"
    if ext in SLASH:
        return "//"
    if ext in BLOCK:
        return "/*"
    return None


def published_files():
    files = subprocess.run(["git", "ls-files", "-z", "--", *INCLUDE], cwd=ROOT, check=True,
                           capture_output=True).stdout.decode().split("\0")
    excluded = tuple(p for p, _ in EXCLUDED)
    for f in files:
        if f and not f.startswith(excluded) and not f.startswith(SKIP_PREFIXES) and style(f):
            yield f


def has_header(text: str) -> bool:
    return "SPDX-License-Identifier:" in "\n".join(text.splitlines()[:8])


def add_header(raw: bytes, kind: str) -> bytes:
    bom = b"\xef\xbb\xbf" if raw.startswith(b"\xef\xbb\xbf") else b""
    text = raw[len(bom):].decode("utf-8")
    nl = "\r\n" if "\r\n" in text else "\n"
    if kind == "/*":
        header = f"/* {SPDX}{nl} * {COPYRIGHT} */{nl}"
    else:
        header = f"{kind} {SPDX}{nl}{kind} {COPYRIGHT}{nl}"
    lines = text.splitlines(keepends=True)
    keep = 0
    # A shebang (and Python's coding line) must stay first.
    if lines and lines[0].startswith("#!"):
        keep = 1
        if len(lines) > 1 and "coding" in lines[1] and lines[1].startswith("#"):
            keep = 2
    if keep and not lines[keep - 1].endswith(("\n", "\r")):
        lines[keep - 1] += nl
    body = "".join(lines[keep:])
    sep = nl if body and not body.startswith(("\n", "\r\n")) else ""
    return bom + ("".join(lines[:keep]) + header + sep + body).encode("utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    missing = []
    changed = 0
    for f in published_files():
        p = ROOT / f
        raw = p.read_bytes()
        try:
            text = raw.decode("utf-8-sig")
        except UnicodeDecodeError:
            print(f"skipped (not UTF-8): {f}", file=sys.stderr)
            continue
        if has_header(text):
            continue
        if args.check:
            missing.append(f)
        else:
            p.write_bytes(add_header(raw, style(f)))
            changed += 1
    if args.check:
        for f in missing:
            print(f"no licence header: {f}")
        print(f"{len(missing)} file(s) without a licence header")
        return 1 if missing else 0
    print(f"added a licence header to {changed} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
