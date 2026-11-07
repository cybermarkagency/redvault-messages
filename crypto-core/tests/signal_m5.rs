//! M5 engine support for the apps' screens: which protocol each device gets
//! (`chatProtocols`), a signed way to turn Signal off for a device
//! (`clearSignalCapability`), and what a contact's newly verified list changed
//! (`signalAdded`, `signalRemoved` and `signalReplaced`), so the apps can warn.
//!
//! Run: cargo test --features signal --test signal_m5
#![cfg(feature = "signal")]

mod common;

use common::Device;
use serde_json::{json, Value};

fn signal_device(user: &str, device: &str) -> (Device, Value, Value) {
    let mut d = Device::account(user, device);
    let cert = d.ok("signalEnable", json!({}))["certificate"].clone();
    let upload = d.ok("signalPrekeys", json!({ "count": 2 }))["upload"].clone();
    let list = d.ok("setSignalCapability", json!({ "certificate": cert }))["deviceList"].clone();
    (d, upload, list)
}

fn claimed(upload: &Value, i: usize) -> Value {
    json!({
        "certificate": upload["certificate"],
        "bundle": {
            "registrationId": upload["registrationId"], "identityKey": upload["identityKey"], "signedPreKey": upload["signedPreKey"],
            "kyberPreKey": upload["kyberPreKeys"][i], "lastResort": false, "preKey": upload["preKeys"][i],
        }
    })
}

fn protocol_for(device: &mut Device, peer: &Value) -> String {
    let r = device.ok("chatProtocols", json!({ "recipients": [{ "identity": peer }] }));
    r["protocols"][0]["protocol"].as_str().unwrap().to_owned()
}

#[test]
fn chat_protocols_say_which_protocol_each_device_gets() {
    let (mut alice, _, _) = signal_device("alice", "phone");
    let (bob, _, bob_list) = signal_device("bob", "laptop");
    let mut wendy = Device::account("wendy", "browser");
    alice.ok("verifyDeviceList", json!({ "deviceList": bob_list }));
    alice.ok("verifyDeviceList", json!({ "deviceList": wendy.list() }));
    let r = alice.ok("chatProtocols", json!({ "recipients": [{ "identity": bob.identity }, { "identity": wendy.identity }] }));
    assert_eq!(r["protocols"][0]["protocol"], "rv-signal-v1");
    assert_eq!(r["protocols"][1]["protocol"], "redvault-olm-v1");
    assert_eq!(r["thisDeviceSignal"], true);
    // A device without Signal (the web app) sends Olm to everyone.
    wendy.ok("verifyDeviceList", json!({ "deviceList": bob_list }));
    assert_eq!(protocol_for(&mut wendy, &bob.identity), "redvault-olm-v1");
}

#[test]
fn a_signed_turn_off_moves_a_contact_back_to_olm_and_says_so() {
    let (mut alice, _, alice_list) = signal_device("alice", "phone");
    let (mut bob, bob_up, bob_list) = signal_device("bob", "laptop");
    alice.ok("verifyDeviceList", json!({ "deviceList": bob_list }));
    bob.ok("verifyDeviceList", json!({ "deviceList": alice_list }));
    // A Signal session, so alice also pinned bob from his certificate.
    let sent = alice.ok("encryptMany", json!({ "plaintext": "hi", "recipients": [{ "identity": bob.identity, "signalBundle": claimed(&bob_up, 0) }] }));
    assert_eq!(sent["results"][0]["protocol"], "rv-signal-v1");
    assert_eq!(protocol_for(&mut alice, &bob.identity), "rv-signal-v1");

    let off = bob.ok("clearSignalCapability", json!({ "deviceId": "laptop" }));
    assert_eq!(off["changed"], true);
    assert!(off["deviceList"].get("capabilities").is_none(), "{off}");
    let seen = alice.ok("verifyDeviceList", json!({ "deviceList": off["deviceList"] }));
    assert_eq!(seen["signalRemoved"], json!(["laptop"]), "{seen}");
    assert_eq!(protocol_for(&mut alice, &bob.identity), "redvault-olm-v1");
    // Bob's own device knows Signal is off for it: it stops insisting, and upkeep can't silently switch it back on.
    assert_eq!(bob.ok("signalStatus", json!({}))["disabled"], true);
    assert_eq!(bob.op("signalEnable", json!({}))["error"], "signal_turned_off");
    // Olm chat works again, both ways.
    let olm_keys = bob.prekeys(1);
    let back = alice.ok("encrypt", json!({ "recipient": bob.identity, "plaintext": "olm again", "oneTimeKey": common::first_otk(&olm_keys) }));
    let opened = bob.read(&alice.identity, &back["message"]);
    assert_eq!(opened["plaintext"], "olm again", "{opened}");
    let to_alice = bob.ok("encryptMany", json!({ "plaintext": "from bob", "recipients": [{ "identity": alice.identity }] }));
    assert!(to_alice["results"][0].get("protocol").is_none(), "bob no longer sends Signal: {to_alice}");
    // The person turns it back on explicitly.
    assert_eq!(bob.ok("signalEnable", json!({ "reenable": true }))["protocol"], "rv-signal-v1");
    assert_eq!(bob.ok("signalStatus", json!({}))["disabled"], false);
    // Clearing again changes nothing.
    assert_eq!(bob.ok("clearSignalCapability", json!({ "deviceId": "laptop" }))["changed"], false);
}

#[test]
fn a_replaced_certificate_is_reported_and_only_the_new_one_is_accepted() {
    let (mut alice, _, _) = signal_device("alice", "phone");
    let mut bob = Device::account("bob", "laptop");
    let mut bob_new = bob.clone_state(); // the same device, re-enrolled in Signal (a new Signal identity)
    let old_cert = bob.ok("signalEnable", json!({}))["certificate"].clone();
    let old_up = bob.ok("signalPrekeys", json!({ "count": 1 }))["upload"].clone();
    let v2 = bob.ok("setSignalCapability", json!({ "certificate": old_cert }))["deviceList"].clone();
    alice.ok("verifyDeviceList", json!({ "deviceList": v2 }));
    let first = alice.ok("encryptMany", json!({ "plaintext": "old", "recipients": [{ "identity": bob.identity, "signalBundle": claimed(&old_up, 0) }] }));
    assert_eq!(first["results"][0]["protocol"], "rv-signal-v1");

    // bob's device gets a new Signal identity, and his list (v3, from the newer copy) pins the new certificate.
    bob_new.ok("verifyDeviceList", json!({ "deviceList": v2 }));
    let new_cert = bob_new.ok("signalEnable", json!({}))["certificate"].clone();
    let new_up = bob_new.ok("signalPrekeys", json!({ "count": 1 }))["upload"].clone();
    let v3 = bob_new.ok("setSignalCapability", json!({ "certificate": new_cert }))["deviceList"].clone();
    assert_eq!(v3["version"], 3);
    let seen = alice.ok("verifyDeviceList", json!({ "deviceList": v3 }));
    assert_eq!(seen["signalReplaced"], json!(["laptop"]), "{seen}");

    // The old session is gone: alice needs a new bundle, and only the new certificate opens one.
    let ask = alice.ok("encryptMany", json!({ "plaintext": "x", "recipients": [{ "identity": bob.identity }] }));
    assert_eq!(ask["results"][0]["error"], "no_signal_session", "{ask}");
    let stale = alice.ok("encryptMany", json!({ "plaintext": "x", "recipients": [{ "identity": bob.identity, "signalBundle": claimed(&old_up, 0) }] }));
    assert_eq!(stale["results"][0]["error"], "certificate_mismatch", "{stale}");
    let fresh = alice.ok("encryptMany", json!({ "plaintext": "new", "recipients": [{ "identity": bob.identity, "signalBundle": claimed(&new_up, 0) }] }));
    assert_eq!(fresh["results"][0]["protocol"], "rv-signal-v1", "{fresh}");
    let opened = bob_new.op("decrypt", json!({ "sender": alice.identity, "message": fresh["results"][0]["message"] }));
    assert_eq!(opened["ok"], false, "bob_new hasn't verified alice's list: {opened}");
}

#[test]
fn only_a_trusted_device_can_turn_signal_off() {
    let mut stranger = Device::new("bob", "tablet");
    assert_eq!(stranger.op("clearSignalCapability", json!({ "deviceId": "laptop" }))["error"], "not_trusted");
}
