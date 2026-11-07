//! Redvault's shared end-to-end encryption core (proof of concept).
//!
//! One implementation for every client: built to WebAssembly for the web app,
//! to a C ABI library for the C++ desktop app (Windows and Linux), and to a JNI
//! library for Android. Every binding passes the same JSON requests to
//! [`engine::handle`]; see docs/vodozemac-poc.md for the commands and what
//! they guarantee.

pub mod engine;
pub(crate) mod identity;
pub(crate) mod pairing;
pub(crate) mod stories;
pub(crate) mod signal_state;
/// Signed group rosters with history (M6). No libsignal code: every build.
pub(crate) mod roster;

// Owner decision 2026-11-07 (docs/signal-integration/01-audit-and-plan.md section 5): the web app
// stays on protocols 1 and 3. This stops libsignal (AGPL) from reaching the web bundle by accident.
#[cfg(all(feature = "signal", feature = "wasm"))]
compile_error!("the signal feature is not for the web build (owner decision 2026-11-07)");

/// Signal sessions from libsignal (AGPL-3.0-only). Development only: see Cargo.toml.
#[cfg(feature = "signal")]
pub(crate) mod signal;
/// Signal groups: sender keys over the pairwise Signal sessions (M6). Development only.
#[cfg(feature = "signal")]
pub(crate) mod signal_group;

#[cfg(feature = "ffi")]
pub mod ffi;

#[cfg(feature = "wasm")]
pub mod wasm;

#[cfg(feature = "jni")]
pub mod jni_binding;
