// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Anonymous sticker answers (RVSD copies): `storySeal` to a pinned author device
//! certificate, `storyUnseal` with this device's own Signal identity key.
//!
//! Run: cargo test --features signal --test signal_story_seal
#![cfg(feature = "signal")]

mod common;

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use common::Device;
use serde_json::{json, Value};

struct Author {
    phone: Device,
    laptop: Device,
    phone_cert: Value,
    laptop_cert: Value,
    list: Value,
}

/// An author with two devices that both speak Signal, both pinned in the signed list.
fn author() -> Author {
    let mut phone = Device::account("alice", "phone");
    let mut laptop = Device::new("alice", "laptop");
    common::link(&mut phone, &mut laptop);
    let phone_cert = phone.ok("signalEnable", json!({}))["certificate"].clone();
    let laptop_cert = laptop.ok("signalEnable", json!({}))["certificate"].clone();
    phone.ok("setSignalCapability", json!({ "certificate": phone_cert }));
    let list = phone.ok("setSignalCapability", json!({ "certificate": laptop_cert }))["deviceList"].clone();
    laptop.ok("verifyDeviceList", json!({ "deviceList": list }));
    Author { phone, laptop, phone_cert, laptop_cert, list }
}

fn viewer(list: &Value) -> Device {
    let mut bob = Device::account("bob", "phone");
    bob.ok("signalEnable", json!({}));
    bob.ok("verifyDeviceList", json!({ "deviceList": list }));
    bob
}

fn seal(d: &mut Device, cert: &Value, text: &str) -> String {
    d.ok("storySeal", json!({ "certificate": cert, "plaintext": text }))["sealed"].as_str().unwrap().to_owned()
}

#[test]
fn copies_open_on_each_author_device_only() {
    let mut a = author();
    let mut bob = viewer(&a.list);
    let answer = r#"{"choice":2}"#;
    let to_phone = seal(&mut bob, &a.phone_cert, answer);
    let to_laptop = seal(&mut bob, &a.laptop_cert, answer);
    assert!(B64.decode(&to_phone).unwrap().len() >= 48);
    assert_eq!(B64.decode(&to_phone).unwrap().len(), 32 + answer.len() + 16);
    assert_ne!(to_phone, seal(&mut bob, &a.phone_cert, answer), "a fresh ephemeral key every time");

    assert_eq!(a.phone.ok("storyUnseal", json!({ "sealed": to_phone }))["plaintext"], answer);
    assert_eq!(a.laptop.ok("storyUnseal", json!({ "sealed": to_laptop }))["plaintext"], answer);
    // Each copy opens only on its own device.
    assert_eq!(a.phone.op("storyUnseal", json!({ "sealed": to_laptop }))["error"], "open_failed");
    assert_eq!(a.laptop.op("storyUnseal", json!({ "sealed": to_phone }))["error"], "open_failed");
    // A third device (the viewer itself, or anyone else) can't open it.
    assert_eq!(bob.op("storyUnseal", json!({ "sealed": to_phone }))["error"], "open_failed");
    let mut carol = Device::account("carol", "phone");
    carol.ok("signalEnable", json!({}));
    assert_eq!(carol.op("storyUnseal", json!({ "sealed": to_laptop }))["error"], "open_failed");
}

#[test]
fn an_unpinned_or_unverified_certificate_is_refused() {
    let a = author();
    let mut bob = viewer(&a.list);

    // The laptop's certificate with a changed field: its signature no longer verifies.
    let mut tampered = a.laptop_cert.clone();
    tampered["registrationId"] = json!(tampered["registrationId"].as_u64().unwrap() ^ 1);
    let r = bob.op("storySeal", json!({ "certificate": tampered, "plaintext": "x" }));
    assert_eq!(r["error"], "bad_certificate", "{r}");

    // An account whose list bob never verified.
    let mut dave = Device::account("dave", "phone");
    let dave_cert = dave.ok("signalEnable", json!({}))["certificate"].clone();
    let r = bob.op("storySeal", json!({ "certificate": dave_cert, "plaintext": "x" }));
    assert_eq!(r["error"], "unverified_device", "{r}");

    // A device in a verified list that pins no certificate for it.
    let mut erin = Device::account("erin", "phone");
    let mut erin_fork = erin.clone_state();
    let erin_cert = erin.ok("signalEnable", json!({}))["certificate"].clone();
    bob.ok("verifyDeviceList", json!({ "deviceList": erin.list() }));
    let r = bob.op("storySeal", json!({ "certificate": erin_cert, "plaintext": "x" }));
    assert_eq!(r["error"], "certificate_mismatch", "{r}");

    // The list pins erin's first certificate; a second validly signed one (another Signal identity
    // for the same device, as a server could offer) is refused.
    let other_cert = erin_fork.ok("signalEnable", json!({}))["certificate"].clone();
    assert_ne!(other_cert["identityKey"], erin_cert["identityKey"]);
    let list = erin.ok("setSignalCapability", json!({ "certificate": erin_cert }))["deviceList"].clone();
    bob.ok("verifyDeviceList", json!({ "deviceList": list }));
    let r = bob.op("storySeal", json!({ "certificate": other_cert, "plaintext": "x" }));
    assert_eq!(r["error"], "certificate_mismatch", "{r}");
    let sealed = bob.ok("storySeal", json!({ "certificate": erin_cert, "plaintext": "x" }))["sealed"].clone();
    assert_eq!(erin.ok("storyUnseal", json!({ "sealed": sealed }))["plaintext"], "x");
    assert_eq!(erin_fork.op("storyUnseal", json!({ "sealed": sealed }))["error"], "open_failed");
}

#[test]
fn a_changed_byte_fails_and_bad_input_is_refused() {
    let mut a = author();
    let mut bob = viewer(&a.list);
    let sealed = B64.decode(seal(&mut bob, &a.phone_cert, "hello")).unwrap();
    for i in [0, 31, 32, sealed.len() - 1] {
        let mut changed = sealed.clone();
        changed[i] ^= 0x01;
        let r = a.phone.op("storyUnseal", json!({ "sealed": B64.encode(&changed) }));
        assert_eq!(r["error"], "open_failed", "byte {i}: {r}");
    }
    assert_eq!(a.phone.op("storyUnseal", json!({ "sealed": "not base64!" }))["error"], "bad_request");
    assert_eq!(a.phone.op("storyUnseal", json!({ "sealed": B64.encode([0u8; 47]) }))["error"], "bad_request");
    assert_eq!(a.phone.op("storyUnseal", json!({}))["error"], "bad_request");
    assert_eq!(bob.op("storySeal", json!({ "plaintext": "x" }))["error"], "bad_request");
    // A device without Signal can't open anything.
    let mut web = Device::account("wendy", "browser");
    assert_eq!(web.op("storyUnseal", json!({ "sealed": B64.encode(&sealed) }))["error"], "signal_not_enabled");
}
