//! Redvault's embedded Tor gateway (proof of concept).
//!
//! Arti (the Tor Project's Rust implementation) runs inside the app. The app's
//! HTTP client talks to a loopback SOCKS5 port that only reaches the Redvault
//! onion service, so private messaging, attachments, stories and voice
//! messages have no route except Tor. Calls are not routed here: they stay on
//! WebRTC with TURN relays.
//!
//! Isolated: no app uses it yet. See docs/tor-routing-plan.md.

mod gateway;
mod policy;
mod socks;
mod status;

#[cfg(feature = "ffi")]
mod ffi;
#[cfg(feature = "jni")]
mod jni_binding;
#[cfg(any(feature = "ffi", feature = "jni"))]
mod json_config;

pub use gateway::{Gateway, GatewayConfig, GatewayError, SocksEndpoint};
pub use policy::{OnionTarget, PolicyError, TargetPolicy};
pub use status::{Stats, Status, TorState};
