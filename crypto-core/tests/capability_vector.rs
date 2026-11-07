//! The committed capability vector (examples/make_capability_vector.rs) verifies
//! in every build, including the web app's, which has no `signal` feature.
mod common;

use common::Device;
use serde_json::{json, Value};

fn vector() -> Value {
    serde_json::from_str(include_str!("vectors/signal_capability_list.json")).unwrap()
}

#[test]
fn a_list_with_a_signal_capability_verifies_without_libsignal() {
    let v = vector();
    let mut alice = Device::account("alice", "phone");
    let seen = alice.ok("verifyDeviceList", json!({ "deviceList": v["deviceList"] }));
    assert_eq!(seen["version"], 2);
    let mut stripped = v["deviceList"].clone();
    stripped.as_object_mut().unwrap().remove("capabilities");
    assert_eq!(alice.op("verifyDeviceList", json!({ "deviceList": stripped }))["error"], "bad_device_list");
}

#[test]
fn a_device_without_signal_still_chats_over_olm_with_a_signal_device() {
    // This is the web app's position: it knows the peer speaks Signal, and it isn't refused itself.
    let v = vector();
    let mut alice = Device::account("alice", "phone");
    alice.ok("verifyDeviceList", json!({ "deviceList": v["deviceList"] }));
    let peer = &v["deviceList"]["devices"][0];
    let r = alice.op("encrypt", json!({ "recipient": peer, "plaintext": "x" }));
    // No downgrade refusal. The only error is the missing Olm one-time key.
    assert_eq!(r["error"], "no_session", "{r}");
}
