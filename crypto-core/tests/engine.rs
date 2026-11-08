// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! The milestone scenarios against the engine itself (every binding passes
//! the same JSON to the same function, so these hold for all of them; the
//! cross-platform run in interop/ checks the bindings agree byte for byte).

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use redvault_e2ee::engine::handle;
use serde_json::{json, Value};

struct Device {
    state: String,
    key: String,
    identity: Value,
}

fn call(request: Value) -> Value {
    serde_json::from_str(&handle(&request.to_string())).unwrap()
}

impl Device {
    fn new(user: &str, device: &str) -> Device {
        let mut key = [0u8; 32];
        getrandom::fill(&mut key).unwrap();
        let key = B64.encode(key);
        let created = call(json!({ "op": "createDevice", "userId": user, "deviceId": device, "pickleKey": key }));
        assert_eq!(created["ok"], true, "{created}");
        Device { state: created["state"].as_str().unwrap().to_owned(), key, identity: created["identity"].clone() }
    }

    /// Runs an op; on success the new state replaces the old one (as the platform persists it).
    fn op(&mut self, op: &str, mut fields: Value) -> Value {
        fields["op"] = json!(op);
        fields["state"] = json!(self.state);
        fields["pickleKey"] = json!(self.key);
        let response = call(fields);
        if response["ok"] == true {
            self.state = response["state"].as_str().unwrap().to_owned();
        }
        response
    }

    fn prekeys(&mut self, count: u64) -> Value {
        let response = self.op("prekeys", json!({ "count": count }));
        assert_eq!(response["ok"], true, "{response}");
        response["upload"].clone()
    }

    fn send(&mut self, to: &Value, otk: Option<&Value>, text: &str) -> Value {
        let mut request = json!({ "recipient": to, "plaintext": text });
        if let Some(otk) = otk {
            request["oneTimeKey"] = otk.clone();
        }
        let response = self.op("encrypt", request);
        assert_eq!(response["ok"], true, "{response}");
        response["message"].clone()
    }

    fn read(&mut self, from: &Value, message: &Value) -> Value {
        self.op("decrypt", json!({ "sender": from, "message": message }))
    }

    fn read_ok(&mut self, from: &Value, message: &Value) -> String {
        let response = self.read(from, message);
        assert_eq!(response["ok"], true, "{response}");
        response["plaintext"].as_str().unwrap().to_owned()
    }
}

fn first_otk(upload: &Value) -> Value {
    upload["oneTimeKeys"][0].clone()
}

#[test]
fn alice_starts_a_session_while_bob_is_offline_and_bob_reads_everything_later() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let bundle = bob.prekeys(5);
    // Bob is offline: Alice only has his published, signed one-time key.
    let m1 = alice.send(&bob.identity, Some(&first_otk(&bundle)), "one");
    let m2 = alice.send(&bob.identity, None, "two");
    let m3 = alice.send(&bob.identity, None, "three");
    assert_eq!(m1["type"], 0, "the first messages are pre-key messages until Bob replies");
    assert_eq!(bob.read_ok(&alice.identity, &m1), "one");
    assert_eq!(bob.read_ok(&alice.identity, &m2), "two");
    assert_eq!(bob.read_ok(&alice.identity, &m3), "three");
    // Bob replies; Alice's next message is a normal one (the ratchet turned).
    let reply = bob.send(&alice.identity, None, "hi alice");
    assert_eq!(alice.read_ok(&bob.identity, &reply), "hi alice");
    let m4 = alice.send(&bob.identity, None, "four");
    assert_eq!(m4["type"], 1);
    assert_eq!(bob.read_ok(&alice.identity, &m4), "four");
}

#[test]
fn signed_prekeys_are_checked() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let mallory = Device::new("mallory", "M1");
    let bundle = bob.prekeys(3);

    // A one-time key whose signature was altered.
    let mut tampered = first_otk(&bundle);
    // vodozemac writes keys and signatures as unpadded base64.
    let unpadded = base64::engine::general_purpose::STANDARD_NO_PAD;
    let mut signature = unpadded.decode(tampered["signature"].as_str().unwrap()).unwrap();
    signature[0] ^= 1;
    tampered["signature"] = json!(unpadded.encode(signature));
    let refused = alice.op("encrypt", json!({ "recipient": bob.identity, "oneTimeKey": tampered, "plaintext": "x" }));
    assert_eq!(refused["error"], "bad_prekey_signature");

    // Bob's genuine key presented for Mallory's identity (a server swapping identities).
    let swapped = alice.op("encrypt", json!({ "recipient": mallory.identity, "oneTimeKey": first_otk(&bundle), "plaintext": "x" }));
    assert_eq!(swapped["error"], "bad_prekey_signature");

    // A malformed identity.
    let mut bad = bob.identity.clone();
    bad["curve25519"] = json!("not a key");
    assert_eq!(alice.op("encrypt", json!({ "recipient": bad, "oneTimeKey": first_otk(&bundle), "plaintext": "x" }))["error"], "bad_request");

    // No session and no one-time key.
    assert_eq!(alice.op("encrypt", json!({ "recipient": bob.identity, "plaintext": "x" }))["error"], "no_session");
}

#[test]
fn out_of_order_lost_and_duplicate_messages() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let bundle = bob.prekeys(1);
    let first = alice.send(&bob.identity, Some(&first_otk(&bundle)), "0");
    assert_eq!(bob.read_ok(&alice.identity, &first), "0");
    let reply = bob.send(&alice.identity, None, "ok");
    alice.read_ok(&bob.identity, &reply);

    let messages: Vec<Value> = (1..=6).map(|i| alice.send(&bob.identity, None, &i.to_string())).collect();
    // Out of order, with message 4 lost for good.
    for i in [5, 2, 1, 6, 3] {
        assert_eq!(bob.read_ok(&alice.identity, &messages[i - 1]), i.to_string());
    }
    // A replay of one already read is refused, and the session still works.
    assert_eq!(bob.read(&alice.identity, &messages[1])["error"], "duplicate");
    // So is a replay of the very first pre-key message.
    assert_eq!(bob.read(&alice.identity, &first)["error"], "duplicate");
    let later = alice.send(&bob.identity, None, "7");
    assert_eq!(bob.read_ok(&alice.identity, &later), "7");
}

#[test]
fn a_long_conversation_keeps_ratcheting() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let bundle = bob.prekeys(1);
    let first = alice.send(&bob.identity, Some(&first_otk(&bundle)), "start");
    bob.read_ok(&alice.identity, &first);
    for round in 0..150 {
        let a = alice.send(&bob.identity, None, &format!("a{round}"));
        let b = bob.send(&alice.identity, None, &format!("b{round}"));
        assert_eq!(bob.read_ok(&alice.identity, &a), format!("a{round}"));
        assert_eq!(alice.read_ok(&bob.identity, &b), format!("b{round}"));
    }
}

#[test]
fn state_survives_a_restart_and_a_failed_call_changes_nothing() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let bundle = bob.prekeys(1);
    let m1 = alice.send(&bob.identity, Some(&first_otk(&bundle)), "before restart");

    // Bob "restarts": all that survives is his persisted state and the key from secure storage.
    let mut restarted = Device { state: bob.state.clone(), key: bob.key.clone(), identity: bob.identity.clone() };
    assert_eq!(restarted.read_ok(&alice.identity, &m1), "before restart");
    let m2 = alice.send(&bob.identity, None, "after restart");
    assert_eq!(restarted.read_ok(&alice.identity, &m2), "after restart");

    // A failed call returns no state: the caller keeps the previous one.
    let before = restarted.state.clone();
    let failure = restarted.read(&alice.identity, &json!({ "type": 1, "body": B64.encode(b"garbage") }));
    assert_eq!(failure["ok"], false);
    assert!(failure.get("state").is_none());
    assert_eq!(restarted.state, before);

    // The wrong storage key can't open the state.
    let wrong = call(json!({ "op": "identity", "state": restarted.state, "pickleKey": B64.encode([7u8; 32]) }));
    assert_eq!(wrong["error"], "bad_state");
}

#[test]
fn persisting_before_sending_means_no_message_key_is_used_twice() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let bundle = bob.prekeys(1);
    let first = alice.send(&bob.identity, Some(&first_otk(&bundle)), "0");
    bob.read_ok(&alice.identity, &first);
    let reply = bob.send(&alice.identity, None, "ok");
    alice.read_ok(&bob.identity, &reply);

    // Alice encrypts; the new state is persisted, then she "crashes" before sending.
    let lost = alice.send(&bob.identity, None, "lost in the crash");
    // After the restart she continues from the persisted state: a new message key.
    let next = alice.send(&bob.identity, None, "after the crash");
    assert_ne!(lost["body"], next["body"]);
    assert_eq!(bob.read_ok(&alice.identity, &next), "after the crash");
}

#[test]
fn payloads_name_both_devices_and_device_keys_are_pinned() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let mut carol = Device::new("carol", "C1");
    let bob_bundle = bob.prekeys(1);
    let carol_bundle = carol.prekeys(1);
    let to_bob = alice.send(&bob.identity, Some(&first_otk(&bob_bundle)), "for bob");
    let _ = alice.send(&carol.identity, Some(&first_otk(&carol_bundle)), "for carol");

    // Bob is told (by a lying server) the message came from Carol's user id with Alice's keys.
    let mut liar = alice.identity.clone();
    liar["userId"] = json!("carol");
    assert_eq!(bob.read(&liar, &to_bob)["error"], "identity_mismatch");
    assert_eq!(bob.read_ok(&alice.identity, &to_bob), "for bob");
    // Once seen, Alice's device key stays Alice's.
    let mut swapped = alice.identity.clone();
    swapped["userId"] = json!("someone-else");
    let again = alice.send(&bob.identity, None, "again");
    assert_eq!(bob.read(&swapped, &again)["error"], "identity_mismatch");
}

#[test]
fn every_device_of_a_user_gets_its_own_session() {
    let mut alice = Device::new("alice", "A1");
    let mut bob_phone = Device::new("bob", "B-phone");
    let mut bob_laptop = Device::new("bob", "B-laptop");
    let phone_keys = bob_phone.prekeys(1);
    let laptop_keys = bob_laptop.prekeys(1);
    let to_phone = alice.send(&bob_phone.identity, Some(&first_otk(&phone_keys)), "hello");
    let to_laptop = alice.send(&bob_laptop.identity, Some(&first_otk(&laptop_keys)), "hello");
    assert_ne!(to_phone["body"], to_laptop["body"]);
    assert_eq!(bob_phone.read_ok(&alice.identity, &to_phone), "hello");
    assert_eq!(bob_laptop.read_ok(&alice.identity, &to_laptop), "hello");
    // A message for the phone means nothing to the laptop.
    assert_eq!(bob_laptop.read(&alice.identity, &to_phone)["ok"], false);
}

// --- Groups (Megolm) ---

struct Group {
    id: &'static str,
}

impl Group {
    fn members(&self, devices: &mut [&mut Device], members: &[&Value]) {
        for device in devices.iter_mut() {
            let response = device.op("groupSetMembers", json!({ "groupId": self.id, "members": members }));
            assert_eq!(response["ok"], true, "{response}");
        }
    }

    /// `sender` makes sure every member has its current session key (over Olm), then sends.
    fn send(&self, sender: &mut Device, recipients: &mut [(&mut Device, Option<Value>)], text: &str) -> Value {
        let session = sender.op("groupSession", json!({ "groupId": self.id }));
        assert_eq!(session["ok"], true, "{session}");
        for (recipient, otk) in recipients.iter_mut() {
            let unshared: Vec<Value> = session["unshared"].as_array().unwrap().clone();
            if !unshared.contains(&recipient.identity) {
                continue;
            }
            let mut request = json!({ "groupId": self.id, "recipient": recipient.identity });
            if let Some(otk) = otk.take() {
                request["oneTimeKey"] = otk;
            }
            let shared = sender.op("shareRoomKey", request);
            assert_eq!(shared["ok"], true, "{shared}");
            let received = recipient.read(&sender.identity, &shared["message"]);
            assert_eq!(received["kind"], "roomKey", "{received}");
        }
        let message = sender.op("groupEncrypt", json!({ "groupId": self.id, "plaintext": text }));
        assert_eq!(message["ok"], true, "{message}");
        message
    }
}

fn read_group(device: &mut Device, message: &Value) -> Value {
    device.op(
        "groupDecrypt",
        json!({ "groupId": message["groupId"], "sessionId": message["sessionId"], "ciphertext": message["ciphertext"] }),
    )
}

#[test]
fn group_messages_carry_an_authenticated_sender_and_removal_revokes_access() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let mut carol = Device::new("carol", "C1");
    let bob_keys = bob.prekeys(2);
    let carol_keys = carol.prekeys(2);
    let group = Group { id: "group-1" };
    let (a, b, c) = (alice.identity.clone(), bob.identity.clone(), carol.identity.clone());
    group.members(&mut [&mut alice, &mut bob, &mut carol], &[&a, &b, &c]);

    let hello = group.send(
        &mut alice,
        &mut [(&mut bob, Some(first_otk(&bob_keys))), (&mut carol, Some(first_otk(&carol_keys)))],
        "hello everyone",
    );
    let at_bob = read_group(&mut bob, &hello);
    assert_eq!(at_bob["plaintext"], "hello everyone");
    assert_eq!(at_bob["sender"]["userId"], "alice");
    assert_eq!(read_group(&mut carol, &hello)["plaintext"], "hello everyone");
    // Alice reads her own messages too.
    assert_eq!(read_group(&mut alice, &hello)["plaintext"], "hello everyone");
    // A replayed group message is refused.
    assert_eq!(read_group(&mut bob, &hello)["error"], "duplicate");

    // Carol also sends (her own session, its key sent to each member over Olm).
    let alice_keys = alice.prekeys(1);
    let from_carol = group.send(
        &mut carol,
        &mut [(&mut alice, Some(first_otk(&alice_keys))), (&mut bob, Some(bob_keys["oneTimeKeys"][1].clone()))],
        "carol here",
    );
    assert_eq!(read_group(&mut bob, &from_carol)["sender"]["userId"], "carol");

    // Carol is removed. Alice's sending session is replaced and never reaches Carol;
    // Carol's own session is revoked everywhere.
    group.members(&mut [&mut alice, &mut bob], &[&a, &b]);
    let after = group.send(&mut alice, &mut [(&mut bob, None)], "carol is gone");
    assert_ne!(after["sessionId"], hello["sessionId"]);
    assert_eq!(read_group(&mut bob, &after)["plaintext"], "carol is gone");
    assert_eq!(read_group(&mut carol, &after)["error"], "unknown_session");
    // Carol keeps sending with her old session: Bob refuses it now.
    let late = carol.op("groupEncrypt", json!({ "groupId": "group-1", "plaintext": "still here?" }));
    assert_eq!(late["ok"], true);
    assert_eq!(read_group(&mut bob, &late)["error"], "revoked");
}

#[test]
fn a_group_key_only_counts_from_a_member_device_and_for_its_own_group() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let mut mallory = Device::new("mallory", "M1");
    let bob_keys = bob.prekeys(2);
    let group = Group { id: "group-2" };
    let (a, b) = (alice.identity.clone(), bob.identity.clone());
    group.members(&mut [&mut alice, &mut bob], &[&a, &b]);

    // Mallory (not a member) sends Bob a key for her own session in this group.
    let m = mallory.identity.clone();
    mallory.op("groupSetMembers", json!({ "groupId": "group-2", "members": [m, b.clone()] }));
    mallory.op("groupSession", json!({ "groupId": "group-2" }));
    let shared = mallory.op("shareRoomKey", json!({ "groupId": "group-2", "recipient": b, "oneTimeKey": first_otk(&bob_keys) }));
    assert_eq!(bob.read(&mallory.identity, &shared["message"])["error"], "not_a_member");

    // A real message presented as belonging to another group.
    let hello = group.send(&mut alice, &mut [(&mut bob, Some(bob_keys["oneTimeKeys"][1].clone()))], "hi");
    let mut moved = hello.clone();
    moved["groupId"] = json!("group-3");
    assert_eq!(read_group(&mut bob, &moved)["ok"], false);
    assert_eq!(read_group(&mut bob, &hello)["plaintext"], "hi");
}

#[test]
fn a_new_member_can_not_read_messages_from_before_they_got_the_key() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let mut dave = Device::new("dave", "D1");
    let bob_keys = bob.prekeys(1);
    let dave_keys = dave.prekeys(1);
    let group = Group { id: "group-4" };
    let (a, b, d) = (alice.identity.clone(), bob.identity.clone(), dave.identity.clone());
    group.members(&mut [&mut alice, &mut bob], &[&a, &b]);
    let early = group.send(&mut alice, &mut [(&mut bob, Some(first_otk(&bob_keys)))], "before dave");

    group.members(&mut [&mut alice, &mut bob, &mut dave], &[&a, &b, &d]);
    let later = group.send(&mut alice, &mut [(&mut bob, None), (&mut dave, Some(first_otk(&dave_keys)))], "after dave");
    assert_eq!(read_group(&mut dave, &later)["plaintext"], "after dave");
    // Same session, but Dave's copy of the key starts at the next message.
    assert_eq!(early["sessionId"], later["sessionId"]);
    assert_eq!(read_group(&mut dave, &early)["error"], "unknown_index");
}

#[test]
fn forward_secrecy_a_stolen_later_state_does_not_open_earlier_messages() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let bundle = bob.prekeys(1);
    let first = alice.send(&bob.identity, Some(&first_otk(&bundle)), "0");
    bob.read_ok(&alice.identity, &first);
    let reply = bob.send(&alice.identity, None, "ok");
    alice.read_ok(&bob.identity, &reply);
    let earlier = alice.send(&bob.identity, None, "an earlier secret");
    assert_eq!(bob.read_ok(&alice.identity, &earlier), "an earlier secret");
    // Later, an attacker copies Bob's whole state (and its storage key).
    let mut stolen = Device { state: bob.state.clone(), key: bob.key.clone(), identity: bob.identity.clone() };
    // That message's key was deleted when Bob read it: the copy can't open it.
    assert_eq!(stolen.read(&alice.identity, &earlier)["ok"], false);
}

#[test]
fn post_compromise_recovery_a_stale_copy_stops_working_after_the_ratchet_turns() {
    let mut alice = Device::new("alice", "A1");
    let mut bob = Device::new("bob", "B1");
    let bundle = bob.prekeys(1);
    let first = alice.send(&bob.identity, Some(&first_otk(&bundle)), "0");
    bob.read_ok(&alice.identity, &first);
    let reply = bob.send(&alice.identity, None, "ok");
    alice.read_ok(&bob.identity, &reply);
    // An attacker copies Bob's state now (a one-off compromise)...
    let mut stolen = Device { state: bob.state.clone(), key: bob.key.clone(), identity: bob.identity.clone() };
    let while_compromised = alice.send(&bob.identity, None, "read by the attacker");
    assert_eq!(stolen.read_ok(&alice.identity, &while_compromised), "read by the attacker");
    assert_eq!(bob.read_ok(&alice.identity, &while_compromised), "read by the attacker");
    // ...then Bob replies (a fresh ratchet key the attacker never sees) and Alice answers it.
    let heal = bob.send(&alice.identity, None, "new ratchet key");
    alice.read_ok(&bob.identity, &heal);
    let after = alice.send(&bob.identity, None, "after recovery");
    assert_eq!(bob.read_ok(&alice.identity, &after), "after recovery");
    // A passive attacker with only the old copy can't follow.
    assert_eq!(stolen.read(&alice.identity, &after)["ok"], false);
}
