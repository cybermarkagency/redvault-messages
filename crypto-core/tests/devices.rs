// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Per-device identities for existing accounts (docs/multi-device-threat-model.md): pairing by
//! comparing codes, the legacy certificate on the account key, the legacy key carried to new
//! devices, and one message to many devices.

mod common;

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use common::{call, first_otk, link, Device};
use serde_json::{json, Value};

const VECTORS: &str = include_str!("../../desktop/tests/vectors/message_signature_vectors.json");

fn legacy_account(index: usize) -> (String, String) {
    let vectors: Value = serde_json::from_str(VECTORS).unwrap();
    let account = &vectors["accounts"][index];
    (account["accountPrivateKey"].as_str().unwrap().to_owned(), account["signingPublicKey"].as_str().unwrap().to_owned())
}

/// An existing account's first per-device identity, certified by its legacy key.
fn migrated_account(user: &str, device: &str, legacy_key: &str) -> Device {
    let mut key = [0u8; 32];
    getrandom::fill(&mut key).unwrap();
    let key = B64.encode(key);
    let created = call(json!({ "op": "createAccount", "userId": user, "deviceId": device, "pickleKey": key, "legacyAccountKey": legacy_key }));
    assert_eq!(created["ok"], true, "{created}");
    Device { state: created["state"].as_str().unwrap().to_owned(), key, identity: created["identity"].clone() }
}

struct Pairing {
    new_sas: String,
    trusted_sas: String,
    new_codes: Value,
    trusted_codes: Value,
}

/// Steps 1-3 of src/pairing.rs, with the server relaying each message unchanged.
fn compare_codes(trusted: &Device, new: &Device, pairing_id: &str) -> Pairing {
    let n = call(json!({ "op": "sasStart", "pairingId": pairing_id, "role": "new" }));
    let t = call(json!({ "op": "sasStart", "pairingId": pairing_id, "role": "trusted" }));
    assert!(n.get("publicKey").is_none(), "the new device only commits at first");
    let revealed = call(json!({ "op": "sasReveal", "sasId": n["sasId"] }));
    let mut request = json!({ "op": "sasEstablish", "newDevice": new.identity, "trustedDevice": trusted.identity });
    request["sasId"] = n["sasId"].clone();
    request["theirPublicKey"] = t["publicKey"].clone();
    let new_codes = call(request.clone());
    request["sasId"] = t["sasId"].clone();
    request["theirPublicKey"] = revealed["publicKey"].clone();
    request["theirCommitment"] = n["commitment"].clone();
    let trusted_codes = call(request);
    assert_eq!(new_codes["ok"], true, "{new_codes}");
    assert_eq!(trusted_codes["ok"], true, "{trusted_codes}");
    Pairing {
        new_sas: n["sasId"].as_str().unwrap().to_owned(),
        trusted_sas: t["sasId"].as_str().unwrap().to_owned(),
        new_codes: new_codes["decimals"].clone(),
        trusted_codes: trusted_codes["decimals"].clone(),
    }
}

/// Step 4, after the person said the codes match.
fn exchange_macs(trusted: &mut Device, new: &mut Device, pairing: &Pairing) {
    let new_mac = new.ok("sasMac", json!({ "sasId": pairing.new_sas }))["mac"].clone();
    let trusted_mac = trusted.ok("sasMac", json!({ "sasId": pairing.trusted_sas }))["mac"].clone();
    trusted.ok("sasConfirm", json!({ "sasId": pairing.trusted_sas, "mac": new_mac }));
    new.ok("sasConfirm", json!({ "sasId": pairing.new_sas, "mac": trusted_mac }));
}

#[test]
fn the_engine_derives_the_same_legacy_signing_key_as_the_apps() {
    for index in 0..2 {
        let (legacy, signing_public) = legacy_account(index);
        let mut phone = migrated_account("alice", "phone", &legacy);
        let list = phone.list();
        assert_eq!(list["legacyCertificate"]["legacySigningKey"], signing_public, "account {index}");
    }
}

#[test]
fn contacts_who_pinned_the_legacy_key_accept_only_a_certified_account_key() {
    let (legacy, signing_public) = legacy_account(0);
    let mut alice = migrated_account("alice", "phone", &legacy);
    let mut bob = Device::account("bob", "phone");
    let seen = bob.ok("verifyDeviceList", json!({ "deviceList": alice.list(), "legacySigningKey": signing_public }));
    assert_eq!(seen["legacyCertified"], true);

    // A server that makes up a new account key can't produce the certificate...
    let mut mallory = Device::account("alice", "evil");
    let mut carol = Device::account("carol", "phone");
    let uncertified = carol.op("verifyDeviceList", json!({ "deviceList": mallory.list(), "legacySigningKey": signing_public }));
    assert_eq!(uncertified["error"], "uncertified_account_key");
    // ...nor borrow alice's certificate for its own key...
    let mut borrowed = mallory.list();
    borrowed["legacyCertificate"] = alice.list()["legacyCertificate"].clone();
    assert_eq!(carol.op("verifyDeviceList", json!({ "deviceList": borrowed }))["error"], "bad_device_list");
    // ...nor certify with another legacy key.
    let (other_legacy, _) = legacy_account(1);
    let mut other = migrated_account("alice", "evil2", &other_legacy);
    let wrong = carol.op("verifyDeviceList", json!({ "deviceList": other.list(), "legacySigningKey": signing_public }));
    assert_eq!(wrong["error"], "uncertified_account_key");
}

#[test]
fn the_certificate_survives_list_changes_and_is_renewed_on_rotation() {
    let (legacy, signing_public) = legacy_account(0);
    let mut phone = migrated_account("alice", "phone", &legacy);
    let mut laptop = Device::new("alice", "laptop");
    let v2 = link(&mut phone, &mut laptop);
    assert_eq!(v2["legacyCertificate"]["legacySigningKey"], signing_public);
    let v3 = phone.ok("revokeDevice", json!({ "deviceId": "laptop" }))["deviceList"].clone();
    assert_eq!(v3["legacyCertificate"]["legacySigningKey"], signing_public);
    let rotated = phone.ok("rotateAccountKey", json!({}));
    let v4 = rotated["deviceList"].clone();
    assert_ne!(v4["legacyCertificate"]["signature"], v3["legacyCertificate"]["signature"]);

    let mut bob = Device::account("bob", "phone");
    bob.ok("trustAccountKey", json!({ "userId": "alice", "accountKey": rotated["accountKey"] }));
    let seen = bob.ok("verifyDeviceList", json!({ "deviceList": v4, "legacySigningKey": signing_public }));
    assert_eq!(seen["legacyCertified"], true);
}

#[test]
fn a_device_that_lost_a_concurrent_list_change_adopts_the_winner_and_redoes_its_own() {
    let mut phone = Device::account("alice", "phone");
    let mut laptop = Device::new("alice", "laptop");
    link(&mut phone, &mut laptop);
    let mut x = Device::new("alice", "x");
    let mut y = Device::new("alice", "y");
    let offer_x = x.ok("linkOffer", json!({}));
    let offer_y = y.ok("linkOffer", json!({}));
    // Both trusted devices sign version 3; the server keeps the phone's.
    let won = phone.ok("approveDevice", json!({ "qr": offer_x["qrText"], "verifiedBy": "qr" }))["deviceList"].clone();
    let lost = laptop.ok("approveDevice", json!({ "qr": offer_y["qrText"], "verifiedBy": "qr" }))["deviceList"].clone();
    assert_eq!(won["version"], 3);
    assert_eq!(lost["version"], 3);
    let adopted = laptop.ok("verifyDeviceList", json!({ "deviceList": won }));
    assert_eq!(adopted["replacedUnpublished"], true);
    assert_eq!(adopted["thisDeviceListed"], true);
    let redo = laptop.ok("approveDevice", json!({ "qr": offer_y["qrText"], "verifiedBy": "qr" }))["deviceList"].clone();
    assert_eq!(redo["version"], 4);
    assert_eq!(redo["devices"].as_array().unwrap().len(), 4);
    // Older versions are still refused.
    assert_eq!(laptop.op("verifyDeviceList", json!({ "deviceList": won }))["error"], "rollback");
    assert_eq!(laptop.op("verifyDeviceList", json!({ "deviceList": lost }))["error"], "rollback");
}

#[test]
fn a_device_approved_by_code_can_be_approved_again_after_a_lost_race() {
    let mut phone = Device::account("alice", "phone");
    let mut laptop = Device::new("alice", "laptop");
    link(&mut phone, &mut laptop);
    let mut tablet = Device::new("alice", "tablet");
    let pairing = compare_codes(&phone, &tablet, "pair-race");
    exchange_macs(&mut phone, &mut tablet, &pairing);
    let offer = tablet.ok("linkOffer", json!({}));
    // The phone's approval loses to a change the laptop made at the same version.
    phone.ok("approveDevice", json!({ "qr": offer["qrText"], "verifiedBy": "sas" }));
    let mut other = Device::new("alice", "other");
    let other_offer = other.ok("linkOffer", json!({}));
    let winner = laptop.ok("approveDevice", json!({ "qr": other_offer["qrText"], "verifiedBy": "qr" }))["deviceList"].clone();
    phone.ok("verifyDeviceList", json!({ "deviceList": winner }));
    let redo = phone.ok("approveDevice", json!({ "qr": offer["qrText"], "verifiedBy": "sas" }));
    assert_eq!(redo["deviceList"]["version"], 4);
    assert_eq!(tablet.read(&phone.identity, &redo["message"])["kind"], "link");
    // Once a list with the tablet is adopted, the comparison is used up.
    phone.ok("verifyDeviceList", json!({ "deviceList": redo["deviceList"] }));
    let mut impostor = Device::new("alice", "tablet-2");
    let again = impostor.ok("linkOffer", json!({}));
    assert_eq!(phone.op("approveDevice", json!({ "qr": again["qrText"], "verifiedBy": "sas" }))["error"], "not_verified");
}

#[test]
fn a_linked_device_and_a_restored_device_receive_the_legacy_key() {
    let (legacy, _) = legacy_account(0);
    let mut phone = migrated_account("alice", "phone", &legacy);
    assert_eq!(phone.ok("legacyKeys", json!({}))["legacyAccountKey"], legacy);

    let mut laptop = Device::new("alice", "laptop");
    assert_eq!(laptop.ok("legacyKeys", json!({}))["legacyAccountKey"], Value::Null);
    link(&mut phone, &mut laptop);
    assert_eq!(laptop.ok("legacyKeys", json!({}))["legacyAccountKey"], legacy);

    let recovery = B64.encode([7u8; 32]);
    let backup = phone.ok("exportAccountBackup", json!({ "recoveryKey": recovery }))["backup"].clone();
    let mut new_phone = Device::new("alice", "new-phone");
    let restored = new_phone.ok("restoreAccountBackup", json!({ "backup": backup, "recoveryKey": recovery, "deviceList": phone.list() }));
    assert_eq!(restored["hasLegacyKey"], true);
    assert_eq!(new_phone.ok("legacyKeys", json!({}))["legacyAccountKey"], legacy);
}

#[test]
fn a_restore_whose_legacy_key_does_not_match_the_certificate_is_refused() {
    use chacha20poly1305::aead::{Aead, KeyInit, Payload};
    use chacha20poly1305::{XChaCha20Poly1305, XNonce};

    let (legacy, _) = legacy_account(0);
    let (other_legacy, _) = legacy_account(1);
    let mut phone = migrated_account("alice", "phone", &legacy);
    let recovery_bytes = [3u8; 32];
    let recovery = B64.encode(recovery_bytes);
    let backup = B64.decode(phone.ok("exportAccountBackup", json!({ "recoveryKey": recovery }))["backup"].as_str().unwrap()).unwrap();

    // Someone holding the recovery key rewrites the backup with the right account key but another legacy key.
    assert_eq!(backup[0], 2);
    let cipher = XChaCha20Poly1305::new_from_slice(&recovery_bytes).unwrap();
    let aad = b"redvault-account-backup-v1\0alice";
    let nonce = XNonce::try_from(&backup[1..25]).unwrap();
    let opened = cipher.decrypt(&nonce, Payload { msg: &backup[25..], aad }).unwrap();
    let mut contents: Value = serde_json::from_slice(&opened).unwrap();
    assert_eq!(contents["legacyAccountKey"], legacy);
    contents["legacyAccountKey"] = json!(other_legacy);
    let mut rewritten = backup[..25].to_vec();
    rewritten.extend(cipher.encrypt(&nonce, Payload { msg: contents.to_string().as_bytes(), aad }).unwrap());

    let mut new_phone = Device::new("alice", "new-phone");
    let refused = new_phone.op(
        "restoreAccountBackup",
        json!({ "backup": B64.encode(rewritten), "recoveryKey": recovery, "deviceList": phone.list() }),
    );
    assert_eq!(refused["error"], "legacy_key_mismatch", "{refused}");
}

#[test]
fn pairing_by_comparing_codes_links_exactly_the_verified_device() {
    let mut phone = Device::account("alice", "phone");
    let mut laptop = Device::new("alice", "laptop");
    let pairing = compare_codes(&phone, &laptop, "pair-1");
    assert_eq!(pairing.new_codes, pairing.trusted_codes, "both screens show the same numbers");
    for code in pairing.new_codes.as_array().unwrap() {
        let code = code.as_u64().unwrap();
        assert!((1000..=9191).contains(&code));
    }
    exchange_macs(&mut phone, &mut laptop, &pairing);

    // The offer travels through the server this time; the phone approves only the verified device.
    let offer = laptop.ok("linkOffer", json!({}));
    let approved = phone.ok("approveDevice", json!({ "qr": offer["qrText"], "verifiedBy": "sas" }));
    let accepted = laptop.read(&phone.identity, &approved["message"]);
    assert_eq!(accepted["ok"], true, "{accepted}");
    assert_eq!(accepted["deviceList"]["devices"].as_array().unwrap().len(), 2);
}

#[test]
fn approval_by_code_needs_a_completed_comparison_with_that_very_device() {
    let mut phone = Device::account("alice", "phone");
    let mut laptop = Device::new("alice", "laptop");
    let offer = laptop.ok("linkOffer", json!({}));
    assert_eq!(phone.op("approveDevice", json!({ "qr": offer["qrText"], "verifiedBy": "sas" }))["error"], "not_verified");
    assert_eq!(phone.op("approveDevice", json!({ "qr": offer["qrText"] }))["error"], "bad_request");

    // The server swaps in its own device after the person compared codes with the real laptop.
    let pairing = compare_codes(&phone, &laptop, "pair-2");
    exchange_macs(&mut phone, &mut laptop, &pairing);
    let mut server_device = Device::new("alice", "laptop-2");
    let swapped = server_device.ok("linkOffer", json!({}));
    assert_eq!(phone.op("approveDevice", json!({ "qr": swapped["qrText"], "verifiedBy": "sas" }))["error"], "not_verified");
}

#[test]
fn a_server_in_the_middle_gets_different_codes_on_each_screen() {
    let phone = Device::account("alice", "phone");
    let laptop = Device::new("alice", "laptop");
    let n = call(json!({ "op": "sasStart", "pairingId": "pair-3", "role": "new" }));
    let t = call(json!({ "op": "sasStart", "pairingId": "pair-3", "role": "trusted" }));
    // The relay runs its own pairing with each side.
    let mitm_to_new = call(json!({ "op": "sasStart", "pairingId": "pair-3", "role": "trusted" }));
    let mitm_to_trusted = call(json!({ "op": "sasStart", "pairingId": "pair-3", "role": "new" }));
    let mitm_reveal = call(json!({ "op": "sasReveal", "sasId": mitm_to_trusted["sasId"] }));

    let mut request = json!({ "op": "sasEstablish", "newDevice": laptop.identity, "trustedDevice": phone.identity });
    request["sasId"] = n["sasId"].clone();
    request["theirPublicKey"] = mitm_to_new["publicKey"].clone();
    let new_side = call(request.clone());
    request["sasId"] = t["sasId"].clone();
    request["theirPublicKey"] = mitm_reveal["publicKey"].clone();
    request["theirCommitment"] = mitm_to_trusted["commitment"].clone();
    let trusted_side = call(request);
    assert_eq!(new_side["ok"], true);
    assert_eq!(trusted_side["ok"], true);
    assert_ne!(new_side["decimals"], trusted_side["decimals"], "the person sees the mismatch and stops");
}

#[test]
fn a_key_that_does_not_match_its_commitment_stops_the_pairing() {
    let phone = Device::account("alice", "phone");
    let laptop = Device::new("alice", "laptop");
    let n = call(json!({ "op": "sasStart", "pairingId": "pair-4", "role": "new" }));
    let t = call(json!({ "op": "sasStart", "pairingId": "pair-4", "role": "trusted" }));
    let other = call(json!({ "op": "sasStart", "pairingId": "pair-4", "role": "new" }));
    let other_key = call(json!({ "op": "sasReveal", "sasId": other["sasId"] }));
    let established = call(json!({
        "op": "sasEstablish", "sasId": t["sasId"], "theirPublicKey": other_key["publicKey"], "theirCommitment": n["commitment"],
        "newDevice": laptop.identity, "trustedDevice": phone.identity,
    }));
    assert_eq!(established["error"], "bad_commitment");
    // The pairing is gone: no second guess.
    let again = call(json!({
        "op": "sasEstablish", "sasId": t["sasId"], "theirPublicKey": other_key["publicKey"], "theirCommitment": other["commitment"],
        "newDevice": laptop.identity, "trustedDevice": phone.identity,
    }));
    assert_eq!(again["error"], "unknown_pairing");
}

#[test]
fn a_wrong_mac_is_refused_and_a_link_from_another_trusted_device_is_refused() {
    let mut phone = Device::account("alice", "phone");
    let mut tablet = Device::new("alice", "tablet");
    link(&mut phone, &mut tablet);
    let mut laptop = Device::new("alice", "laptop");

    // A tampered MAC doesn't verify, and the pairing is dropped.
    let pairing = compare_codes(&phone, &laptop, "pair-5");
    let laptop_mac = laptop.ok("sasMac", json!({ "sasId": pairing.new_sas }))["mac"].clone();
    let mut tampered = laptop_mac.as_str().unwrap().to_owned();
    tampered.replace_range(0..2, if tampered.starts_with("AA") { "BB" } else { "AA" });
    assert_eq!(phone.op("sasConfirm", json!({ "sasId": pairing.trusted_sas, "mac": tampered }))["error"], "bad_pairing");
    assert_eq!(phone.op("sasConfirm", json!({ "sasId": pairing.trusted_sas, "mac": laptop_mac }))["error"], "unknown_pairing");

    // A MAC from a device that isn't the one in the pairing is refused before it's computed.
    let pairing = compare_codes(&phone, &laptop, "pair-6");
    let mut stranger = Device::new("alice", "stranger");
    assert_eq!(stranger.op("sasMac", json!({ "sasId": pairing.new_sas }))["error"], "bad_pairing");

    // A clean pairing with the phone; then the tablet (also trusted) tries to link the laptop instead.
    exchange_macs(&mut phone, &mut laptop, &pairing);
    let offer = laptop.ok("linkOffer", json!({}));
    let from_tablet = tablet.ok("approveDevice", json!({ "qr": offer["qrText"], "verifiedBy": "qr" }));
    let refused = laptop.read(&tablet.identity, &from_tablet["message"]);
    assert_eq!(refused["error"], "bad_link");
    // Cancelling lets the person start over with any method.
    laptop.ok("cancelLink", json!({}));
    let offer = laptop.ok("linkOffer", json!({}));
    let from_tablet = tablet.ok("approveDevice", json!({ "qr": offer["qrText"], "verifiedBy": "qr" }));
    assert_eq!(laptop.read(&tablet.identity, &from_tablet["message"])["ok"], true);
}

#[test]
fn an_unknown_pairing_is_refused() {
    let n = call(json!({ "op": "sasReveal", "sasId": "00112233445566778899aabbccddeeff" }));
    assert_eq!(n["error"], "unknown_pairing");
}

#[test]
fn one_message_reaches_many_devices_and_a_bad_recipient_does_not_spoil_the_rest() {
    let mut alice_phone = Device::account("alice", "phone");
    let mut alice_laptop = Device::new("alice", "laptop");
    link(&mut alice_phone, &mut alice_laptop);
    let mut bob_phone = Device::account("bob", "phone");
    let mut bob_laptop = Device::new("bob", "laptop");
    link(&mut bob_phone, &mut bob_laptop);

    let otk_laptop = first_otk(&alice_laptop.prekeys(1));
    let otk_bob_phone = first_otk(&bob_phone.prekeys(1));
    let otk_bob_laptop = first_otk(&bob_laptop.prekeys(1));
    let mut forged = otk_bob_laptop.clone();
    forged["signature"] = otk_bob_phone["signature"].clone();

    let sent = alice_phone.ok(
        "encryptMany",
        json!({ "plaintext": "hello everyone", "recipients": [
            { "identity": bob_phone.identity, "oneTimeKey": otk_bob_phone },
            { "identity": bob_laptop.identity, "oneTimeKey": forged },
            { "identity": alice_laptop.identity, "oneTimeKey": otk_laptop },
            { "nothing": true },
        ]}),
    );
    let results = sent["results"].as_array().unwrap();
    assert_eq!(results.len(), 4);
    assert!(results[0].get("error").is_none(), "{}", results[0]);
    assert_eq!(results[1]["error"], "bad_prekey_signature");
    assert!(results[2].get("error").is_none(), "{}", results[2]);
    assert_eq!(results[3]["error"], "bad_request");
    assert_eq!(bob_phone.read(&alice_phone.identity, &results[0]["message"])["plaintext"], "hello everyone");
    assert_eq!(alice_laptop.read(&alice_phone.identity, &results[2]["message"])["plaintext"], "hello everyone");

    // The sessions made in that call were kept: the next message needs no one-time key.
    let again = alice_phone.ok("encryptMany", json!({ "plaintext": "again", "recipients": [{ "identity": bob_phone.identity }] }));
    assert_eq!(bob_phone.read(&alice_phone.identity, &again["results"][0]["message"])["plaintext"], "again");
}

#[test]
fn a_failed_send_keeps_the_existing_session() {
    let mut alice = Device::account("alice", "phone");
    let mut bob = Device::account("bob", "phone");
    let otk = first_otk(&bob.prekeys(1));
    let first = alice.send(&bob.identity, Some(&otk), "one");
    assert_eq!(bob.read(&alice.identity, &first)["plaintext"], "one");
    let mut broken = bob.identity.clone();
    broken["ed25519"] = json!("not a key");
    assert_eq!(alice.op("encrypt", json!({ "recipient": broken, "plaintext": "x" }))["ok"], false);
    let second = alice.send(&bob.identity, None, "two");
    assert_eq!(bob.read(&alice.identity, &second)["plaintext"], "two");
}
