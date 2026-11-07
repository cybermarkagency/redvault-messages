#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems
"""
Licence-policy checks for RedVault. Technical policy only: passing this is
NOT a legal sign-off.

  python tools/compliance/check.py            # development: hard policy only
  python tools/compliance/check.py --release  # also release-eligibility checks

Exit code 1 if any FAIL. Standard library only, no network.
"""
import argparse, hashlib, json, os, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AGPL_SHA256 = "0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0"
CLIENT_DIRS = ["ui", "desktop", "android", "crypto-core", "tor-core"]
# The exact upstream commits documented in docs/legal/LIBSIGNAL_MODIFICATIONS.md.
# Changing them is fine, but update that document (and the notices) in the same change.
PINS = {
    "libsignal-protocol": ("https://github.com/signalapp/libsignal", "850942581c06fa65c0323fd52029afa02fa1a1ba"),
    "spqr": ("https://github.com/signalapp/SparsePostQuantumRatchet.git", "06959b4708f9b7b1e94d0f8cc835f3958c077e94"),
}
NOTICES = {
    "web": ROOT / "ui/public/legal/THIRD_PARTY_NOTICES.txt",
    "desktop": ROOT / "desktop/app/legal/THIRD_PARTY_NOTICES.txt",
    "android": ROOT / "android/app/src/main/assets/legal/THIRD_PARTY_NOTICES.txt",
}
BUNDLED_LICENSE = [ROOT / "ui/public/legal/LICENSE.txt", ROOT / "desktop/app/legal/LICENSE.txt",
                   ROOT / "android/app/src/main/assets/legal/LICENSE.txt"]
COPYLEFT_STRONG = re.compile(r"\b(A?GPL)(-[0-9.]+)?(-only|-or-later|\+)?\b", re.I)

results: list[tuple[str, str, str]] = []


def record(level: str, name: str, detail: str) -> None:
    results.append((level, name, detail))


def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def check_licence_files() -> None:
    for d in CLIENT_DIRS:
        p = ROOT / d / "LICENSE"
        if not p.exists():
            record("FAIL", "licence file", f"{d}/LICENSE is missing (copy the canonical AGPLv3 text)")
        elif sha256(p) != AGPL_SHA256:
            record("FAIL", "licence file", f"{d}/LICENSE is not the canonical AGPLv3 text (sha256 differs); don't edit the licence text")
    for p in BUNDLED_LICENSE:
        if not p.exists() or sha256(p) != AGPL_SHA256:
            record("FAIL", "bundled licence", f"{p.relative_to(ROOT)} missing or not the canonical AGPLv3 text")
    if (ROOT / "LICENSE").exists() and (ROOT / "server").is_dir():
        record("FAIL", "licence scope", "a repository-root LICENSE would also cover server/, which stays proprietary; keep licences per client folder")
    record("PASS", "licence files", "AGPLv3 text present and unmodified in every client folder and app bundle")


def check_manifest_fields() -> None:
    for toml in ("crypto-core/Cargo.toml", "tor-core/Cargo.toml"):
        text = (ROOT / toml).read_text(encoding="utf-8")
        pkg = text.split("[lib]")[0]
        if not re.search(r'^license\s*=\s*"AGPL-3\.0-only"\s*$', pkg, re.M):
            record("FAIL", "licence field", f'{toml}: [package] needs license = "AGPL-3.0-only"')
    ui = json.loads((ROOT / "ui/package.json").read_text(encoding="utf-8"))
    if ui.get("license") != "AGPL-3.0-only":
        record("FAIL", "licence field", 'ui/package.json needs "license": "AGPL-3.0-only"')
    record("PASS", "licence fields", "crypto-core, tor-core and ui declare AGPL-3.0-only")


def lock_packages(lock: Path) -> list[dict]:
    text = lock.read_text(encoding="utf-8")
    return [dict(re.findall(r'^(\w+) = "([^"]*)"', block, re.M)) for block in text.split("[[package]]")[1:]]


def check_libsignal_pins() -> None:
    toml = (ROOT / "crypto-core/Cargo.toml").read_text(encoding="utf-8")
    if re.search(r"^\[(patch|replace)", toml, re.M):
        record("FAIL", "libsignal modification", "crypto-core/Cargo.toml has a [patch]/[replace] section: document any libsignal change in docs/legal/LIBSIGNAL_MODIFICATIONS.md and mark modified files (AGPL s5(a))")
    if (ROOT / "crypto-core/.cargo/config.toml").exists() and "replace-with" in (ROOT / "crypto-core/.cargo/config.toml").read_text(encoding="utf-8"):
        record("FAIL", "libsignal modification", "crypto-core/.cargo/config.toml replaces a source")
    pkgs = {p.get("name"): p for p in lock_packages(ROOT / "crypto-core/Cargo.lock")}
    for name, (repo, commit) in PINS.items():
        p = pkgs.get(name)
        src = (p or {}).get("source", "")
        if not p:
            record("FAIL", "libsignal pin", f"{name} not in crypto-core/Cargo.lock")
        elif not src.startswith(f"git+{repo}?") or not src.endswith(f"#{commit}"):
            record("FAIL", "libsignal pin", f"{name} source changed to {src!r}; expected {repo} @ {commit}. Update docs/legal/LIBSIGNAL_MODIFICATIONS.md, regenerate notices, then update PINS here")
    # Tracked files only (build folders and caches are not part of the source).
    import subprocess
    tracked = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True).stdout.splitlines()
    vendored = sorted({f.split("/libsignal")[0] + "/libsignal…" for f in tracked
                       if re.search(r"(^|/)(libsignal[^/]*|SparsePostQuantumRatchet|spqr)/", f)})
    if vendored:
        record("FAIL", "libsignal modification", f"vendored libsignal source tracked in the repository: {vendored}; document it in docs/legal/LIBSIGNAL_MODIFICATIONS.md")
    record("PASS", "libsignal pins", "libsignal and spqr are the documented, unmodified upstream commits")


def check_web_never_signal() -> None:
    lib = (ROOT / "crypto-core/src/lib.rs").read_text(encoding="utf-8")
    if not re.search(r'cfg\(all\(feature = "signal", feature = "wasm"\)\)\]\s*compile_error!', lib):
        record("FAIL", "web scope", "crypto-core/src/lib.rs lost the compile_error! that keeps libsignal out of the web build; the web scope analysis assumes it")
    web = (ROOT / "crypto-core/build-web.sh").read_text(encoding="utf-8")
    if re.search(r"--features[^\n]*signal", web):
        record("FAIL", "web scope", "crypto-core/build-web.sh enables `signal`")
    record("PASS", "web scope", "the web build cannot include libsignal")


def check_server_boundary() -> None:
    if not (ROOT / "server").is_dir():
        record("PASS", "server boundary", "no server/ in this tree (public source release): nothing to check")
        return
    lock = json.loads((ROOT / "server/package-lock.json").read_text(encoding="utf-8"))
    bad, unknown = [], []
    for path, info in lock.get("packages", {}).items():
        if not path or info.get("dev"):
            continue
        name = path.split("node_modules/")[-1]
        if name.startswith("@signalapp/") or "libsignal" in name:
            bad.append(f"{name} (Signal package)")
        lic = info.get("license")
        if lic is None:
            unknown.append(name)
        elif COPYLEFT_STRONG.search(str(lic)) and not re.search(r"\bLGPL", str(lic), re.I):
            bad.append(f"{name} ({lic})")
    if bad:
        record("FAIL", "server boundary", "server production dependencies must not be AGPL/GPL or Signal code (the server stays proprietary): " + ", ".join(bad))
    if unknown:
        record("WARN", "server boundary", f"{len(unknown)} server production packages have no licence in package-lock.json: " + ", ".join(unknown[:10]))
    hits = []
    for f in (ROOT / "server/src").rglob("*.ts"):
        text = f.read_text(encoding="utf-8", errors="replace")
        if re.search(r"""from\s+["'][^"']*(libsignal|@signalapp|crypto-core)""", text) or re.search(r"""require\(\s*["'][^"']*(libsignal|@signalapp)""", text):
            hits.append(str(f.relative_to(ROOT)))
    if hits:
        record("FAIL", "server boundary", "server code imports Signal or crypto-core code: " + ", ".join(hits))
    record("PASS", "server boundary", "no AGPL/GPL or Signal code in the server's production dependencies or imports")


def notices_summary(p: Path) -> list[str]:
    text = p.read_text(encoding="utf-8")
    m = re.search(r"\nSUMMARY\n=+\n(.*?)\n\n=+", text, re.S)
    return m.group(1).splitlines() if m else []


def check_notices(release: bool) -> None:
    lib = next((p for p in lock_packages(ROOT / "crypto-core/Cargo.lock") if p.get("name") == "libsignal-protocol"), {})
    for client, p in NOTICES.items():
        if not p.exists():
            record("FAIL", "third-party notices", f"{p.relative_to(ROOT)} missing: run tools/compliance/third_party_notices.py {client}")
            continue
        lines = notices_summary(p)
        if not lines:
            record("FAIL", "third-party notices", f"{p.relative_to(ROOT)} has no SUMMARY section")
            continue
        if client in ("desktop", "android") and not any(l.startswith(f"libsignal-protocol {lib.get('version')} ") for l in lines):
            record("FAIL", "third-party notices", f"{p.relative_to(ROOT)} doesn't list libsignal-protocol {lib.get('version')}: regenerate it")
        if "not affiliated with" not in p.read_text(encoding="utf-8"):
            record("FAIL", "third-party notices", f"{p.relative_to(ROOT)} lacks the non-endorsement line")
        unknown = [l for l in lines if "UNKNOWN" in l]
        if unknown:
            record("FAIL" if release else "WARN", "third-party notices", f"{client}: {len(unknown)} components with an unknown licence")
        proprietary = [l.split("  [")[0] for l in lines if "Android Software Development Kit License" in l]
        if proprietary:
            record("FAIL" if release else "WARN", "proprietary libraries",
                   f"{client}: {len(proprietary)} components under Google's proprietary Android SDK License "
                   f"(e.g. {', '.join(proprietary[:3])}): legal review required before an AGPL release")
    # Rust crates in the lockfile but not in the notices = notices out of date.
    pkgs = {(p.get("name"), p.get("version")) for p in lock_packages(ROOT / "crypto-core/Cargo.lock")}
    desk = NOTICES["desktop"]
    if desk.exists():
        listed = {tuple(l.split("  [")[0].rsplit(" ", 1)) for l in notices_summary(desk)}
        signal_crates = {("libsignal-protocol", lib.get("version")), ("spqr", next((p.get("version") for p in lock_packages(ROOT / "crypto-core/Cargo.lock") if p.get("name") == "spqr"), None))}
        missing = sorted(f"{n} {v}" for n, v in signal_crates if (n, v) not in listed and (n, v) in pkgs)
        if missing:
            record("FAIL", "third-party notices", f"desktop notices miss {missing}: regenerate")
    record("PASS", "third-party notices", "notices exist for web, desktop and Android, list libsignal and carry the non-endorsement line")


def check_source_offer(release: bool) -> None:
    for var in ("VITE_SOURCE_CODE_URL", "REDVAULT_SOURCE_CODE_URL", "REDVAULT_SOURCE_URL_FOR_CI"):
        v = os.environ.get(var, "")
        if v and not v.startswith("https://"):
            record("FAIL", "source offer", f"{var} must be an https URL (got {v!r})")
    if release and not os.environ.get("REDVAULT_SOURCE_URL_FOR_CI"):
        record("FAIL", "source offer", "release builds need the public source URL (REDVAULT_SOURCE_URL_FOR_CI); no approved public repository yet")
    for p in (ROOT / "desktop/packaging/collect-linux-sources.sh", ROOT / "tools/compliance/third_party_notices.py"):
        if not p.exists():
            record("FAIL", "source release tooling", f"{p.relative_to(ROOT)} missing")
    record("PASS", "source offer", "source URL settings are https or unset; source tooling present")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--release", action="store_true", help="also fail on release-eligibility gaps")
    a = ap.parse_args()
    for check in (check_licence_files, check_manifest_fields, check_libsignal_pins, check_web_never_signal, check_server_boundary):
        check()
    check_notices(a.release)
    check_source_offer(a.release)
    # A PASS line is only kept when its check recorded no FAIL.
    failed_checks = {n for lvl, n, _ in results if lvl == "FAIL"}
    shown = [(l, n, d) for l, n, d in results if not (l == "PASS" and any(f.startswith(n.split()[0]) for f in failed_checks))]
    for level, name, detail in shown:
        print(f"{level:4}  {name}: {detail}")
    mode = "release eligibility" if a.release else "development"
    fails = sum(1 for l, _, _ in results if l == "FAIL")
    print(f"\n{mode}: {fails} failure(s). Technical policy only, not a legal sign-off.")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
