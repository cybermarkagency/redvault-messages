// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Signal sessions (rv-signal-v1) through the engine's real JSON interface:
//! PQXDH, the Triple Ratchet, certificates, downgrade pins and the failure
//! paths. Every test drives `engine::handle`, the same entry point the apps use.
//! Nothing is mocked.
//!
//! Run: cargo test --features signal --test signal
#![cfg(feature = "signal")]

mod common;

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use common::Device;
use proptest::prelude::*;
use serde_json::{json, Value};

// --- Helpers ---

/// A device of a new account with Signal enabled. Returns the device and its certificate.
fn signal_account(user: &str, device: &str) -> (Device, Value) {
    let mut d = Device::account(user, device);
    let enabled = d.ok("signalEnable", json!({}));
    assert_eq!(enabled["protocol"], "rv-signal-v1");
    let cert = enabled["certificate"].clone();
    (d, cert)
}

/// `who` checks `other`'s signed device list. This is what makes `other`'s devices trusted.
fn know(who: &mut Device, other: &mut Device) {
    let list = other.list();
    who.ok("verifyDeviceList", json!({ "deviceList": list }));
}

fn upload(d: &mut Device, count: u64) -> Value {
    d.ok("signalPrekeys", json!({ "count": count }))["upload"].clone()
}

/// A bundle as the server would hand it out: the signed keys plus one one-time key of each kind
/// (or the last-resort Kyber key when `use_last_resort`).
fn bundle(up: &Value, index: usize, use_last_resort: bool) -> Value {
    let kyber = if use_last_resort { up["lastResortKyberPreKey"].clone() } else { up["kyberPreKeys"][index].clone() };
    let pre = up["preKeys"].get(index).cloned().unwrap_or(Value::Null);
    json!({
        "registrationId": up["registrationId"],
        "identityKey": up["identityKey"],
        "signedPreKey": up["signedPreKey"],
        "kyberPreKey": kyber,
        "preKey": pre,
    })
}

fn start(from: &mut Device, to: &Device, cert: &Value, b: &Value) -> Value {
    from.op("signalStartSession", json!({ "peer": to.identity, "certificate": cert, "bundle": b }))
}

fn send(from: &mut Device, to: &Device, text: &str) -> Value {
    from.ok("signalEncrypt", json!({ "peer": to.identity, "plaintext": text }))["message"].clone()
}

fn read(to: &mut Device, from: &Device, cert: Option<&Value>, message: &Value) -> Value {
    let mut request = json!({ "sender": from.identity, "message": message });
    if let Some(cert) = cert {
        request["certificate"] = cert.clone();
    }
    to.op("signalDecrypt", request)
}

/// alice (phone) and bob (laptop): accounts that verified each other's lists,
/// with Signal on, and a session from alice to bob. Nothing has been sent yet.
struct Pair {
    alice: Device,
    alice_cert: Value,
    bob: Device,
    bob_cert: Value,
}

fn pair() -> Pair {
    let (mut alice, alice_cert) = signal_account("alice", "phone");
    let (mut bob, bob_cert) = signal_account("bob", "laptop");
    know(&mut alice, &mut bob);
    know(&mut bob, &mut alice);
    let up = upload(&mut bob, 3);
    let started = start(&mut alice, &bob, &bob_cert, &bundle(&up, 0, false));
    assert_eq!(started["ok"], true, "{started}");
    Pair { alice, alice_cert, bob, bob_cert }
}

/// The pair after the first message (alice to bob) and a reply, so both sides have a full session.
fn talking() -> Pair {
    let mut p = pair();
    let first = send(&mut p.alice, &p.bob, "hello");
    assert_eq!(first["type"], "prekey");
    let opened = read(&mut p.bob, &p.alice, Some(&p.alice_cert), &first);
    assert_eq!(opened["plaintext"], "hello", "{opened}");
    let reply = send(&mut p.bob, &p.alice, "hi");
    assert_eq!(reply["type"], "signal");
    assert_eq!(read(&mut p.alice, &p.bob, None, &reply)["plaintext"], "hi");
    p
}

fn flip(message: &Value, at: usize) -> Value {
    let mut bytes = B64.decode(message["body"].as_str().unwrap()).unwrap();
    let i = at % bytes.len();
    bytes[i] ^= 0x01;
    let mut out = message.clone();
    out["body"] = json!(B64.encode(bytes));
    out
}

// --- End-to-end encryption and PQXDH ---

#[test]
fn pqxdh_session_carries_messages_both_ways_and_names_the_protocol() {
    let mut p = talking();
    for i in 0..10 {
        let m = send(&mut p.alice, &p.bob, &format!("a{i}"));
        assert_eq!(m["protocol"], "rv-signal-v1");
        assert_eq!(read(&mut p.bob, &p.alice, None, &m)["plaintext"], format!("a{i}"));
        let r = send(&mut p.bob, &p.alice, &format!("b{i}"));
        assert_eq!(read(&mut p.alice, &p.bob, None, &r)["plaintext"], format!("b{i}"));
    }
    let status = p.bob.ok("signalStatus", json!({}));
    assert_eq!(status["enabled"], true);
    assert_eq!(status["sessions"], 1);
    // One one-time EC key and one one-time Kyber key were used up by alice's first message.
    assert_eq!(status["oneTimePreKeys"], 2);
    assert_eq!(status["oneTimeKyberPreKeys"], 2);
}

#[test]
fn the_engine_reports_the_pinned_libsignal_version() {
    let version = common::call(json!({ "op": "version" }));
    assert_eq!(version["signal"]["libsignal"], "v0.106.2");
    assert_eq!(version["signal"]["protocol"], "rv-signal-v1");
}

#[test]
fn a_session_needs_signal_enabled_on_this_device() {
    let mut alice = Device::account("alice", "phone");
    let (mut bob, bob_cert) = signal_account("bob", "laptop");
    know(&mut alice, &mut bob);
    let up = upload(&mut bob, 1);
    assert_eq!(start(&mut alice, &bob, &bob_cert, &bundle(&up, 0, false))["error"], "signal_not_enabled");
}

#[test]
fn the_last_resort_kyber_key_works_when_one_time_keys_run_out() {
    let (mut alice, _) = signal_account("alice", "phone");
    let (mut carol, carol_cert) = signal_account("carol", "phone");
    let (mut bob, bob_cert) = signal_account("bob", "laptop");
    for d in [&mut alice, &mut carol] {
        know(d, &mut bob);
        know(&mut bob, d);
    }
    let up = upload(&mut bob, 0);
    assert_eq!(up["preKeys"].as_array().unwrap().len(), 0);
    let b = bundle(&up, 0, true);
    assert_eq!(start(&mut alice, &bob, &bob_cert, &b)["ok"], true);
    assert_eq!(start(&mut carol, &bob, &bob_cert, &b)["ok"], true);
    let alice_cert = alice.ok("signalEnable", json!({}))["certificate"].clone();
    let from_alice = send(&mut alice, &bob, "via last resort");
    let from_carol = send(&mut carol, &bob, "me too");
    assert_eq!(read(&mut bob, &alice, Some(&alice_cert), &from_alice)["plaintext"], "via last resort");
    assert_eq!(read(&mut bob, &carol, Some(&carol_cert), &from_carol)["plaintext"], "me too");
    // The last-resort key stays; the replayed first message is refused.
    assert_eq!(read(&mut bob, &alice, None, &from_alice)["ok"], false);
}

#[test]
fn a_one_time_prekey_opens_only_one_session() {
    // A faulty or hostile server hands the same one-time keys to two senders.
    let (mut alice, alice_cert) = signal_account("alice", "phone");
    let (mut carol, carol_cert) = signal_account("carol", "phone");
    let (mut bob, bob_cert) = signal_account("bob", "laptop");
    for d in [&mut alice, &mut carol] {
        know(d, &mut bob);
        know(&mut bob, d);
    }
    let up = upload(&mut bob, 1);
    let b = bundle(&up, 0, false);
    assert_eq!(start(&mut alice, &bob, &bob_cert, &b)["ok"], true);
    assert_eq!(start(&mut carol, &bob, &bob_cert, &b)["ok"], true);
    let from_alice = send(&mut alice, &bob, "first");
    let from_carol = send(&mut carol, &bob, "second");
    assert_eq!(read(&mut bob, &alice, Some(&alice_cert), &from_alice)["plaintext"], "first");
    assert_eq!(read(&mut bob, &carol, Some(&carol_cert), &from_carol)["error"], "unknown_prekey");
}

// --- Bundle and certificate validation ---

#[test]
fn a_tampered_signed_prekey_or_kyber_signature_is_refused() {
    let mut p = pair();
    let up = upload(&mut p.bob, 1);
    for field in ["signedPreKey", "kyberPreKey"] {
        let mut b = bundle(&up, 0, false);
        let mut sig = B64.decode(b[field]["signature"].as_str().unwrap()).unwrap();
        sig[5] ^= 0x80;
        b[field]["signature"] = json!(B64.encode(sig));
        let r = start(&mut p.alice, &p.bob, &p.bob_cert, &b);
        assert_eq!(r["error"], "bad_prekey_signature", "{field}: {r}");
    }
}

#[test]
fn a_swapped_prekey_key_is_refused_by_its_signature() {
    let mut p = pair();
    let up = upload(&mut p.bob, 2);
    let mut b = bundle(&up, 0, false);
    // Another real Kyber key from the same device, under the first one's signature.
    b["kyberPreKey"]["key"] = up["kyberPreKeys"][1]["key"].clone();
    assert_eq!(start(&mut p.alice, &p.bob, &p.bob_cert, &b)["error"], "bad_prekey_signature");
}

#[test]
fn malformed_or_missing_keys_are_refused() {
    let mut p = pair();
    let up = upload(&mut p.bob, 1);

    let mut short = bundle(&up, 0, false);
    let key = B64.decode(short["kyberPreKey"]["key"].as_str().unwrap()).unwrap();
    short["kyberPreKey"]["key"] = json!(B64.encode(&key[..key.len() - 7]));
    assert_eq!(start(&mut p.alice, &p.bob, &p.bob_cert, &short)["error"], "bad_key");

    let mut bad_ec = bundle(&up, 0, false);
    bad_ec["signedPreKey"]["key"] = json!(B64.encode([9u8; 33]));
    assert_eq!(start(&mut p.alice, &p.bob, &p.bob_cert, &bad_ec)["ok"], false);

    let mut no_kyber = bundle(&up, 0, false);
    no_kyber.as_object_mut().unwrap().remove("kyberPreKey");
    assert_eq!(start(&mut p.alice, &p.bob, &p.bob_cert, &no_kyber)["error"], "bad_request");

    let mut not_b64 = bundle(&up, 0, false);
    not_b64["identityKey"] = json!("%%%");
    assert_eq!(start(&mut p.alice, &p.bob, &p.bob_cert, &not_b64)["error"], "bad_request");
}

#[test]
fn a_substituted_identity_key_is_refused() {
    let mut p = pair();
    let (mut mallory, mallory_cert) = signal_account("mallory", "pc");
    let up = upload(&mut p.bob, 1);
    let evil = upload(&mut mallory, 1);

    // The server swaps in mallory's whole bundle under bob's certificate.
    let r = start(&mut p.alice, &p.bob, &p.bob_cert, &bundle(&evil, 0, false));
    assert_eq!(r["error"], "identity_mismatch", "{r}");

    // A certificate whose identity key was edited no longer verifies.
    let mut forged = p.bob_cert.clone();
    forged["identityKey"] = mallory_cert["identityKey"].clone();
    assert_eq!(start(&mut p.alice, &p.bob, &forged, &bundle(&up, 0, false))["error"], "bad_certificate");

    // A real certificate, but for another device.
    assert_eq!(start(&mut p.alice, &p.bob, &mallory_cert, &bundle(&up, 0, false))["error"], "bad_certificate");

    // Extra fields aren't accepted (no ambiguity about what was signed).
    let mut extra = p.bob_cert.clone();
    extra["protocols"] = json!(["redvault-olm-v1"]);
    assert_eq!(start(&mut p.alice, &p.bob, &extra, &bundle(&up, 0, false))["error"], "bad_certificate");
}

#[test]
fn a_device_outside_a_verified_list_is_refused() {
    let (mut alice, _) = signal_account("alice", "phone");
    let (mut dave, dave_cert) = signal_account("dave", "phone");
    let up = upload(&mut dave, 1);
    // alice never verified dave's list: no trust on first use.
    let r = start(&mut alice, &dave, &dave_cert, &bundle(&up, 0, false));
    assert_eq!(r["error"], "unverified_device", "{r}");
}

#[test]
fn a_changed_signal_identity_for_the_same_device_is_refused() {
    // The same protocol-3 device, with two different Signal identities. This is what
    // a server replaying an old certificate, or a reinstall that kept the Olm keys, would look like.
    let mut alice = Device::account("alice", "phone");
    alice.ok("signalEnable", json!({}));
    let mut bob = Device::account("bob", "laptop");
    let mut bob_other = bob.clone_state();
    let cert1 = bob.ok("signalEnable", json!({}))["certificate"].clone();
    let cert2 = bob_other.ok("signalEnable", json!({}))["certificate"].clone();
    assert_ne!(cert1["identityKey"], cert2["identityKey"]);
    know(&mut alice, &mut bob);
    let up1 = upload(&mut bob, 1);
    let up2 = upload(&mut bob_other, 1);
    assert_eq!(start(&mut alice, &bob, &cert1, &bundle(&up1, 0, false))["ok"], true);
    let r = start(&mut alice, &bob, &cert2, &bundle(&up2, 0, false));
    assert_eq!(r["error"], "identity_changed", "{r}");
}

// --- Tampering, replay, ordering, wrong recipients ---

#[test]
fn a_tampered_message_is_refused_and_changes_nothing() {
    let mut p = talking();
    let m = send(&mut p.alice, &p.bob, "intact");
    let before = p.bob.state.clone();
    for at in [0usize, 1, 7, 20, 40, 1000] {
        let r = read(&mut p.bob, &p.alice, None, &flip(&m, at));
        assert_eq!(r["ok"], false, "byte {at}: {r}");
        assert!(r.get("state").is_none());
    }
    assert_eq!(p.bob.state, before, "a failed call must not change the stored state");
    assert_eq!(read(&mut p.bob, &p.alice, None, &m)["plaintext"], "intact");
}

#[test]
fn replays_are_refused() {
    let mut p = talking();
    let m = send(&mut p.alice, &p.bob, "once");
    assert_eq!(read(&mut p.bob, &p.alice, None, &m)["plaintext"], "once");
    assert_eq!(read(&mut p.bob, &p.alice, None, &m)["error"], "duplicate");
}

#[test]
fn out_of_order_delivery_opens_every_message_once() {
    let mut p = talking();
    let messages: Vec<Value> = (0..6).map(|i| send(&mut p.alice, &p.bob, &format!("m{i}"))).collect();
    for i in [5usize, 2, 0, 4, 1, 3] {
        assert_eq!(read(&mut p.bob, &p.alice, None, &messages[i])["plaintext"], format!("m{i}"));
    }
    for m in &messages {
        assert_eq!(read(&mut p.bob, &p.alice, None, m)["error"], "duplicate");
    }
}

#[test]
fn a_message_for_one_device_does_not_open_on_another() {
    let mut p = talking();
    let (mut carol, _) = signal_account("carol", "phone");
    know(&mut carol, &mut p.alice);
    let m = send(&mut p.alice, &p.bob, "for bob only");
    let r = read(&mut carol, &p.alice, Some(&p.alice_cert), &m);
    assert_eq!(r["ok"], false, "{r}");
}

#[test]
fn a_message_presented_as_from_another_sender_is_refused() {
    let mut p = talking();
    let (mut carol, carol_cert) = signal_account("carol", "phone");
    know(&mut p.bob, &mut carol);
    let m = send(&mut p.alice, &p.bob, "from alice");
    // The server claims it came from carol.
    let r = read(&mut p.bob, &carol, Some(&carol_cert), &m);
    assert_eq!(r["ok"], false, "{r}");
}

// --- Protocol confusion and downgrade ---

#[test]
fn messages_of_another_protocol_are_refused() {
    let mut p = talking();
    let m = send(&mut p.alice, &p.bob, "x");

    let mut relabelled = m.clone();
    relabelled["protocol"] = json!("redvault-olm-v1");
    assert_eq!(read(&mut p.bob, &p.alice, None, &relabelled)["error"], "protocol_mismatch");

    let mut unlabelled = m.clone();
    unlabelled.as_object_mut().unwrap().remove("protocol");
    assert_eq!(read(&mut p.bob, &p.alice, None, &unlabelled)["error"], "protocol_mismatch");

    let mut unknown_type = m.clone();
    unknown_type["type"] = json!("olm");
    assert_eq!(read(&mut p.bob, &p.alice, None, &unknown_type)["error"], "malformed_message");

    // A Signal message handed to the Olm decrypter is not mistaken for Olm.
    let olm_shaped = json!({ "type": 1, "body": m["body"] });
    assert_eq!(p.bob.op("decrypt", json!({ "sender": p.alice.identity, "message": olm_shaped }))["ok"], false);
}

#[test]
fn once_a_device_speaks_signal_olm_chat_with_it_is_refused_both_ways() {
    let (mut alice, alice_cert) = signal_account("alice", "phone");
    let (mut bob, bob_cert) = signal_account("bob", "laptop");
    know(&mut alice, &mut bob);
    know(&mut bob, &mut alice);

    // Before Signal: an Olm chat message from alice, held back by the server.
    let olm_keys = bob.prekeys(1);
    let held_back = alice.send(&bob.identity, Some(&common::first_otk(&olm_keys)), "old protocol");

    let up = upload(&mut bob, 1);
    assert_eq!(start(&mut alice, &bob, &bob_cert, &bundle(&up, 0, false))["ok"], true);
    let first = send(&mut alice, &bob, "new protocol");
    assert_eq!(read(&mut bob, &alice, Some(&alice_cert), &first)["plaintext"], "new protocol");

    // alice won't send Olm chat to bob any more...
    let olm = alice.op("encrypt", json!({ "recipient": bob.identity, "plaintext": "downgrade?" }));
    assert_eq!(olm["error"], "downgrade_refused");
    // encryptMany (what the apps call) picks Signal for him by itself: never Olm.
    let many = alice.ok("encryptMany", json!({ "plaintext": "downgrade?", "recipients": [{ "identity": bob.identity }] }));
    assert_eq!(many["results"][0]["protocol"], "rv-signal-v1", "{many}");
    assert_eq!(many["results"][0]["message"]["protocol"], "rv-signal-v1");
    // ...and bob won't accept the held-back Olm chat message from alice.
    assert_eq!(bob.read(&alice.identity, &held_back)["error"], "downgrade_refused");
}

#[test]
fn room_keys_still_travel_over_olm_to_a_signal_device() {
    // Groups stay on protocol 3 (decision 4 is open), so Megolm room keys must still pass.
    let (mut alice, _) = signal_account("alice", "phone");
    let (mut bob, bob_cert) = signal_account("bob", "laptop");
    know(&mut alice, &mut bob);
    know(&mut bob, &mut alice);
    let up = upload(&mut bob, 1);
    assert_eq!(start(&mut alice, &bob, &bob_cert, &bundle(&up, 0, false))["ok"], true);
    let olm_keys = bob.prekeys(1);
    let members = json!([alice.identity, bob.identity]);
    alice.ok("groupSetMembers", json!({ "groupId": "g1", "members": members }));
    bob.ok("groupSetMembers", json!({ "groupId": "g1", "members": members }));
    alice.ok("groupSession", json!({ "groupId": "g1" }));
    let shared = alice.op("shareRoomKey", json!({ "groupId": "g1", "recipient": bob.identity, "oneTimeKey": common::first_otk(&olm_keys) }));
    assert_eq!(shared["ok"], true, "{shared}");
    let got = bob.read(&alice.identity, &shared["message"]);
    assert_eq!(got["kind"], "roomKey", "{got}");
}

// --- Forward secrecy and post-compromise security ---

#[test]
fn later_state_does_not_open_messages_already_read() {
    // Forward secrecy, under libsignal's model: a message key is deleted once
    // used, and chain keys only move forward. Someone who steals bob's later
    // state can't reopen what bob already read.
    let mut p = talking();
    let captured: Vec<Value> = (0..3).map(|i| send(&mut p.alice, &p.bob, &format!("secret {i}"))).collect();
    for m in &captured {
        assert_eq!(read(&mut p.bob, &p.alice, None, m)["ok"], true);
    }
    for i in 0..4 {
        let r = send(&mut p.bob, &p.alice, &format!("r{i}"));
        read(&mut p.alice, &p.bob, None, &r);
        let a = send(&mut p.alice, &p.bob, &format!("a{i}"));
        read(&mut p.bob, &p.alice, None, &a);
    }
    let mut stolen = p.bob.clone_state();
    for m in &captured {
        let r = read(&mut stolen, &p.alice, None, m);
        assert_eq!(r["ok"], false, "a stolen later state reopened a read message: {r}");
    }
}

#[test]
fn skipped_message_keys_stay_until_used_documented_limit() {
    // A documented limit, not a bug: libsignal keeps keys for messages that
    // haven't arrived yet (bounded), so out-of-order delivery works. Until
    // such a message arrives, a thief of the current state could open it.
    let mut p = talking();
    let late = send(&mut p.alice, &p.bob, "delayed");
    let next = send(&mut p.alice, &p.bob, "arrives first");
    assert_eq!(read(&mut p.bob, &p.alice, None, &next)["ok"], true);
    let mut stolen = p.bob.clone_state();
    assert_eq!(read(&mut stolen, &p.alice, None, &late)["plaintext"], "delayed");
}

#[test]
fn a_stolen_state_stops_working_after_fresh_ratchet_steps() {
    // Post-compromise security, under libsignal's model. The attacker copies
    // bob's state once, then loses access (no malware left behind). Once
    // alice and bob have exchanged fresh ratchet keys, new messages are out
    // of the attacker's reach.
    let mut p = talking();
    let mut attacker = p.bob.clone_state();

    // While the compromise is fresh, the attacker reads the next message.
    let exposed = send(&mut p.alice, &p.bob, "exposed");
    assert_eq!(read(&mut attacker, &p.alice, None, &exposed)["plaintext"], "exposed");
    assert_eq!(read(&mut p.bob, &p.alice, None, &exposed)["plaintext"], "exposed");

    // The legitimate devices keep talking. Each change of direction is a fresh
    // DH ratchet step, and SPQR advances alongside it.
    for i in 0..6 {
        let b = send(&mut p.bob, &p.alice, &format!("b{i}"));
        assert_eq!(read(&mut p.alice, &p.bob, None, &b)["ok"], true);
        let a = send(&mut p.alice, &p.bob, &format!("a{i}"));
        assert_eq!(read(&mut p.bob, &p.alice, None, &a)["ok"], true);
    }
    let protected = send(&mut p.alice, &p.bob, "protected again");
    let r = read(&mut attacker, &p.alice, None, &protected);
    assert_eq!(r["ok"], false, "the stale stolen state still opened a new message: {r}");
    assert_eq!(read(&mut p.bob, &p.alice, None, &protected)["plaintext"], "protected again");
}

// --- Persistence and the host's "store before send" contract ---

#[test]
fn reusing_an_unsaved_state_is_caught_by_the_receiver() {
    // If the host sends a ciphertext without saving the state that came with it,
    // and then encrypts again from the old state, the same message key is used
    // twice. The engine can't stop that by itself; this is why apps must save
    // the state first. The receiver does refuse the second message.
    let mut p = talking();
    let old = p.alice.clone_state();
    let first = send(&mut p.alice, &p.bob, "one");
    let mut crashed = old;
    let reused = send(&mut crashed, &p.bob, "two");
    assert_eq!(read(&mut p.bob, &p.alice, None, &first)["plaintext"], "one");
    assert_eq!(read(&mut p.bob, &p.alice, None, &reused)["error"], "duplicate");
}

#[test]
fn signal_state_survives_a_restart() {
    let mut p = talking();
    // A "restart": a new Device value built only from what was stored.
    let mut bob = Device { state: p.bob.state.clone(), key: p.bob.key.clone(), identity: p.bob.identity.clone() };
    let m = send(&mut p.alice, &bob, "after restart");
    assert_eq!(read(&mut bob, &p.alice, None, &m)["plaintext"], "after restart");
    let r = send(&mut bob, &p.alice, "still here");
    assert_eq!(read(&mut p.alice, &bob, None, &r)["plaintext"], "still here");
}

#[test]
fn rotating_prekeys_keeps_the_previous_signed_key_for_messages_in_flight() {
    let (mut alice, alice_cert) = signal_account("alice", "phone");
    let (mut bob, bob_cert) = signal_account("bob", "laptop");
    know(&mut alice, &mut bob);
    know(&mut bob, &mut alice);
    let old = upload(&mut bob, 1);
    assert_eq!(start(&mut alice, &bob, &bob_cert, &bundle(&old, 0, false))["ok"], true);
    let in_flight = send(&mut alice, &bob, "sent before rotation");
    let rotated = bob.ok("signalPrekeys", json!({ "count": 0, "rotate": true }))["upload"].clone();
    assert_ne!(rotated["signedPreKey"]["id"], old["signedPreKey"]["id"]);
    assert_eq!(read(&mut bob, &alice, Some(&alice_cert), &in_flight)["plaintext"], "sent before rotation");
    // A second rotation drops the oldest signed key.
    bob.ok("signalPrekeys", json!({ "count": 0, "rotate": true }));
    let (mut carol, carol_cert) = signal_account("carol", "phone");
    know(&mut carol, &mut bob);
    know(&mut bob, &mut carol);
    let mut stale = bundle(&old, 0, true);
    stale["kyberPreKey"] = old["lastResortKyberPreKey"].clone();
    assert_eq!(start(&mut carol, &bob, &bob_cert, &stale)["ok"], true);
    let m = send(&mut carol, &bob, "to a deleted key");
    assert_eq!(read(&mut bob, &carol, Some(&carol_cert), &m)["error"], "unknown_prekey");
}

// --- Devices leaving a list ---

#[test]
fn a_revoked_device_gets_nothing_new() {
    let (mut alice, _) = signal_account("alice", "phone");
    let (mut bob, bob_cert) = signal_account("bob", "laptop");
    let mut bob_phone = Device::new("bob", "phone");
    common::link(&mut bob, &mut bob_phone);
    bob_phone.ok("signalEnable", json!({}));
    let phone_cert = bob_phone.ok("signalEnable", json!({}))["certificate"].clone();
    know(&mut alice, &mut bob);
    let up_laptop = upload(&mut bob, 1);
    let up_phone = upload(&mut bob_phone, 1);
    assert_eq!(start(&mut alice, &bob, &bob_cert, &bundle(&up_laptop, 0, false))["ok"], true);
    assert_eq!(start(&mut alice, &bob_phone, &phone_cert, &bundle(&up_phone, 0, false))["ok"], true);

    // bob's laptop revokes the phone; alice sees the new list.
    let revoked = bob.ok("revokeDevice", json!({ "deviceId": "phone" }));
    alice.ok("verifyDeviceList", json!({ "deviceList": revoked["deviceList"] }));

    let r = alice.op("signalEncrypt", json!({ "peer": bob_phone.identity, "plaintext": "after revocation" }));
    assert_eq!(r["error"], "unlisted_device", "{r}");
    assert_eq!(alice.ok("signalStatus", json!({}))["sessions"], 1);
    // The laptop still works.
    send(&mut alice, &bob, "still fine");
}

// --- Robustness: mutation fuzzing and a property test ---

#[test]
fn thousands_of_mutated_messages_never_crash_or_corrupt_state() {
    // Deterministic mutation of real first and later messages. Every result
    // must be a clean refusal or the original plaintext, the stored state must
    // not change on a refusal, and nothing may panic (a panic would abort the test).
    let mut p = pair();
    let first = send(&mut p.alice, &p.bob, "first");
    let mut seed: u64 = 0x5eed;
    let mut next = || {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        seed
    };
    let bytes = B64.decode(first["body"].as_str().unwrap()).unwrap();
    for _ in 0..1500 {
        let mut b = bytes.clone();
        match next() % 4 {
            0 => {
                let i = (next() as usize) % b.len();
                b[i] ^= 1 << (next() % 8);
            }
            1 => b.truncate((next() as usize) % b.len()),
            2 => b.extend((0..(next() % 64)).map(|_| next() as u8)),
            _ => {
                let i = (next() as usize) % b.len();
                b[i] = next() as u8;
            }
        }
        let mut m = first.clone();
        m["body"] = json!(B64.encode(&b));
        let before = p.bob.state.clone();
        let r = read(&mut p.bob, &p.alice, Some(&p.alice_cert), &m);
        if r["ok"] == false {
            assert_eq!(p.bob.state, before);
        } else {
            assert_eq!(r["plaintext"], "first", "a mutation opened to something else: {r}");
            return; // A mutation in an unauthenticated length field can still decode; then the session exists.
        }
    }
    assert_eq!(read(&mut p.bob, &p.alice, Some(&p.alice_cert), &first)["plaintext"], "first");
}

proptest! {
    #![proptest_config(ProptestConfig { cases: 12, .. ProptestConfig::default() })]

    /// Any interleaving of sends in both directions, delivered in any order
    /// within each direction, opens every message exactly once.
    #[test]
    fn any_delivery_order_opens_every_message_exactly_once(
        script in proptest::collection::vec(any::<bool>(), 1..14),
        shuffle_seed in any::<u64>(),
    ) {
        let mut p = talking();
        let mut to_bob = Vec::new();
        let mut to_alice = Vec::new();
        for (i, from_alice) in script.iter().enumerate() {
            if *from_alice {
                to_bob.push((format!("a{i}"), send(&mut p.alice, &p.bob, &format!("a{i}"))));
            } else {
                to_alice.push((format!("b{i}"), send(&mut p.bob, &p.alice, &format!("b{i}"))));
            }
        }
        let mut s = shuffle_seed | 1;
        let mut shuffle = |v: &mut Vec<(String, Value)>| {
            for i in (1..v.len()).rev() {
                s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                v.swap(i, (s as usize) % (i + 1));
            }
        };
        shuffle(&mut to_bob);
        shuffle(&mut to_alice);
        for (text, m) in &to_bob {
            let r = read(&mut p.bob, &p.alice, None, m);
            prop_assert_eq!(&r["plaintext"], &json!(text), "{}", r);
            prop_assert_eq!(&read(&mut p.bob, &p.alice, None, m)["error"], &json!("duplicate"));
        }
        for (text, m) in &to_alice {
            let r = read(&mut p.alice, &p.bob, None, m);
            prop_assert_eq!(&r["plaintext"], &json!(text), "{}", r);
        }
    }
}
