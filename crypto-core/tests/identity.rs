// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Account keys, signed device lists, linking, revocation, fingerprints and recovery.

mod common;

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use common::{first_otk, link, Device};
use serde_json::{json, Value};

#[test]
fn an_account_starts_with_a_signed_list_of_its_first_device() {
    let mut phone = Device::account("alice", "phone");
    let list = phone.list();
    assert_eq!(list["version"], 1);
    assert_eq!(list["devices"], json!([phone.identity]));
    assert_eq!(phone.ok("deviceList", json!({}))["trusted"], true);
}

#[test]
fn a_trusted_device_links_a_new_one_by_qr_and_others_see_it_added() {
    let mut phone = Device::account("alice", "phone");
    let mut laptop = Device::new("alice", "laptop");
    let mut bob = Device::account("bob", "phone");
    let v1 = phone.list();
    let seen = bob.ok("verifyDeviceList", json!({ "deviceList": v1 }));
    assert_eq!(seen["firstSeen"], true);

    let v2 = link(&mut phone, &mut laptop);
    assert_eq!(v2["version"], 2);
    // The laptop now holds the account key too, and the same list.
    let on_laptop = laptop.ok("deviceList", json!({}));
    assert_eq!(on_laptop["trusted"], true);
    assert_eq!(on_laptop["deviceList"], v2);

    // Bob is told a device was added (a notification), and can now reach it.
    let seen = bob.ok("verifyDeviceList", json!({ "deviceList": v2 }));
    assert_eq!(seen["added"], json!([laptop.identity]));
    assert_eq!(seen["removed"], json!([]));
    let upload = laptop.prekeys(1);
    let message = bob.send(&laptop.identity, Some(&first_otk(&upload)), "hi laptop");
    assert_eq!(laptop.read(&bob.identity, &message)["plaintext"], "hi laptop");
}

#[test]
fn linking_is_refused_for_another_account_a_forged_key_or_a_wrong_nonce() {
    let mut phone = Device::account("alice", "phone");
    let mut stranger = Device::new("mallory", "m1");
    let offer = stranger.ok("linkOffer", json!({}));
    assert_eq!(phone.op("approveDevice", json!({ "qr": offer["qrText"], "verifiedBy": "qr" }))["error"], "wrong_account");

    let mut laptop = Device::new("alice", "laptop");
    let offer = laptop.ok("linkOffer", json!({}))["qr"].clone();
    let mut tampered = offer.clone();
    tampered["oneTimeKey"]["key"] = stranger.prekeys(1)["oneTimeKeys"][0]["key"].clone();
    assert_eq!(phone.op("approveDevice", json!({ "qr": tampered, "verifiedBy": "qr" }))["error"], "bad_prekey_signature");

    // The laptop made a second offer meanwhile: an approval of the first one carries the old nonce.
    let _second = laptop.ok("linkOffer", json!({}));
    let approved = phone.ok("approveDevice", json!({ "qr": offer, "verifiedBy": "qr" }));
    assert_eq!(laptop.read(&phone.identity, &approved["message"])["error"], "bad_link");
    assert_eq!(laptop.op("deviceList", json!({}))["error"], "not_linked");
}

#[test]
fn a_device_the_server_adds_on_its_own_can_not_send_or_receive() {
    let mut alice = Device::account("alice", "phone");
    let mut bob = Device::account("bob", "phone");
    bob.ok("verifyDeviceList", json!({ "deviceList": alice.list() }));
    // The server registers its own "alice" device, which isn't in her signed list.
    let mut fake = Device::new("alice", "server-made");
    let upload = fake.prekeys(1);
    let refused = bob.op("encrypt", json!({ "recipient": fake.identity, "oneTimeKey": first_otk(&upload), "plaintext": "secret" }));
    assert_eq!(refused["error"], "unlisted_device");
    let bob_keys = bob.prekeys(1);
    let from_fake = fake.send(&bob.identity, Some(&first_otk(&bob_keys)), "trust me");
    assert_eq!(bob.read(&fake.identity, &from_fake)["error"], "unlisted_device");
    // Group membership is held to the same lists.
    let refused = bob.op("groupSetMembers", json!({ "groupId": "g", "members": [bob.identity, fake.identity] }));
    assert_eq!(refused["error"], "unlisted_device");
}

#[test]
fn older_lists_conflicting_lists_and_changed_account_keys_are_refused() {
    let mut phone = Device::account("alice", "phone");
    let mut bob = Device::account("bob", "phone");
    let v1 = phone.list();
    let mut fork = phone.clone_state();
    let mut laptop = Device::new("alice", "laptop");
    let v2 = link(&mut phone, &mut laptop);
    bob.ok("verifyDeviceList", json!({ "deviceList": v2 }));
    assert_eq!(bob.op("verifyDeviceList", json!({ "deviceList": v1 }))["error"], "rollback");

    // A second, different version 2 (the server showing different lists to different people).
    let mut tablet = Device::new("alice", "tablet");
    let other_v2 = link(&mut fork, &mut tablet);
    assert_eq!(other_v2["version"], 2);
    assert_eq!(bob.op("verifyDeviceList", json!({ "deviceList": other_v2 }))["error"], "equivocation");

    // A list signed by some other key, e.g. the server's.
    let impostor = Device::account("alice", "impostor").list();
    assert_eq!(bob.op("verifyDeviceList", json!({ "deviceList": impostor }))["error"], "account_key_changed");

    // A forged signature.
    let mut forged = v2.clone();
    forged["devices"].as_array_mut().unwrap().push(tablet.identity.clone());
    assert_eq!(bob.op("verifyDeviceList", json!({ "deviceList": forged }))["error"], "bad_device_list");
}

#[test]
fn a_rotated_account_key_needs_explicit_trust() {
    let mut alice = Device::account("alice", "phone");
    let mut bob = Device::account("bob", "phone");
    bob.ok("verifyDeviceList", json!({ "deviceList": alice.list() }));
    let rotated = alice.ok("rotateAccountKey", json!({}));
    let v2 = rotated["deviceList"].clone();
    assert_eq!(bob.op("verifyDeviceList", json!({ "deviceList": v2 }))["error"], "account_key_changed");
    // Only after Bob accepts it (e.g. having compared safety numbers again).
    bob.ok("trustAccountKey", json!({ "userId": "alice", "accountKey": rotated["accountKey"] }));
    assert_eq!(bob.ok("verifyDeviceList", json!({ "deviceList": v2 }))["firstSeen"], false);
}

#[test]
fn revoked_devices_drop_out_and_everyone_is_told() {
    let mut phone = Device::account("alice", "phone");
    let mut laptop = Device::new("alice", "laptop");
    let mut bob = Device::account("bob", "phone");
    let v2 = link(&mut phone, &mut laptop);
    bob.ok("verifyDeviceList", json!({ "deviceList": v2 }));
    let revoked = phone.ok("revokeDevice", json!({ "deviceId": "laptop" }));
    assert_eq!(revoked["revoked"], json!([laptop.identity]));
    let v3 = revoked["deviceList"].clone();
    let seen = bob.ok("verifyDeviceList", json!({ "deviceList": v3 }));
    assert_eq!(seen["removed"], json!([laptop.identity]));
    let upload = laptop.prekeys(1);
    let refused = bob.op("encrypt", json!({ "recipient": laptop.identity, "oneTimeKey": first_otk(&upload), "plaintext": "x" }));
    assert_eq!(refused["error"], "unlisted_device");
    assert_eq!(phone.op("revokeDevice", json!({ "deviceId": "phone" }))["ok"], false);
}

#[test]
fn safety_numbers_match_on_both_sides_and_the_qr_code_verifies() {
    let mut alice = Device::account("alice", "phone");
    let mut bob = Device::account("bob", "phone");
    let alice_key = alice.list()["accountKey"].clone();
    let bob_key = bob.list()["accountKey"].clone();
    bob.ok("verifyDeviceList", json!({ "deviceList": alice.list() }));
    alice.ok("verifyDeviceList", json!({ "deviceList": bob.list() }));
    let at_alice = alice.ok("fingerprint", json!({ "userId": "bob" }));
    let at_bob = bob.ok("fingerprint", json!({ "userId": "alice" }));
    assert_eq!(at_alice["safetyNumber"], at_bob["safetyNumber"]);
    let groups = at_alice["safetyNumber"].as_array().unwrap();
    assert_eq!(groups.len(), 12);
    assert!(groups.iter().all(|g| g.as_str().unwrap().len() == 5 && g.as_str().unwrap().chars().all(|c| c.is_ascii_digit())));
    assert_eq!(at_alice["verified"], false);

    // Alice scans Bob's code.
    let scanned = alice.ok("verifyFingerprintQr", json!({ "userId": "bob", "qr": at_bob["qr"] }));
    assert_eq!(scanned["verified"], true);
    assert_eq!(alice.ok("fingerprint", json!({ "userId": "bob" }))["verified"], true);

    // A code made against a different key for Alice doesn't verify.
    let mallory_view = bob.ok("fingerprint", json!({ "userId": "alice", "accountKey": bob_key }));
    assert_eq!(alice.ok("verifyFingerprintQr", json!({ "userId": "bob", "qr": mallory_view["qr"] }))["verified"], false);
    // Same keys, same numbers, every time (deterministic).
    assert_eq!(alice.ok("fingerprint", json!({ "userId": "bob" }))["qr"], at_alice["qr"]);
    let _ = alice_key;
}

#[test]
fn the_qr_text_verifies_and_marking_by_hand_keeps_the_device_list() {
    let mut alice = Device::account("alice", "phone");
    let mut bob = Device::account("bob", "phone");
    bob.ok("verifyDeviceList", json!({ "deviceList": alice.list() }));
    alice.ok("verifyDeviceList", json!({ "deviceList": bob.list() }));
    let at_bob = bob.ok("fingerprint", json!({ "userId": "alice" }));
    let text = at_bob["qrText"].as_str().unwrap().to_owned();
    assert!(text.starts_with("RVDK1:"));
    assert_eq!(&text["RVDK1:".len()..], at_bob["qr"].as_str().unwrap());

    // What the camera reads (with stray whitespace) verifies; other codes don't.
    assert_eq!(alice.op("verifyFingerprintQr", json!({ "userId": "bob", "qrText": "RVSN1:123" }))["error"], "bad_request");
    assert_eq!(alice.op("verifyFingerprintQr", json!({ "userId": "bob", "qrText": "RVDK1:%%%" }))["error"], "bad_request");
    assert_eq!(alice.ok("verifyFingerprintQr", json!({ "userId": "bob", "qrText": format!(" {text}\n") }))["verified"], true);

    // Bob compares digits instead and marks Alice by hand; then takes it back.
    assert_eq!(bob.ok("setContactVerified", json!({ "userId": "alice", "verified": true }))["verified"], true);
    assert_eq!(bob.ok("fingerprint", json!({ "userId": "alice" }))["verified"], true);
    assert_eq!(bob.ok("setContactVerified", json!({ "userId": "alice", "verified": false }))["verified"], false);
    assert_eq!(bob.ok("fingerprint", json!({ "userId": "alice" }))["verified"], false);
    // The cached list survived: the same list again isn't "first seen" and adds no devices.
    let again = bob.ok("verifyDeviceList", json!({ "deviceList": alice.list() }));
    assert_eq!(again["firstSeen"], false);
    assert_eq!(again["added"], json!([]));

    // Only for a pinned contact, and only for the key the screen showed.
    assert_eq!(bob.op("setContactVerified", json!({ "userId": "carol", "verified": true }))["error"], "unknown_contact");
    let wrong = bob.list()["accountKey"].clone();
    assert_eq!(bob.op("setContactVerified", json!({ "userId": "alice", "verified": true, "accountKey": wrong }))["error"], "account_key_changed");
    assert_eq!(bob.op("setContactVerified", json!({ "userId": "alice" }))["error"], "bad_request");
}

#[test]
fn a_new_account_key_starts_unverified() {
    let mut alice = Device::account("alice", "phone");
    let mut bob = Device::account("bob", "phone");
    bob.ok("verifyDeviceList", json!({ "deviceList": alice.list() }));
    bob.ok("setContactVerified", json!({ "userId": "alice", "verified": true }));
    // Alice starts over with a new account key: Bob's app is told and must accept it again.
    let mut new_alice = Device::account("alice", "new-phone");
    assert_eq!(bob.op("verifyDeviceList", json!({ "deviceList": new_alice.list() }))["error"], "account_key_changed");
    let new_key = new_alice.list()["accountKey"].clone();
    // Bob's screen shows the number for the new key before he trusts it.
    let preview = bob.ok("fingerprint", json!({ "userId": "alice", "accountKey": new_key }));
    assert_eq!(preview["verified"], false, "the old key's flag doesn't carry over to a new key");
    assert_eq!(preview["keyChanged"], true);
    assert_eq!(bob.ok("fingerprint", json!({ "userId": "alice" }))["keyChanged"], false);
    bob.ok("trustAccountKey", json!({ "userId": "alice", "accountKey": new_key }));
    assert_eq!(bob.ok("fingerprint", json!({ "userId": "alice" }))["verified"], false);
    assert_eq!(bob.ok("fingerprint", json!({ "userId": "alice" }))["safetyNumber"], preview["safetyNumber"]);
    assert_eq!(bob.ok("verifyDeviceList", json!({ "deviceList": new_alice.list() }))["firstSeen"], false);
}

#[test]
fn scanning_their_new_code_accepts_the_new_key_only_when_asked() {
    let mut alice = Device::account("alice", "phone");
    let mut bob = Device::account("bob", "phone");
    bob.ok("verifyDeviceList", json!({ "deviceList": alice.list() }));
    let mut new_alice = Device::account("alice", "new-phone");
    new_alice.ok("verifyDeviceList", json!({ "deviceList": bob.list() }));
    let new_key = new_alice.list()["accountKey"].clone();
    let code = new_alice.ok("fingerprint", json!({ "userId": "bob" }))["qrText"].clone();

    let refused = bob.op("verifyFingerprintQr", json!({ "userId": "bob-sees-alice", "qrText": code }));
    assert_eq!(refused["ok"], false, "no key for an unknown user");
    let not_asked = bob.op("verifyFingerprintQr", json!({ "userId": "alice", "accountKey": new_key, "qrText": code }));
    assert_eq!(not_asked["error"], "account_key_changed");
    let accepted = bob.ok("verifyFingerprintQr", json!({ "userId": "alice", "accountKey": new_key, "qrText": code, "acceptNewKey": true }));
    assert_eq!(accepted["acceptedNewKey"], true);
    assert_eq!(bob.ok("fingerprint", json!({ "userId": "alice" }))["verified"], true);
    assert_eq!(bob.ok("verifyDeviceList", json!({ "deviceList": new_alice.list() }))["firstSeen"], false);

    // A code for some other key never replaces the pinned one, even when asked.
    let mut mallory = Device::account("alice", "mallory");
    mallory.ok("verifyDeviceList", json!({ "deviceList": bob.list() }));
    let mallory_key = mallory.list()["accountKey"].clone();
    let wrong_code = mallory.ok("fingerprint", json!({ "userId": "bob" }))["qrText"].clone();
    let no = bob.ok("verifyFingerprintQr", json!({ "userId": "alice", "accountKey": new_key, "qrText": wrong_code, "acceptNewKey": true }));
    assert_eq!(no["verified"], false);
    let _ = mallory_key;
    assert_eq!(bob.ok("verifyDeviceList", json!({ "deviceList": new_alice.list() }))["firstSeen"], false);
}

#[test]
fn the_recovery_key_restores_the_account_key_onto_a_new_device() {
    let mut phone = Device::account("alice", "phone");
    let recovery = B64.encode([9u8; 32]);
    let backup = phone.ok("exportAccountBackup", json!({ "recoveryKey": recovery }))["backup"].clone();
    let current = phone.list();

    let mut new_phone = Device::new("alice", "new-phone");
    let wrong = new_phone.op("restoreAccountBackup", json!({ "backup": backup, "recoveryKey": B64.encode([8u8; 32]), "deviceList": current }));
    assert_eq!(wrong["error"], "bad_backup");
    let impostor_list = Device::account("alice", "impostor").list();
    let mismatch = new_phone.op("restoreAccountBackup", json!({ "backup": backup, "recoveryKey": recovery, "deviceList": impostor_list }));
    assert_eq!(mismatch["error"], "account_key_changed");

    let restored = new_phone.ok("restoreAccountBackup", json!({ "backup": backup, "recoveryKey": recovery, "deviceList": current }));
    let v2 = restored["deviceList"].clone();
    assert_eq!(v2["version"], 2);
    assert_eq!(v2["accountKey"], current["accountKey"]);
    assert!(v2["devices"].as_array().unwrap().contains(&new_phone.identity));
    // Another account's backup can't be opened as this one.
    let mut bobs_device = Device::new("bob", "x");
    let other = bobs_device.op("restoreAccountBackup", json!({ "backup": backup, "recoveryKey": recovery, "deviceList": current }));
    assert_eq!(other["ok"], false);
    let _: Value = v2;
}
