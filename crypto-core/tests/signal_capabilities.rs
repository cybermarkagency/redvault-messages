// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Capabilities in the signed device list (M2). A trusted device declares that
//! one of its account's devices speaks rv-signal-v1 with exactly one
//! certificate, so a server can neither hide nor swap that certificate.
//!
//! Run: cargo test --features signal --test signal_capabilities
#![cfg(feature = "signal")]

mod common;

use common::Device;
use serde_json::{json, Value};

fn enable(d: &mut Device) -> Value {
    d.ok("signalEnable", json!({}))["certificate"].clone()
}

fn upload(d: &mut Device) -> Value {
    d.ok("signalPrekeys", json!({ "count": 1 }))["upload"].clone()
}

fn bundle(up: &Value) -> Value {
    json!({
        "registrationId": up["registrationId"],
        "identityKey": up["identityKey"],
        "signedPreKey": up["signedPreKey"],
        "kyberPreKey": up["kyberPreKeys"][0],
        "preKey": up["preKeys"][0],
    })
}

/// `bob`'s trusted device declares the certificate in its signed list and returns the new list.
fn advertise(bob: &mut Device, cert: &Value) -> Value {
    let r = bob.ok("setSignalCapability", json!({ "certificate": cert }));
    assert_eq!(r["changed"], true);
    r["deviceList"].clone()
}

#[test]
fn a_listed_capability_pins_the_certificate() {
    let mut alice = Device::account("alice", "phone");
    enable(&mut alice);
    let mut bob = Device::account("bob", "laptop");
    let mut bob_other = bob.clone_state();
    let cert = enable(&mut bob);
    let other_cert = enable(&mut bob_other);
    let v2 = advertise(&mut bob, &cert);
    assert_eq!(v2["version"], 2);
    assert_eq!(v2["capabilities"][0]["protocol"], "rv-signal-v1");
    // Declaring the same certificate again changes nothing.
    assert_eq!(bob.ok("setSignalCapability", json!({ "certificate": cert }))["changed"], false);
    alice.ok("verifyDeviceList", json!({ "deviceList": v2 }));

    // A validly signed certificate for the same device that isn't the listed one.
    let other_up = upload(&mut bob_other);
    let r = alice.op("signalStartSession", json!({ "peer": bob.identity, "certificate": other_cert, "bundle": bundle(&other_up) }));
    assert_eq!(r["error"], "certificate_mismatch", "{r}");
    let up = upload(&mut bob);
    let ok = alice.op("signalStartSession", json!({ "peer": bob.identity, "certificate": cert, "bundle": bundle(&up) }));
    assert_eq!(ok["ok"], true, "{ok}");
}

#[test]
fn a_withheld_certificate_cannot_downgrade_two_signal_devices() {
    let mut alice = Device::account("alice", "phone");
    enable(&mut alice);
    let mut web = Device::account("wendy", "browser"); // no Signal, like the web app
    let mut bob = Device::account("bob", "laptop");
    let cert = enable(&mut bob);
    let v2 = advertise(&mut bob, &cert);
    alice.ok("verifyDeviceList", json!({ "deviceList": v2 }));
    web.ok("verifyDeviceList", json!({ "deviceList": v2 }));
    let olm_keys = bob.prekeys(2);

    // The server hides bob's certificate from alice. She still refuses Olm chat with him.
    let r = alice.op("encrypt", json!({ "recipient": bob.identity, "plaintext": "x", "oneTimeKey": olm_keys["oneTimeKeys"][0] }));
    assert_eq!(r["error"], "downgrade_refused", "{r}");

    // A device without Signal is never refused: web chats keep working over Olm.
    let from_web = web.send(&bob.identity, Some(&olm_keys["oneTimeKeys"][1]), "from the web");
    let web_list = web.list();
    bob.ok("verifyDeviceList", json!({ "deviceList": web_list }));
    assert_eq!(bob.read(&web.identity, &from_web)["plaintext"], "from the web");
}

#[test]
fn stripping_or_editing_capabilities_breaks_the_list_signature() {
    let mut alice = Device::account("alice", "phone");
    let mut bob = Device::account("bob", "laptop");
    let cert = enable(&mut bob);
    let v2 = advertise(&mut bob, &cert);

    let mut stripped = v2.clone();
    stripped.as_object_mut().unwrap().remove("capabilities");
    assert_eq!(alice.op("verifyDeviceList", json!({ "deviceList": stripped }))["error"], "bad_device_list");

    let mut edited = v2.clone();
    edited["capabilities"][0]["certificateHash"] = json!("A".repeat(43));
    assert_eq!(alice.op("verifyDeviceList", json!({ "deviceList": edited }))["error"], "bad_device_list");

    let mut unknown = v2.clone();
    unknown["capabilities"][0]["protocol"] = json!("rv-signal-v9");
    assert_eq!(alice.op("verifyDeviceList", json!({ "deviceList": unknown }))["error"], "bad_device_list");

    let mut extra = v2.clone();
    extra["capabilities"][0]["note"] = json!("x");
    assert_eq!(alice.op("verifyDeviceList", json!({ "deviceList": extra }))["ok"], false);

    assert_eq!(alice.op("verifyDeviceList", json!({ "deviceList": v2 }))["ok"], true);
}

#[test]
fn only_a_trusted_device_can_declare_a_capability_and_only_for_a_listed_device() {
    let mut bob = Device::account("bob", "laptop");
    let mut phone = Device::new("bob", "phone");
    common::link(&mut bob, &mut phone);
    let phone_cert = enable(&mut phone);
    // A device that was never linked doesn't hold the account key.
    // (Linked devices do: protocol 3 makes every linked device trusted.)
    let mut tablet = Device::new("bob", "tablet");
    let tablet_cert = enable(&mut tablet);
    assert_eq!(tablet.op("setSignalCapability", json!({ "certificate": tablet_cert }))["error"], "not_trusted");
    // A device that holds the key can't declare one for a device outside the list.
    assert_eq!(bob.op("setSignalCapability", json!({ "certificate": tablet_cert }))["error"], "unknown_device");
    // The trusted laptop can declare it for the linked phone.
    let list = bob.ok("setSignalCapability", json!({ "certificate": phone_cert }))["deviceList"].clone();
    assert_eq!(list["capabilities"][0]["deviceId"], "phone");
    // A certificate from another account's device is refused.
    let mut eve = Device::account("eve", "pc");
    let eve_cert = enable(&mut eve);
    assert_eq!(bob.op("setSignalCapability", json!({ "certificate": eve_cert }))["error"], "unknown_device");
    // A tampered certificate is refused.
    let mut forged = phone_cert.clone();
    forged["registrationId"] = json!(1);
    assert_eq!(bob.op("setSignalCapability", json!({ "certificate": forged }))["error"], "bad_certificate");
}

#[test]
fn capabilities_follow_the_list_through_rotation_and_revocation() {
    let mut bob = Device::account("bob", "laptop");
    let mut phone = Device::new("bob", "phone");
    common::link(&mut bob, &mut phone);
    let laptop_cert = enable(&mut bob);
    let phone_cert = enable(&mut phone);
    bob.ok("setSignalCapability", json!({ "certificate": laptop_cert }));
    let v = bob.ok("setSignalCapability", json!({ "certificate": phone_cert }))["deviceList"].clone();
    assert_eq!(v["capabilities"].as_array().unwrap().len(), 2);
    let rotated = bob.ok("rotateAccountKey", json!({}))["deviceList"].clone();
    assert_eq!(rotated["capabilities"], v["capabilities"]);
    let revoked = bob.ok("revokeDevice", json!({ "deviceId": "phone" }))["deviceList"].clone();
    assert_eq!(revoked["capabilities"].as_array().unwrap().len(), 1);
    assert_eq!(revoked["capabilities"][0]["deviceId"], "laptop");
}
