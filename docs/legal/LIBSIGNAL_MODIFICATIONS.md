# Modifications to libsignal

Date: 2026-11-07. Technical review, not legal advice.

## Result: unmodified

RedVault uses **unmodified** upstream releases:

| Project | Tag | Commit | How it's obtained |
|---|---|---|---|
| signalapp/libsignal | v0.106.2 | 850942581c06fa65c0323fd52029afa02fa1a1ba | Cargo git dependency (`crypto-core/Cargo.toml:41`), pinned by `Cargo.lock` |
| signalapp/SparsePostQuantumRatchet (`spqr`) | v1.6.0 | 06959b4708f9b7b1e94d0f8cc835f3958c077e94 | Transitive Cargo git dependency, pinned by `Cargo.lock` |

## Evidence

- No `[patch]` or `[replace]` section in `crypto-core/Cargo.toml`, and no
  `crypto-core/.cargo/config.toml` source replacement.
- No Signal source is vendored in the repository. The only copy is Cargo's
  checkout `~/.cargo/git/checkouts/libsignal-2a193a9867decbc4/8509425`:
  `git rev-parse HEAD` gives `850942581c06…` and `git status --porcelain`
  shows only Cargo's own marker file `.cargo-ok`, so no file is changed.
- `crypto-core/Cargo.lock` pins both repositories by commit. A modified copy
  would need a lockfile or source change, which the CI check
  `tools/compliance/check.py` refuses.
- The vendored Rust sources attached to each release (`cargo vendor --locked`)
  hold the same upstream files.

## What RedVault wrote around it (not modifications)

These are RedVault's own files that *use* libsignal through its public API.
They are not changes to libsignal:

| File | Added | Commit |
|---|---|---|
| `crypto-core/src/signal.rs`, `signal_state.rs` | 2026-11-07 | 1fad093 |
| `crypto-core/src/signal_group.rs` | 2026-11-07 | M6 commits (`git log -- crypto-core/src/signal_group.rs`) |
| `crypto-core/src/engine.rs` (`#[cfg(feature = "signal")]` blocks) | 2026-11-07 | 1fad093 onwards |
| Desktop wiring (`desktop/e2ev3/…`), Android wiring (`devicekeys/Signal.kt`) | 2026-11-07 | 7a10640 |

## Notices

Because nothing in libsignal is modified, no "modified file" notices
(AGPLv3 section 5(a)) are needed for it. Signal's own copyright notices and
licence ship unchanged in the crate sources, and the third-party notices
generated for the desktop and Android apps include libsignal's licence file.

If RedVault ever patches libsignal, the patch must be kept as a separate,
dated file (e.g. `crypto-core/patches/`), the changed files must carry a
prominent notice saying they were modified and when (section 5(a)), and this
document must be updated.
