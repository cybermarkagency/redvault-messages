// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Session reset (docs/signal-integration/13-every-device-and-session-reset.md): a device that
//! can't open a peer's messages sets its session aside (`signalResetSession`) and starts a new one
//! with a `signalRefresh` message, which the peer opens and switches to.
//!
//! Run: cargo test --features signal --test signal_reset
#![cfg(feature = "signal")]

mod common;

use common::Device;
use serde_json::{json, Value};

const MINUTE: u64 = 60_000;

fn signal_device(user: &str, device: &str) -> (Device, Value, Value) {
    let mut d = Device::account(user, device);
    let cert = d.ok("signalEnable", json!({}))["certificate"].clone();
    let upload = d.ok("signalPrekeys", json!({ "count": 4 }))["upload"].clone();
    let list = d.ok("setSignalCapability", json!({ "certificate": cert }))["deviceList"].clone();
    (d, upload, list)
}

fn claimed(upload: &Value, index: usize) -> Value {
    json!({
        "certificate": upload["certificate"],
        "bundle": {
            "registrationId": upload["registrationId"],
            "identityKey": upload["identityKey"],
            "signedPreKey": upload["signedPreKey"],
            "kyberPreKey": upload["kyberPreKeys"][index],
            "lastResort": false,
            "preKey": upload["preKeys"][index],
        }
    })
}

/// encryptMany to one device: Ok(message) or Err(error code).
fn send(from: &mut Device, to: &Value, bundle: Option<Value>, text: &str) -> Result<Value, String> {
    let mut recipient = json!({ "identity": to });
    if let Some(b) = bundle {
        recipient["signalBundle"] = b;
    }
    let r = from.ok("encryptMany", json!({ "plaintext": text, "recipients": [recipient] }));
    let first = r["results"][0].clone();
    match first.get("error").and_then(Value::as_str) {
        Some(code) => Err(code.to_owned()),
        None => Ok(first["message"].clone()),
    }
}

/// Two Signal devices that know each other's lists and have talked both ways.
fn talking() -> (Device, Value, Device, Value) {
    let (mut alice, alice_up, alice_list) = signal_device("alice", "phone");
    let (mut bob, bob_up, bob_list) = signal_device("bob", "laptop");
    alice.ok("verifyDeviceList", json!({ "deviceList": bob_list }));
    bob.ok("verifyDeviceList", json!({ "deviceList": alice_list }));
    let first = send(&mut alice, &bob.identity.clone(), Some(claimed(&bob_up, 0)), "hello").unwrap();
    assert_eq!(bob.read(&alice.identity.clone(), &first)["plaintext"], "hello");
    let reply = send(&mut bob, &alice.identity.clone(), None, "hi").unwrap();
    assert_eq!(alice.read(&bob.identity.clone(), &reply)["plaintext"], "hi");
    (alice, alice_up, bob, bob_up)
}

#[test]
fn a_device_that_lost_its_session_heals_it_with_one_refresh() {
    let (mut alice, alice_up, bob, _) = talking();
    // bob's laptop will come back from this copy of its state, after the two have moved on.
    let mut bob_now = bob;
    let mut bob = bob_now.clone_state();
    for round in 0..3 {
        let m = send(&mut alice, &bob_now.identity.clone(), None, &format!("a{round}")).unwrap();
        assert_eq!(bob_now.read(&alice.identity.clone(), &m)["ok"], true);
        let r = send(&mut bob_now, &alice.identity.clone(), None, &format!("b{round}")).unwrap();
        assert_eq!(alice.read(&bob_now.identity.clone(), &r)["ok"], true);
    }
    let lost = send(&mut alice, &bob.identity.clone(), None, "can you read this?").unwrap();
    assert_eq!(lost["type"], "signal");
    let failed = bob.read(&alice.identity.clone(), &lost);
    assert_eq!(failed["ok"], false);
    assert_eq!(failed["error"], "decrypt_failed", "{failed}");

    // The app's answer: reset (rate-limited) and send a refresh on a bundle claimed for alice.
    let reset = bob.ok("signalResetSession", json!({ "peer": alice.identity, "now": 20 * MINUTE }));
    assert_eq!(reset["reset"], true);
    let needs = bob.op("signalRefresh", json!({ "peer": alice.identity }));
    assert_eq!(needs["error"], "no_signal_session", "a reset session can't be sent on: {needs}");
    let refresh = bob.ok("signalRefresh", json!({ "peer": alice.identity, "claimed": claimed(&alice_up, 1) }))["message"].clone();
    assert_eq!(refresh["type"], "prekey");

    let opened = alice.read(&bob.identity.clone(), &refresh);
    assert_eq!(opened["ok"], true, "{opened}");
    assert_eq!(opened["kind"], "refresh");
    assert!(opened.get("plaintext").is_none());

    // From here alice sends on the new session, and bob opens it.
    let healed = send(&mut alice, &bob.identity.clone(), None, "and now?").unwrap();
    assert_eq!(bob.read(&alice.identity.clone(), &healed)["plaintext"], "and now?");
    let back = send(&mut bob, &alice.identity.clone(), None, "yes").unwrap();
    assert_eq!(alice.read(&bob.identity.clone(), &back)["plaintext"], "yes");
}

#[test]
fn a_reset_keeps_the_old_state_for_messages_already_on_their_way() {
    let (mut alice, alice_up, mut bob, _) = talking();
    let in_flight = send(&mut alice, &bob.identity.clone(), None, "sent before the reset").unwrap();
    assert_eq!(bob.ok("signalResetSession", json!({ "peer": alice.identity, "now": 0 }))["reset"], true);
    assert_eq!(bob.read(&alice.identity.clone(), &in_flight)["plaintext"], "sent before the reset");
    // bob's next message needs a new bundle; the old session isn't used to send any more.
    assert_eq!(send(&mut bob, &alice.identity.clone(), None, "x").unwrap_err(), "no_signal_session");
    let fresh = send(&mut bob, &alice.identity.clone(), Some(claimed(&alice_up, 1)), "new session").unwrap();
    assert_eq!(fresh["type"], "prekey");
    assert_eq!(alice.read(&bob.identity.clone(), &fresh)["plaintext"], "new session");
}

#[test]
fn resets_are_limited_to_one_per_peer_every_ten_minutes() {
    let (alice, _, mut bob, _) = talking();
    let peer = alice.identity.clone();
    assert_eq!(bob.ok("signalResetSession", json!({ "peer": peer, "now": 100 * MINUTE }))["reset"], true);
    let again = bob.ok("signalResetSession", json!({ "peer": peer, "now": 109 * MINUTE }));
    assert_eq!(again["reset"], false);
    assert_eq!(again["reason"], "recent");
    assert_eq!(bob.ok("signalResetSession", json!({ "peer": peer, "now": 111 * MINUTE }))["reset"], true);
}

#[test]
fn only_a_listed_device_can_be_reset() {
    let (_, _, mut bob, _) = talking();
    let (stranger, _, _) = signal_device("mallory", "phone");
    let refused = bob.op("signalResetSession", json!({ "peer": stranger.identity, "now": 0 }));
    assert_eq!(refused["ok"], false, "{refused}");
}
