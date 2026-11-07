//! M3: the engine picks the protocol for the apps. `encryptMany` uses rv-signal-v1 for every
//! recipient device that speaks it (pinned, or declared in a verified signed list) and Olm for
//! the others. `decrypt` opens either, by the message's protocol tag, and never tries a message
//! under two protocols.
//!
//! Run: cargo test --features signal --test signal_routing
#![cfg(feature = "signal")]

mod common;

use common::Device;
use serde_json::{json, Value};

/// An account whose only device has Signal on, its keys made, and the capability in its signed list.
/// Returns the device, its Signal upload (what the server would store) and the new list.
fn signal_device(user: &str, device: &str) -> (Device, Value, Value) {
    let mut d = Device::account(user, device);
    let cert = d.ok("signalEnable", json!({}))["certificate"].clone();
    let upload = d.ok("signalPrekeys", json!({ "count": 3 }))["upload"].clone();
    let list = d.ok("setSignalCapability", json!({ "certificate": cert }))["deviceList"].clone();
    (d, upload, list)
}

/// What the server's /signal/claim returns for a device (a one-time key of each kind).
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

fn results(r: &Value) -> Vec<Value> {
    r["results"].as_array().unwrap().clone()
}

#[test]
fn encrypt_many_uses_signal_where_both_devices_speak_it_and_olm_elsewhere() {
    let (mut alice, _, alice_list) = signal_device("alice", "phone");
    let (mut bob, bob_up, bob_list) = signal_device("bob", "laptop");
    let mut wendy = Device::account("wendy", "browser"); // the web app: no Signal
    let wendy_olm = wendy.prekeys(1);
    alice.ok("verifyDeviceList", json!({ "deviceList": bob_list }));
    alice.ok("verifyDeviceList", json!({ "deviceList": wendy.list() }));
    bob.ok("verifyDeviceList", json!({ "deviceList": alice_list }));
    wendy.ok("verifyDeviceList", json!({ "deviceList": alice_list }));

    // First try, as the apps do it: no keys. bob needs a Signal bundle, wendy an Olm one-time key.
    let first = alice.ok("encryptMany", json!({ "plaintext": "hi both", "recipients": [{ "identity": bob.identity }, { "identity": wendy.identity }] }));
    let r = results(&first);
    assert_eq!(r[0]["error"], "no_signal_session", "{first}");
    assert_eq!(r[1]["error"], "no_session", "{first}");

    // Second try with what the server handed out.
    let second = alice.ok(
        "encryptMany",
        json!({ "plaintext": "hi both", "recipients": [
            { "identity": bob.identity, "signalBundle": claimed(&bob_up, 0) },
            { "identity": wendy.identity, "oneTimeKey": wendy_olm["oneTimeKeys"][0] },
        ] }),
    );
    let r = results(&second);
    assert_eq!(r[0]["protocol"], "rv-signal-v1");
    assert_eq!(r[0]["message"]["type"], "prekey");
    assert_eq!(r[0]["message"]["certificate"]["deviceId"], "phone", "a first message carries the sender's certificate");
    assert!(r[1].get("protocol").is_none() && r[1]["message"]["type"].is_number(), "wendy gets Olm: {second}");

    // Both open with the same `decrypt` call the apps already make.
    let at_bob = bob.op("decrypt", json!({ "sender": alice.identity, "message": r[0]["message"] }));
    assert_eq!(at_bob["plaintext"], "hi both", "{at_bob}");
    assert_eq!(at_bob["protocol"], "rv-signal-v1");
    let at_wendy = wendy.read(&alice.identity, &r[1]["message"]);
    assert_eq!(at_wendy["plaintext"], "hi both", "{at_wendy}");

    // bob replies through encryptMany too: he has a session now, so no bundle and no certificate.
    let reply = bob.ok("encryptMany", json!({ "plaintext": "hi alice", "recipients": [{ "identity": alice.identity }] }));
    let m = &results(&reply)[0]["message"];
    assert_eq!(m["type"], "signal");
    assert!(m.get("certificate").is_none());
    assert_eq!(alice.op("decrypt", json!({ "sender": bob.identity, "message": m }))["plaintext"], "hi alice");
}

#[test]
fn a_signal_device_without_keys_on_the_server_gets_nothing_rather_than_olm() {
    let (mut alice, _, _) = signal_device("alice", "phone");
    let (bob, _, bob_list) = signal_device("bob", "laptop");
    alice.ok("verifyDeviceList", json!({ "deviceList": bob_list }));
    let withheld = json!({ "certificate": null, "bundle": null });
    let r = alice.ok("encryptMany", json!({ "plaintext": "x", "recipients": [{ "identity": bob.identity, "signalBundle": withheld }] }));
    assert_eq!(results(&r)[0]["error"], "signal_keys_missing", "{r}");
}

#[test]
fn decrypt_never_tries_a_message_under_another_protocol() {
    let (mut alice, _, alice_list) = signal_device("alice", "phone");
    let (mut bob, bob_up, bob_list) = signal_device("bob", "laptop");
    alice.ok("verifyDeviceList", json!({ "deviceList": bob_list }));
    bob.ok("verifyDeviceList", json!({ "deviceList": alice_list }));
    let sent = alice.ok("encryptMany", json!({ "plaintext": "x", "recipients": [{ "identity": bob.identity, "signalBundle": claimed(&bob_up, 0) }] }));
    let m = results(&sent)[0]["message"].clone();

    // The tag stripped: it goes to the Olm decrypter, which can't read Signal bytes.
    let mut untagged = m.clone();
    untagged.as_object_mut().unwrap().remove("protocol");
    assert_eq!(bob.op("decrypt", json!({ "sender": alice.identity, "message": untagged }))["ok"], false);
    // An unknown tag: refused outright.
    let mut unknown = m.clone();
    unknown["protocol"] = json!("rv-signal-v2");
    assert_eq!(bob.op("decrypt", json!({ "sender": alice.identity, "message": unknown }))["error"], "unknown_protocol");
    // A first message without the sender's certificate can't be checked: refused, nothing changes.
    let mut no_cert = m.clone();
    no_cert.as_object_mut().unwrap().remove("certificate");
    let before = bob.state.clone();
    assert_eq!(bob.op("decrypt", json!({ "sender": alice.identity, "message": no_cert }))["error"], "bad_request");
    assert_eq!(bob.state, before);
    // A certificate for another device: refused.
    let mut wrong_cert = m.clone();
    wrong_cert["certificate"] = bob_up["certificate"].clone();
    assert_eq!(bob.op("decrypt", json!({ "sender": alice.identity, "message": wrong_cert }))["error"], "bad_certificate");
    // The real one opens.
    assert_eq!(bob.op("decrypt", json!({ "sender": alice.identity, "message": m }))["plaintext"], "x");
}

#[test]
fn copies_for_the_senders_own_other_devices_use_signal_too() {
    // alice's laptop is linked; both her devices speak Signal and the list says so.
    let mut phone = Device::account("alice", "phone");
    let mut laptop = Device::new("alice", "laptop");
    common::link(&mut phone, &mut laptop);
    let phone_cert = phone.ok("signalEnable", json!({}))["certificate"].clone();
    let laptop_cert = laptop.ok("signalEnable", json!({}))["certificate"].clone();
    let laptop_up = laptop.ok("signalPrekeys", json!({ "count": 1 }))["upload"].clone();
    phone.ok("setSignalCapability", json!({ "certificate": phone_cert }));
    let list = phone.ok("setSignalCapability", json!({ "certificate": laptop_cert }))["deviceList"].clone();
    laptop.ok("verifyDeviceList", json!({ "deviceList": list }));

    let r = phone.ok("encryptMany", json!({ "plaintext": "note to self", "recipients": [{ "identity": laptop.identity, "signalBundle": claimed(&laptop_up, 0) }] }));
    let m = results(&r)[0]["message"].clone();
    assert_eq!(m["protocol"], "rv-signal-v1");
    assert_eq!(laptop.op("decrypt", json!({ "sender": phone.identity, "message": m }))["plaintext"], "note to self");
}

#[test]
fn a_build_without_signal_refuses_signal_messages_by_tag() {
    // This is what the web build answers. Simulated here with an unknown protocol, since this
    // test binary has the feature. Also check that Olm still works for a device without Signal.
    let mut a = Device::account("a", "1");
    let mut b = Device::account("b", "1");
    let keys = b.prekeys(1);
    a.ok("verifyDeviceList", json!({ "deviceList": b.list() }));
    b.ok("verifyDeviceList", json!({ "deviceList": a.list() }));
    let m = a.send(&b.identity, Some(&common::first_otk(&keys)), "olm still works");
    assert_eq!(b.read(&a.identity, &m)["plaintext"], "olm still works");
    let mut tagged = m.clone();
    tagged["protocol"] = json!("something-else");
    assert_eq!(b.read(&a.identity, &tagged)["error"], "unknown_protocol");
}
