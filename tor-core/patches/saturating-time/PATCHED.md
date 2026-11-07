# saturating-time 0.5.0, patched (needs your review before any release build)

Upstream: https://crates.io/crates/saturating-time/0.5.0 (MIT OR Apache-2.0,
published by the Arti project). Every Arti 0.47 crate depends on it.

## The bug (reported upstream, not fixed as of 2026-10-20)

- Arti issue [#2678](https://gitlab.torproject.org/tpo/core/arti/-/issues/2678)
  "Windows: bootstrap hangs at 15% in saturating-time::find_limit while parsing
  consensus" (opened 2026-08-25, open, last activity 2026-09-15)
- Arti issue [#2726](https://gitlab.torproject.org/tpo/core/arti/-/issues/2726)
  "Windows 11 bootstrap hangs at 15% due to `saturating-time` time-limit loop"
  (opened 2026-09-15, open, assigned to a maintainer, last activity 2026-10-13)
- Upstream also has #2728 "Import `saturating-time` into tree" (opened
  2026-09-17, open), so a fix may arrive as part of Arti itself.

`find_limit` halves a step down to 1 ns and stops only when `checked_sub` /
`checked_add` returns `None`. Windows `SystemTime` counts 100 ns intervals, so
a step under 100 ns returns `Some` with the same value and the loop never ends.
`saturating_sub` evaluated `min_value()` eagerly (`unwrap_or`), so even an
ordinary subtraction runs that search. Arti hits it while parsing the
consensus: a stock Arti 0.47 client never finishes bootstrapping on Windows
(reproduced here 2026-10-20; Linux is unaffected).

## The patch: 8 changed lines, the same fix both upstream reports propose

See `../saturating-time-0.5.0-windows.diff` (a `diff -ru` against the
crates.io source):

1. `find_limit`: `Some(st) if st != res` - a step that made no progress counts
   as `None` (the trait gains a `PartialEq` bound; every implementor already has it).
2. `saturating_add` / `saturating_sub`: `unwrap_or_else(...)` instead of
   `unwrap_or(...)`, so the limit is only computed when saturation is needed.

Nothing else in the crate is changed. It is wired in through
`[patch.crates-io]` in `tor-core/Cargo.toml`.

## Tests

| Test | Windows | Linux |
|---|---|---|
| `tor-core/tests/saturating_time_regression.rs` (the upstream reproducer, with a 10 s timeout; fails on Windows if the patch is dropped) | 2/2 pass | 2/2 pass |
| The crate's own tests (`cargo test -p saturating-time`) | 4/6: two upstream tests assume 1 ns resolution / a `Duration::MAX` span, which Windows doesn't have; their own comment flags one as "might not be so portable". Unpatched they hang instead of failing. | 6/6 unit + 8/8 doc tests pass |
| Arti bootstraps and reaches an onion service | yes (PoC runs, see docs/tor-routing-plan.md) | yes |

## How to review it (about 5 minutes)

1. Read `../saturating-time-0.5.0-windows.diff`: two files, 13 changed lines
   counting the explanatory comment.
2. Run `bash ../verify-saturating-time-patch.sh`. It downloads the published
   0.5.0 source, checks it against the checksum crates.io publishes, applies the
   diff, and fails unless the result is identical to this directory (PATCHED.md
   excepted; crates.io's four packaging files are not vendored). So the diff in
   step 1 is the *whole* change. Last run 2026-10-20: PASS
   (sha256 `a586db15...49a65`).
3. Run `cargo test --test saturating_time_regression` in `tor-core/` (2/2 pass;
   without the patch it fails on Windows within its 10 s timeout).

## Status checked 2026-10-20 (the day it was integrated)

- **No fixed release.** crates.io has 0.5.0 as the newest saturating-time and
  Arti 0.47.0 as the newest arti-client. In the Arti repository nothing touched
  `crates/saturating-time` after the release except changelog maintenance, and no
  merge request mentions it.
- **Downgrading doesn't avoid it.** 0.3.0 and 0.4.0 use the same search
  (identical `find_limit` apart from a time-source call and a semicolon), which
  matches issue #2678 being filed before 0.5.0 existed. There is no published
  version to pin to instead of patching.
- Re-check before each production build: `cargo update -p saturating-time --dry-run`
  and the three issues above. A fixed release makes this directory unnecessary.

## Review status and removal

- Reviewed by: **pending your review** (steps 1-3 above make it quick).
- Upstream: issues filed by others; no fix released. Posting a comment or a
  merge request upstream is an outward-facing action left to you. The wording
  for one: the diff above, and "`find_limit` assumes `checked_add/sub` with a
  1 ns step eventually returns `None`; where the clock's resolution is coarser
  (Windows `SystemTime`: 100 ns) it returns the same value forever. Treating a
  step with no progress as `None` fixes it; `saturating_add/sub` should also
  compute the limit lazily (`unwrap_or_else`)."
- Remove this directory and the `[patch.crates-io]` entry as soon as an Arti
  release depends on a fixed saturating-time (or imports it in-tree with the
  fix). `cargo build` warns "Patch `saturating-time` was not used" when Arti
  moves to a different version: treat that as the signal to re-check, and keep
  the regression test.
