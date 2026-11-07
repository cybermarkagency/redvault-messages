# tor-core: embedded Tor gateway (proof of concept)

Isolated: no app uses it yet. Plan, results and limitations:
[docs/tor-routing-plan.md](../docs/tor-routing-plan.md).

Arti (the Tor Project's Rust Tor, `arti-client` 0.47.0) runs inside the app and
opens a loopback SOCKS5 port that only reaches the Redvault onion service:
username/password required, CONNECT only, hostnames only (IP literals mean a
local DNS lookup happened), exact `<v3>.onion:port` allowlist. The apps point
their one HTTP client at it (libcurl `socks5h://`, OkHttp `Proxy.Type.SOCKS`).
Calls are not routed here: they stay on WebRTC with TURN relays.

| Path | What |
|---|---|
| `src/policy.rs` | the onion allowlist |
| `src/socks.rs` | the SOCKS5 gate (+ unit tests with a fake Tor) |
| `src/status.rs` | connecting / connected / unavailable, counters |
| `src/gateway.rs` | Arti client + gate |
| `src/ffi.rs`, `include/redvault_tor.h` | C ABI (desktop) |
| `src/jni_binding.rs`, `bindings/android/` | JNI (Android) + the emulator probe |
| `src/bin/rv-tor-probe.rs` | test probe (Windows/Linux) |
| `src/bin/rv-onion-host.rs` | test only (`onion-host` feature): a throwaway onion service in front of a local port |
| `patches/saturating-time` | fix for an upstream bug that hangs Arti on Windows |
| `poc/harness/` | the real server code on in-memory repositories behind onion / direct / control listeners, and the prototype server policy |
| `poc/desktop-driver/` | the desktop core's repositories end to end over Tor (libcurl transport with a socket guard) |
| `poc/linux/` | LD_PRELOAD `connect()`/DNS logger, leak and blocked-network tests |
| `poc/check_relays.py`, `poc/run-with-connection-log.ps1` | "every remote address is a Tor relay" check |

Features: `ffi`, `jni`; test-only `test-hooks` (simulated blocked network) and
`onion-host`, which must never ship.

Builds (Rust 1.99.1, `~/.cargo/bin` on PATH, MSYS2 UCRT64 for Windows):

    cargo test --features ffi,test-hooks
    cargo build --features ffi,test-hooks,onion-host            # Windows
    bash poc/wsl-build.sh                                        # Linux (WSL)
    # Android: NDK r29 clang as linker and CC, see docs/tor-routing-plan.md
