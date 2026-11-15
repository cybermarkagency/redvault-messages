# tor-core dependency audit (RustSec)

Last run: 2026-11-15, `cargo-audit` 0.22.2, RustSec advisory-db fetched the
same day (1509 advisories), against `tor-core/Cargo.lock` (Arti 0.47.0).

    cd tor-core && cargo audit

There is no `audit.toml` and no `--ignore`: every item below is either fixed
in `Cargo.lock` or still reported by `cargo audit` and explained here. Re-run
`cargo audit` whenever `Cargo.lock` changes or before a release, and update
this file.

## Before / after

Before (lock from release 0.8.0 / Android 0.7.0): 6 vulnerabilities, 4 warnings.
After: 2 vulnerabilities (both explained below, neither reachable in our
build), 2 "unmaintained" warnings.

| Advisory | Crate (before → after) | Pulled in by | Status |
|---|---|---|---|
| RUSTSEC-2026-0385 non-constant-time mask generation / `u128` compare | ctutils 0.4.3 → **0.4.8** | `digest` 0.11 → `blake2` → `hashx`/`equix` (`tor-hscrypto`, onion-service PoW) | **Fixed** (`cargo update`) |
| RUSTSEC-2026-0382 webpki may accept distrusted intermediates | rustls-webpki 0.103.15 → **0.103.17** | `rustls` ← `futures-rustls` ← `tor-rtcompat` (and our direct `rustls`) | **Fixed** (`cargo update`) |
| RUSTSEC-2026-0384 race in `tokio::time::Interval` (1.54.0 also yanked) | tokio 1.54.0 → **1.54.1** | our direct dep, `tor-rtcompat`, `async_executors` | **Fixed** (`cargo update`) |
| RUSTSEC-2026-0486 (unsound) xattr zero-length buffer | rustix 1.1.5 → **1.1.6** | `tempfile` ← `arti-client` | **Fixed** (`cargo update`) |
| RUSTSEC-2026-0459 / CVE-2026-119375 SQLite WAL uninitialized memory | libsqlite3-sys 0.38.2 (SQLite 3.53.2) → **0.39.1 (SQLite 3.54.1)** | `rusqlite` 0.40 ← `tor-dirmgr` (directory cache, `static-sqlite`) | **Fixed** by a 2-line patch of rusqlite, see below |
| RUSTSEC-2023-0071 Marvin attack (RSA private-key timing) | rsa 0.9.10 | `tor-llcrypto`, `tor-key-forge`, `ssh-key-fork-arti` | **Open, not exploitable here**, see below |
| RUSTSEC-2026-0387 crypto-bigint timing variability | crypto-bigint 0.5.5 | `elliptic-curve` ← `p256`/`p384`/`p521` ← `ssh-key-fork-arti` (optional ECDSA) | **Open, not compiled into any build**, see below |
| RUSTSEC-2025-0141 bincode unmaintained (warning) | bincode 2.0.1 | `typed-index-collections` ← `tor-netdir` (optional feature) | Not compiled, see below |
| RUSTSEC-2024-0436 paste unmaintained (warning) | paste 1.0.15 | `slotmap-careful` ← `tor-async-utils`, `tor-memquota` | Compile-time macro only, see below |

`crypto-core/` (separate lockfile) for the record: 0 vulnerabilities, 1
warning, RUSTSEC-2026-0173 `proc-macro-error2` 2.0.1 unmaintained (a
compile-time proc-macro helper; nothing runs at run time).

## Fixed by patch: libsqlite3-sys / SQLite (RUSTSEC-2026-0459)

`tor-dirmgr` 0.47 requires `rusqlite` 0.40, which requires `libsqlite3-sys`
0.38 (last affected release). The fixed `libsqlite3-sys` 0.39 is only used
by `rusqlite` 0.41, whose API changed, and Arti 0.48 (newest on 2026-11-15)
still pins `rusqlite` 0.40, so neither `cargo update` nor an Arti bump fixes
it. We vendor `rusqlite` 0.40.2 in `patches/rusqlite` with its two
`libsqlite3-sys` requirements raised to 0.39.1 (no source change; see
`patches/rusqlite/PATCHED.md`), wired in through `[patch.crates-io]`.
`libsqlite3-sys` 0.39.1 comes unmodified from crates.io and bundles SQLite
3.54.1. Check it with:

    bash tor-core/patches/verify-rusqlite-patch.sh

Drop the patch once our Arti pin depends on `rusqlite` >= 0.41.

## Open: rsa 0.9.10, Marvin attack (RUSTSEC-2023-0071)

No fix for 0.9: the fix is `rsa` 0.10 (published), but `tor-llcrypto`,
`tor-key-forge` and `ssh-key-fork-arti` 0.47 (and 0.48) all require `rsa`
0.9, and 0.10 is not API-compatible, so it can't be substituted without
forking Arti.

Why it is not exploitable in tor-core: the Marvin attack recovers an RSA
*private* key by timing many decryptions (or signatures) made with it.
tor-core is a Tor *client* only:

- Tor clients use RSA solely to *verify* (public-key operations) legacy relay
  identity and directory-authority signatures. Public-key operations handle
  no secret, so their timing leaks nothing.
- tor-core never creates or loads an RSA private key. The code that does
  (`tor-proto`'s relay channel, `tor-netdoc`/`tor-cert` encoders, RSA relay
  keys in `tor-key-forge`/`tor-keymgr`) is for running a relay; Arti's
  `relay` features are off in our build (`cargo tree -e features`).
  Our onion-service keys (client side, and the test-only `rv-onion-host`) are
  ed25519/x25519.
- Arti never performs RSA decryption at all.

Recheck when upgrading Arti; drop this entry once Arti moves to `rsa` 0.10.

## Open: crypto-bigint 0.5.5 (RUSTSEC-2026-0387)

Fixed only in 0.7.6+; there is no fixed 0.5.x. It is in `Cargo.lock` only
because Cargo records optional dependencies: it comes from `elliptic-curve`
0.13 ← `p256`/`p384`/`p521`, which `ssh-key-fork-arti` only uses for its
optional ECDSA keys, a feature nothing in our tree enables.
`cargo tree -i crypto-bigint@0.5.5 --all-features --target all` prints
nothing, and a full build (`cargo test --features ffi,test-hooks`, the
desktop and Android builds) produces no `crypto_bigint` artifact. Code that
isn't compiled can't leak timing. (`rsa` 0.9 uses `num-bigint-dig`, not
crypto-bigint.)

Recheck when upgrading Arti: if `cargo tree -i crypto-bigint` ever prints a
path, this becomes a real finding.

## Warnings (unmaintained, no known vulnerability)

- **bincode 2.0.1**: like crypto-bigint, in the lock only through an optional
  feature (`typed-index-collections`' serde/bincode support, not enabled by
  `tor-netdir`); never compiled.
- **paste 1.0.15**: a proc-macro used while compiling `slotmap-careful`; it
  runs only in the compiler and none of it ends up in our binaries. Arti
  would have to replace it.

## Verified with this lockfile (2026-11-15, Windows)

- `cargo test --features ffi,test-hooks`: 24 passed (16 unit, 6 gateway hardening, 2 saturating-time regression).
- Desktop `release` preset build + `ctest`: 732/732 passed.
- Android `:app:testDebugUnitTest`: 696/696 passed; `buildTorCore` rebuilt the library for arm64-v8a and x86_64.
- `tor_routing_check` against the real onion, from an empty state (new SQLite directory cache) and again warm: `"viaOnion":true` both times.
- `sealed_route_check --key <pinned key> --tries 2`: `"viaSealed":true` (twice). The dead onion port now fails within seconds, so the app's switch rule (3 failures or 30 s) was never reached in 2 tries; the check now switches on its last try at the latest (`switchAfterFailures = clamp(tries, 1, 3)`, the app itself unchanged).

## The other vendored patch: saturating-time 0.5.0

`patches/saturating-time` (Windows bootstrap hang, Arti #2678/#2726) is not a
RustSec item but is kept: Arti 0.47 needs saturating-time 0.5, and 0.5.0 is
still broken. Upstream **saturating-time 0.6.0 contains the same fix**
(`Some(st) if st != res` and `unwrap_or_else`), but only Arti 0.48 uses
0.6.0. Moving to Arti 0.48 would let us drop this patch (not the rusqlite
one). `bash tor-core/patches/verify-saturating-time-patch.sh` passes
(2026-11-15).
