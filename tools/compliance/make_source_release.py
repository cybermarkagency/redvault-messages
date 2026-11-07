#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems
"""
Builds the local, UNPUBLISHED source-release staging folder for the RedVault
client apps (AGPL-3.0-only Corresponding Source), plus SOURCE_MANIFEST.json
tying it to the commit and app versions it was made from.

  python tools/compliance/make_source_release.py --out C:/Users/user/Documents/redvault-clients-export

What goes in: tracked + new non-ignored files of the client folders (never
build output, node_modules or git-ignored secrets), the legal texts the apps
read, the Linux build workflow and the compliance tooling. What stays out, and
why, is in EXCLUDED below, with a reason for
each. It refuses to write into a non-empty folder unless it made that folder.
Nothing is uploaded anywhere.
"""
import argparse, datetime, hashlib, json, re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = ["ui", "desktop", "android", "crypto-core", "tor-core",
           "legal/terms-of-service.md", "legal/privacy-policy.md",
           ".github/workflows/desktop-linux.yml", ".github/workflows/license-compliance.yml",
           ".gitleaks.toml", ".gitleaksignore", "tools/compliance",
           # Public legal texts only; the internal legal analysis (scope, distribution review, audits,
           # compliance report) stays private.
           "docs/legal/THIRD_PARTY_NOTICES.md", "docs/legal/LIBSIGNAL_MODIFICATIONS.md", "docs/legal/sbom"]
# (path prefix, reason). Paths are repository-relative, forward slashes.
EXCLUDED = [
    ("android/app/google-services.json", "Firebase project config; the build skips Firebase without it, builders add their own"),
    ("android/app/local.properties", "developer machine path, tracked by mistake"),
    ("ui/src/test/groupKeysAndSignatures.test.ts", "test imports the proprietary server's code"),
    ("ui/src/test/shareEndToEnd.test.ts", "test starts the proprietary server in-process"),
    ("crypto-core/interop/server/", "dev tool that runs the proprietary server"),
    ("crypto-core/interop/server-run.mjs", "dev tool that runs the proprietary server"),
    ("tor-core/poc/results/", "test logs with connection lists, possibly the developer's own IP address"),
]
MARKER = ".redvault-source-release"


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout


def versions() -> dict:
    desk = re.search(r"project\(RedvaultDesktop VERSION ([0-9.]+)", (ROOT / "desktop/CMakeLists.txt").read_text(encoding="utf-8"))
    andr = re.search(r'versionName = "([^"]+)"', (ROOT / "android/app/build.gradle.kts").read_text(encoding="utf-8"))
    web = json.loads((ROOT / "ui/package.json").read_text(encoding="utf-8")).get("version")
    return {"desktop": desk and desk.group(1), "android": andr and andr.group(1), "web": web}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    out = a.out.resolve()
    if out.exists() and any(out.iterdir()) and not (out / MARKER).exists():
        sys.exit(f"{out} is not empty and wasn't made by this script; refusing to overwrite")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    (out / MARKER).write_text("made by tools/compliance/make_source_release.py\n", encoding="utf-8")

    files = [f for f in git("ls-files", "-co", "--exclude-standard", "--", *INCLUDE).splitlines()
             if not any(f == p or (p.endswith("/") and f.startswith(p)) for p, _ in EXCLUDED)]
    files = sorted(set(files))
    entries = []
    for rel in files:
        src = ROOT / rel
        if not src.is_file():
            continue
        dst = out / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        entries.append({"path": rel, "sha256": hashlib.sha256(src.read_bytes()).hexdigest(), "bytes": src.stat().st_size})
    # Repository-root files of the public source tree.
    for name in ("README.md", "NOTICE.md", "TRADEMARKS.md", "CONTRIBUTING.md"):
        shutil.copy2(ROOT / "tools/compliance/source-release" / name, out / name)
    shutil.copy2(ROOT / "crypto-core/LICENSE", out / "LICENSE")
    shutil.copy2(ROOT / "desktop/app/legal/THIRD_PARTY_NOTICES.txt", out / "THIRD_PARTY_NOTICES-desktop.txt")
    shutil.copy2(ROOT / "android/app/src/main/assets/legal/THIRD_PARTY_NOTICES.txt", out / "THIRD_PARTY_NOTICES-android.txt")
    shutil.copy2(ROOT / "ui/public/legal/THIRD_PARTY_NOTICES.txt", out / "THIRD_PARTY_NOTICES-web.txt")

    status = git("status", "--porcelain", "--", *INCLUDE)
    manifest = {
        "generated": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "repository_commit": git("rev-parse", "HEAD").strip(),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD").strip(),
        "uncommitted_changes_included": bool(status.strip()),
        "app_versions": versions(),
        "libsignal": "signalapp/libsignal v0.106.2 @ 850942581c06fa65c0323fd52029afa02fa1a1ba (unmodified)",
        "excluded": [{"path": p, "reason": r} for p, r in EXCLUDED],
        "not_included_but_required_to_build": [
            "Rust crates: crates.io + git, pinned by crypto-core/Cargo.lock and tor-core/Cargo.lock (a `cargo vendor` copy is attached to each release)",
            "npm packages: pinned by ui/package-lock.json",
            "Gradle/Maven artifacts: pinned versions in android/ build files",
            "MSYS2 packages for Windows (source packages attached to each release), Ubuntu 24.04 packages + Qt for Linux (collected per CI build)",
            "libdatachannel (FetchContent, pinned tag) and livekit-ffi (pinned version + SHA-256) fetched by CMake",
        ],
        "files": entries,
    }
    (out / "SOURCE_MANIFEST.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    print(f"{out}: {len(entries)} files, commit {manifest['repository_commit'][:10]}"
          f"{' + uncommitted changes' if manifest['uncommitted_changes_included'] else ''}, versions {manifest['app_versions']}")
    if manifest["uncommitted_changes_included"]:
        print("NOTE: includes uncommitted changes. For a real release, commit and tag first so the manifest names one exact commit.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
