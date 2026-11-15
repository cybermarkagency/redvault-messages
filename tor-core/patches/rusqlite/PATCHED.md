# rusqlite 0.40.2, patched to use libsqlite3-sys 0.39.1 (SQLite 3.54.1)

Upstream: https://crates.io/crates/rusqlite/0.40.2 (MIT, rusqlite project).
Arti 0.47's `tor-dirmgr` (its directory cache) needs `rusqlite` 0.40, and
we build it with `static-sqlite`, so SQLite is compiled in from
`libsqlite3-sys`'s bundled source.

## Why

RUSTSEC-2026-0459 / CVE-2026-119375: SQLite before 3.54.0 can return
uninitialized page-cache memory when a database is opened together with a
crafted `-wal` file whose page size differs. `libsqlite3-sys` 0.38.2 (the
version `rusqlite` 0.40.2 asks for) bundles SQLite 3.53.2 and is affected;
0.39.0+ bundles 3.54.x and is fixed. `rusqlite` 0.41 moved to 0.39, but it
also changes its API, and `tor-dirmgr` 0.47 (and 0.48, the newest Arti on
2026-11-15) still require `rusqlite` 0.40, so we can't simply upgrade.

## The patch: 2 changed lines, in Cargo.toml only

See `../rusqlite-0.40.2-sqlite-3.54.diff` (a `diff -u` against the crates.io
source): the two `libsqlite3-sys` requirements (native and wasm targets) go
from `0.38.x` to `0.39.1`. No Rust source changes. `libsqlite3-sys` 0.39.1
itself comes unmodified from crates.io (checksum pinned in `Cargo.lock`).

Between 0.38.2 and 0.39.1 `libsqlite3-sys` changed the bundled SQLite and
renamed its default `min_sqlite_version_*` feature (not referenced by
rusqlite); every feature rusqlite 0.40.2 forwards to it still exists. The
SQLite C API is backwards compatible.

`bash tor-core/patches/verify-rusqlite-patch.sh` proves the vendored copy is
crates.io 0.40.2 plus that diff and nothing else.

## When to drop it

As soon as the Arti version we pin depends on `rusqlite` >= 0.41 (that is,
`libsqlite3-sys` >= 0.39): remove the `rusqlite` line from
`[patch.crates-io]` in `tor-core/Cargo.toml`, delete this directory, the diff
and the verify script, run `cargo update -p rusqlite`, and confirm
`cargo audit` still shows no RUSTSEC-2026-0459.
