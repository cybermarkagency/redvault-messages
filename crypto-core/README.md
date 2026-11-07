# crypto-core: shared end-to-end encryption core (proof of concept)

vodozemac 0.11.1 (Olm and Megolm) behind one JSON command interface, for the
web app (WebAssembly), the desktop app (C ABI, Windows and Linux) and Android
(JNI). What it does and doesn't guarantee, the commands, and the test results
are in `docs/vodozemac-poc.md`. The per-device identity work built on it
(server, web preview, end-to-end run) is in `docs/multi-device-threat-model.md`.
**Development only:** the web app loads it only behind the server's
`E2E_V3_ENABLED` flag, and no real account uses it.

## Build (Rust 1.99.1 via rustup)

```bash
cargo test --release --locked
```

| Platform | Command | Output |
|---|---|---|
| Windows (MinGW, the desktop app's toolchain) | `cargo build --release --locked --features ffi` | `target/release/libredvault_e2ee.a` |
| Linux | `cargo build --release --locked --features ffi` | `target/release/libredvault_e2ee.a` |
| Web | `RUSTFLAGS='--cfg getrandom_backend="wasm_js"' cargo build --release --locked --target wasm32-unknown-unknown --features wasm`, then `wasm-bindgen --target web --out-dir interop/build/web target/wasm32-unknown-unknown/release/redvault_e2ee.wasm` (and `--target nodejs` into `interop/build/node`) | the module and its JS glue |
| Android (NDK r29) | Set `CARGO_TARGET_<TRIPLE>_LINKER` to the NDK's `<triple>26-clang.cmd` and `RUSTFLAGS="-C link-arg=-Wl,-z,max-page-size=16384"`, then `cargo build --release --locked --features jni --target aarch64-linux-android` (also `armv7-linux-androideabi`, `x86_64-linux-android`, `i686-linux-android`) | `target/<triple>/release/libredvault_e2ee.so` |

Java side of the JNI binding: `bindings/android/com/redvaultsystems/e2ee/`.

For the web app's preview, `build-web.sh` writes `pkg/web` (which the app
loads) and `pkg/node` (which its tests load). Both are ignored by git.

## Cross-platform run

1. Build every driver:
   - `interop/drivers/cpp_driver.cpp`, against the static library, on Windows
     and in WSL
   - `node_driver.cjs`
   - the Java `Driver`, dexed with `d8` and pushed with the `.so` to
     `/data/local/tmp` on an emulator
2. Start the relay: `node interop/relay.mjs`.
3. Open `http://127.0.0.1:8787/` in a browser.
4. Run `node interop/run.mjs web,android,windows,linux`.

## End to end against the real server

1. Create a throwaway PostgreSQL database and load `db/schema.sql` and every
   migration into it.
2. Run the command below. It starts the real API with protocol 3 on
   (`interop/server/dev-server.mts`, local databases only) and runs every
   pairing, the leak scan and the rollback check.

```bash
V3_DEV_DATABASE_URL=postgres://postgres@127.0.0.1:5457/v3test node interop/server-run.mjs web,windows,linux,android
```

The "web" platform needs the relay and its browser page, as above.
