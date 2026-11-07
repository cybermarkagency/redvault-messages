#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems
"""
Writes THIRD_PARTY_NOTICES.txt for one RedVault client: every third-party
component compiled into or shipped with that client, its version and licence,
followed by the licence and copyright files the component itself ships
(LICENSE*, COPYING*, NOTICE*, AUTHORS* where present).

Facts come from the lockfiles and installed packages, offline:
  - Rust: `cargo metadata --offline --locked` for crypto-core (and tor-core),
    walking normal (not build/dev) dependencies from the root for the
    features and target that client builds, proc-macros excluded.
  - Web: ui/package-lock.json production packages + their node_modules files.
  - Desktop (Windows): MSYS2 packages listed in --msys2-list, with
    /ucrt64/share/licenses/<package>/ from the MSYS2 install.
  - Android: Maven artifacts listed in --maven-list (group:artifact:version),
    licence from the POM in the Gradle cache; plus android/app/src/main/assets/licenses.
Anything whose licence can't be read is written as UNKNOWN, never guessed.

Usage (from the repository root):
  python tools/compliance/third_party_notices.py web
  python tools/compliance/third_party_notices.py desktop --msys2-list desktop/packaging/msys2-shipped-packages.txt
  python tools/compliance/third_party_notices.py android --maven-list android/maven-runtime-dependencies.txt [--fetch-poms]
Output: --out (default: the client's bundled legal folder, see OUTPUTS).
"""
import argparse, glob, json, os, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUTPUTS = {
    "web": ROOT / "ui/public/legal/THIRD_PARTY_NOTICES.txt",
    "desktop": ROOT / "desktop/app/legal/THIRD_PARTY_NOTICES.txt",
    "android": ROOT / "android/app/src/main/assets/legal/THIRD_PARTY_NOTICES.txt",
}
# (crate dir, features, target) per client: what each build compiles (see the
# client build files: crypto-core/build-web.sh, desktop/e2ev3 + desktop/tor
# CMakeLists, android/app/build.gradle.kts).
RUST = {
    "web": [("crypto-core", "wasm", "wasm32-unknown-unknown")],
    "desktop": [("crypto-core", "ffi,signal", "x86_64-pc-windows-gnu"), ("tor-core", "ffi", "x86_64-pc-windows-gnu")],
    "android": [("crypto-core", "jni,signal", "aarch64-linux-android"), ("tor-core", "jni", "aarch64-linux-android")],
}
LICENSE_FILE = re.compile(r"^(LICEN[CS]E|COPYING|NOTICE|AUTHORS|COPYRIGHT|UNLICENSE)([-._].*)?$", re.I)
OWN_CRATES = {"redvault-e2ee", "redvault-tor"}
MAX_FILE = 200_000


def licence_files(directory: Path) -> list[Path]:
    if not directory.is_dir():
        return []
    return sorted(p for p in directory.iterdir() if p.is_file() and LICENSE_FILE.match(p.name))


def read_text(p: Path) -> str:
    data = p.read_bytes()[:MAX_FILE]
    return data.decode("utf-8", errors="replace").replace("\r\n", "\n").strip()


def rust_components(crate: str, features: str, target: str) -> list[dict]:
    cmd = ["cargo", "metadata", "--offline", "--locked", "--format-version", "1",
           "--filter-platform", target, "--features", features]
    meta = json.loads(subprocess.run(cmd, cwd=ROOT / crate, check=True, capture_output=True, text=True).stdout)
    pkgs = {p["id"]: p for p in meta["packages"]}
    nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
    seen, stack, out = set(), [meta["resolve"]["root"]], []
    while stack:
        pid = stack.pop()
        if pid in seen:
            continue
        seen.add(pid)
        pkg = pkgs[pid]
        if any("proc-macro" in t["kind"] for t in pkg["targets"]):
            continue
        if pkg["name"] not in OWN_CRATES:
            out.append({
                "name": pkg["name"], "version": pkg["version"],
                "licence": pkg.get("license") or ("file: " + pkg["license_file"] if pkg.get("license_file") else "UNKNOWN"),
                "source": (pkg.get("source") or "path").split("#")[0],
                "files": licence_files(Path(pkg["manifest_path"]).parent),
            })
        for dep in nodes[pid]["deps"]:
            if any(k["kind"] is None for k in dep["dep_kinds"]):
                stack.append(dep["pkg"])
    return out


def npm_components() -> list[dict]:
    lock = json.loads((ROOT / "ui/package-lock.json").read_text(encoding="utf-8"))
    out = []
    for path, info in lock["packages"].items():
        if not path or info.get("dev") or info.get("devOptional") or info.get("link"):
            continue
        name = path.split("node_modules/")[-1]
        directory = ROOT / "ui" / path
        lic = info.get("license")
        if not lic and (directory / "package.json").exists():
            lic = json.loads((directory / "package.json").read_text(encoding="utf-8")).get("license")
        out.append({"name": name, "version": info.get("version", "?"), "licence": lic or "UNKNOWN",
                    "source": "npm", "files": licence_files(directory)})
    return out


def msys2_root() -> Path:
    for c in (os.environ.get("MSYS2_ROOT"), "C:/msys64"):
        if c and Path(c).is_dir():
            return Path(c)
    sys.exit("MSYS2 not found (set MSYS2_ROOT)")


def msys2_components(list_file: Path) -> list[dict]:
    root, out = msys2_root(), []
    for line in list_file.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name = line.split()[0]
        short = name.removeprefix("mingw-w64-ucrt-x86_64-")
        desc = next(iter(glob.glob(str(root / "ucrt64/../var/lib/pacman/local" / f"{name}-[0-9]*" / "desc"))), None)
        version, licence = "?", "UNKNOWN"
        if desc:
            fields = Path(desc).read_text(encoding="utf-8").split("\n\n")
            for f in fields:
                head, _, body = f.strip().partition("\n")
                if head == "%VERSION%":
                    version = body.strip()
                elif head == "%LICENSE%":
                    licence = " AND ".join(x.strip().removeprefix("spdx:") for x in body.splitlines() if x.strip())
        files = sorted(p for p in (root / "ucrt64/share/licenses" / short).glob("**/*") if p.is_file())
        out.append({"name": short, "version": version, "licence": licence, "source": "MSYS2 " + name, "files": files})
    return out


MAVEN_REPOS = ("https://dl.google.com/dl/android/maven2", "https://repo1.maven.org/maven2")
POM_CACHE = ROOT / "android/build/compliance-poms"


def pom_text(group: str, artifact: str, version: str, fetch: bool) -> str | None:
    """The POM from the Gradle cache, our POM cache, or (with --fetch-poms) Google Maven / Maven Central."""
    gradle = Path.home() / ".gradle/caches/modules-2/files-2.1"
    found = glob.glob(str(gradle / group / artifact / version / "*" / f"{artifact}-{version}.pom"))
    if found:
        return Path(found[0]).read_text(encoding="utf-8", errors="replace")
    cached = POM_CACHE / f"{group}__{artifact}__{version}.pom"
    if cached.exists():
        return cached.read_text(encoding="utf-8", errors="replace")
    if not fetch:
        return None
    import urllib.request
    rel = f"{group.replace('.', '/')}/{artifact}/{version}/{artifact}-{version}.pom"
    for repo in MAVEN_REPOS:
        try:
            with urllib.request.urlopen(f"{repo}/{rel}", timeout=20) as r:
                text = r.read(1_000_000).decode("utf-8", errors="replace")
            POM_CACHE.mkdir(parents=True, exist_ok=True)
            cached.write_text(text, encoding="utf-8")
            return text
        except Exception:
            continue
    return None


def pom_licences(group: str, artifact: str, version: str, fetch: bool, depth: int = 0) -> list[str]:
    text = pom_text(group, artifact, version, fetch)
    if text is None:
        return []
    names = [n.strip() for n in re.findall(r"<license>\s*<name>([^<]+)</name>", text)]
    if names or depth >= 3:
        return names
    parent = re.search(r"<parent>(.*?)</parent>", text, re.S)
    if parent:
        g = re.search(r"<groupId>([^<]+)</groupId>", parent.group(1))
        a = re.search(r"<artifactId>([^<]+)</artifactId>", parent.group(1))
        v = re.search(r"<version>([^<]+)</version>", parent.group(1))
        if g and a and v:
            return pom_licences(g.group(1).strip(), a.group(1).strip(), v.group(1).strip(), fetch, depth + 1)
    return []


def maven_components(list_file: Path, fetch: bool) -> list[dict]:
    out = []
    for line in list_file.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        group, artifact, version = line.split(":")[:3]
        names = pom_licences(group, artifact, version, fetch)
        out.append({"name": f"{group}:{artifact}", "version": version,
                    "licence": "; ".join(names) or "UNKNOWN (no licence in the POM or its parents)",
                    "source": "Maven", "files": []})
    extra = ROOT / "android/app/src/main/assets/licenses"
    for p in sorted(extra.glob("*")):
        out.append({"name": p.stem, "version": "", "licence": "see text", "source": "bundled asset", "files": [p]})
    return out


def write(client: str, components: list[dict], out: Path) -> None:
    seen, unique = set(), []
    for c in sorted(components, key=lambda c: (c["name"].lower(), c["version"])):
        key = (c["name"], c["version"])
        if key not in seen:
            seen.add(key)
            unique.append(c)
    unknown = [c for c in unique if "UNKNOWN" in c["licence"]]
    lines = [
        f"Third-party software in RedVault Messages ({client})",
        "",
        "RedVault Messages is free software under the GNU Affero General Public",
        "License, version 3 only (see LICENSE). It includes the third-party",
        "components below, each under its own licence. Generated by",
        "tools/compliance/third_party_notices.py from the lockfiles and installed",
        "packages; regenerate it for every release.",
        "",
        "RedVault Messages is not affiliated with, sponsored or endorsed by Signal",
        "Messenger LLC. It uses libsignal under the AGPL-3.0-only licence.",
        "",
        f"{len(unique)} components; {len(unknown)} with a licence that could not be read from local files.",
        "",
        "=" * 78,
        "SUMMARY",
        "=" * 78,
    ]
    for c in unique:
        lines.append(f"{c['name']} {c['version']}  [{c['licence']}]  ({c['source']})")
    lines += ["", "=" * 78, "LICENCE AND COPYRIGHT TEXTS", "=" * 78]
    for c in unique:
        lines += ["", "-" * 78, f"{c['name']} {c['version']}  [{c['licence']}]", "-" * 78]
        if not c["files"]:
            lines.append("(No licence file shipped with this component; its declared licence is shown above.)")
        for f in c["files"]:
            lines += [f"--- {f.name}", read_text(f)]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{out.relative_to(ROOT)}: {len(unique)} components, {len(unknown)} unknown licences")
    for c in unknown:
        print(f"  UNKNOWN: {c['name']} {c['version']}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("client", choices=sorted(OUTPUTS))
    ap.add_argument("--msys2-list", type=Path)
    ap.add_argument("--maven-list", type=Path)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--fetch-poms", action="store_true", help="android: download missing POMs (licence metadata only) from Google Maven / Maven Central")
    a = ap.parse_args()
    comps = [c for crate, feats, target in RUST[a.client] for c in rust_components(crate, feats, target)]
    if a.client == "web":
        comps += npm_components()
    elif a.client == "desktop":
        if not a.msys2_list:
            sys.exit("desktop needs --msys2-list")
        comps += msys2_components(a.msys2_list)
    elif a.client == "android":
        if not a.maven_list:
            sys.exit("android needs --maven-list")
        comps += maven_components(a.maven_list, a.fetch_poms)
    write(a.client, comps, a.out or OUTPUTS[a.client])


if __name__ == "__main__":
    main()
