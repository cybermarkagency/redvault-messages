//! M6: Signal groups. Sender keys sealed over the pairwise Signal sessions, for groups whose
//! membership is a verified, signed roster chain (src/roster.rs, src/signal_group.rs).
//!
//! Run: cargo test --features signal --test signal_group
#![cfg(feature = "signal")]

mod common;

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use common::Device;
use serde_json::{json, Value};

struct Member {
    d: Device,
    upload: Value,
}

fn member(user: &str, device: &str) -> Member {
    let mut d = Device::account(user, device);
    let cert = d.ok("signalEnable", json!({}))["certificate"].clone();
    let upload = d.ok("signalPrekeys", json!({ "count": 5 }))["upload"].clone();
    d.ok("setSignalCapability", json!({ "certificate": cert }));
    Member { d, upload }
}

/// Everyone verifies everyone's current list (what the apps do before a group send).
fn know_all(ms: &mut [&mut Member]) {
    let lists: Vec<Value> = ms.iter_mut().map(|m| m.d.list()).collect();
    for m in ms.iter_mut() {
        for (i, l) in lists.iter().enumerate() {
            if l["userId"] != m.d.identity["userId"] {
                let r = m.d.op("verifyDeviceList", json!({ "deviceList": l }));
                assert_eq!(r["ok"], true, "{i}: {r}");
            }
        }
    }
}

fn bundle(upload: &Value, i: usize) -> Value {
    json!({
        "certificate": upload["certificate"],
        "bundle": {
            "registrationId": upload["registrationId"], "identityKey": upload["identityKey"], "signedPreKey": upload["signedPreKey"],
            "kyberPreKey": upload["kyberPreKeys"][i], "lastResort": false, "preKey": upload["preKeys"][i],
        }
    })
}

/// `from` shares its sender key with `to` (claiming a bundle the first time) and `to` takes it in.
fn share(from: &mut Member, to: &mut [&mut Member], group: &str, claim: usize) -> Value {
    let recipients: Vec<Value> = to.iter().map(|m| json!({ "identity": m.d.identity, "signalBundle": bundle(&m.upload, claim) })).collect();
    let shared = from.d.ok("groupSignalShare", json!({ "groupId": group, "recipients": recipients }));
    for (i, m) in to.iter_mut().enumerate() {
        let r = &shared["results"][i];
        if let Some(message) = r.get("message") {
            let got = m.d.op("decrypt", json!({ "sender": from.d.identity, "message": message }));
            assert_eq!(got["kind"], "senderKey", "{got}");
        }
    }
    shared
}

fn send(from: &mut Member, group: &str, text: &str) -> Value {
    from.d.ok("groupSignalEncrypt", json!({ "groupId": group, "plaintext": text }))["message"].clone()
}

fn read(to: &mut Member, from: &Member, message: &Value) -> Value {
    to.d.op("groupSignalDecrypt", json!({ "sender": from.d.identity, "message": message }))
}

fn verify(m: &mut Member, group: &str, chain: &[&Value]) -> Value {
    m.d.op("rosterVerify", json!({ "groupId": group, "chain": chain }))
}

struct Group {
    a: Member,
    b: Member,
    c: Member,
    r1: Value,
}

/// alice, bob and carol in group "g1": roster epoch 1 by alice, verified by all, alice's key shared.
fn group() -> Group {
    let (mut a, mut b, mut c) = (member("alice", "phone"), member("bob", "laptop"), member("carol", "pc"));
    know_all(&mut [&mut a, &mut b, &mut c]);
    let r1 = a.d.ok("rosterCreate", json!({ "groupId": "g1", "members": ["alice", "bob", "carol"] }))["roster"].clone();
    for m in [&mut b, &mut c] {
        assert_eq!(verify(m, "g1", &[&r1])["epoch"], 1);
    }
    share(&mut a, &mut [&mut b, &mut c], "g1", 0);
    Group { a, b, c, r1 }
}

#[test]
fn members_read_each_others_group_messages_once() {
    let mut g = group();
    let m = send(&mut g.a, "g1", "hello group");
    assert_eq!(m["protocol"], "rv-signal-group-v1");
    assert_eq!(read(&mut g.b, &g.a, &m)["plaintext"], "hello group");
    assert_eq!(read(&mut g.c, &g.a, &m)["plaintext"], "hello group");
    assert_eq!(read(&mut g.b, &g.a, &m)["error"], "duplicate");
    // Sharing again sends nothing new to devices that already have the key.
    let again = g.a.d.ok("groupSignalShare", json!({ "groupId": "g1", "recipients": [{ "identity": g.b.d.identity }] }));
    assert_eq!(again["results"][0]["shared"], true, "{again}");
    // bob answers under his own sender key.
    share(&mut g.b, &mut [&mut g.a, &mut g.c], "g1", 1);
    let back = send(&mut g.b, "g1", "hi from bob");
    assert_eq!(read(&mut g.a, &g.b, &back)["plaintext"], "hi from bob");
    assert_eq!(read(&mut g.c, &g.b, &back)["plaintext"], "hi from bob");
}

#[test]
fn a_removed_member_reads_nothing_new_and_can_no_longer_send() {
    let mut g = group();
    // carol has a sender key out at epoch 1, before she is removed.
    share(&mut g.c, &mut [&mut g.a, &mut g.b], "g1", 1);
    let old_from_carol = send(&mut g.c, "g1", "sent at epoch 1");
    let r2 = g.a.d.ok("rosterUpdate", json!({ "groupId": "g1", "members": ["alice", "bob"] }));
    assert_eq!(r2["removed"], json!(["carol"]));
    let r2 = r2["roster"].clone();
    for m in [&mut g.b, &mut g.c] {
        assert_eq!(verify(m, "g1", &[&g.r1, &r2])["epoch"], 2);
    }
    // The epoch-1 sender key can't be used at epoch 2: share first.
    let encrypt_before_share = g.a.d.op("groupSignalEncrypt", json!({ "groupId": "g1", "plaintext": "x" }));
    assert_eq!(encrypt_before_share["error"], "share_first");
    // A share to carol is refused, even if an app asked for it.
    let to_carol = g.a.d.ok("groupSignalShare", json!({ "groupId": "g1", "recipients": [{ "identity": g.c.d.identity }] }));
    assert_eq!(to_carol["results"][0]["error"], "not_a_member", "{to_carol}");
    // The next message is under a new sender key that carol never gets.
    share(&mut g.a, &mut [&mut g.b], "g1", 1);
    let after = send(&mut g.a, "g1", "after carol left");
    assert_eq!(after["epoch"], 2);
    assert_eq!(read(&mut g.b, &g.a, &after)["plaintext"], "after carol left");
    assert_eq!(read(&mut g.c, &g.a, &after)["error"], "no_sender_key");
    // carol's message from before, and anything she sends now, is refused: she isn't in the latest roster.
    assert_eq!(read(&mut g.b, &g.c, &old_from_carol)["error"], "not_a_member");
    assert_eq!(g.c.d.op("rosterUpdate", json!({ "groupId": "g1", "members": ["alice", "bob", "carol"] }))["error"], "not_a_member");
    assert_eq!(g.c.d.op("groupSignalShare", json!({ "groupId": "g1", "recipients": [] }))["error"], "not_a_member");
}

#[test]
fn the_server_can_not_add_a_member_or_show_two_histories() {
    let mut g = group();
    // An edited roster (a member added) no longer verifies.
    let mut forged = g.r1.clone();
    forged["members"] = json!(["alice", "bob", "carol", "mallory"]);
    let mut fresh = member("bob", "tablet");
    let alice_list = g.a.d.list();
    fresh.d.ok("verifyDeviceList", json!({ "deviceList": alice_list }));
    assert_eq!(verify(&mut fresh, "g1", &[&forged])["error"], "bad_roster");

    // alice's epoch 2 removes carol; carol (still at epoch 1) signs her own epoch 2. A device that has one
    // refuses the other: two rosters for the same epoch.
    let a2 = g.a.d.ok("rosterUpdate", json!({ "groupId": "g1", "members": ["alice", "bob"] }))["roster"].clone();
    let c2 = g.c.d.ok("rosterUpdate", json!({ "groupId": "g1", "members": ["alice", "bob", "carol"] }))["roster"].clone();
    assert_eq!(verify(&mut g.b, "g1", &[&g.r1, &a2])["epoch"], 2);
    assert_eq!(verify(&mut g.b, "g1", &[&g.r1, &c2])["error"], "roster_equivocation");
    // And a roster that skips an epoch, or names another previous one, is refused.
    let a3 = g.a.d.ok("rosterUpdate", json!({ "groupId": "g1", "members": ["alice", "bob"] }))["roster"].clone();
    let a4 = g.a.d.ok("rosterUpdate", json!({ "groupId": "g1", "members": ["alice", "bob"] }))["roster"].clone();
    assert_eq!(verify(&mut g.b, "g1", &[&a4])["error"], "roster_gap");
    let mut wrong_prev = a3.clone();
    wrong_prev["prev"] = json!("A".repeat(43));
    assert_eq!(verify(&mut g.b, "g1", &[&wrong_prev])["ok"], false);
    assert_eq!(verify(&mut g.b, "g1", &[&a3, &a4])["epoch"], 4);
}

#[test]
fn a_new_device_needs_the_whole_history() {
    let mut g = group();
    let r2 = g.a.d.ok("rosterUpdate", json!({ "groupId": "g1", "members": ["alice", "bob"] }))["roster"].clone();
    let mut newcomer = member("dave", "phone");
    // Without alice's list, her rosters can't be checked.
    assert_eq!(verify(&mut newcomer, "g1", &[&g.r1, &r2])["error"], "unknown_issuer");
    let alice_list = g.a.d.list();
    newcomer.d.ok("verifyDeviceList", json!({ "deviceList": alice_list }));
    // Shown only the latest roster, a new device refuses: it must see the chain from epoch 1.
    assert_eq!(verify(&mut newcomer, "g1", &[&r2])["error"], "roster_history_missing");
    let ok = verify(&mut newcomer, "g1", &[&g.r1, &r2]);
    assert_eq!(ok["epoch"], 2);
    assert_eq!(ok["isMember"], false);
}

#[test]
fn a_group_message_is_bound_to_its_sender_group_and_epoch() {
    let mut g = group();
    let m = send(&mut g.a, "g1", "from alice");
    // Presented as carol's: the key belongs to alice's device.
    assert_eq!(read(&mut g.b, &g.c, &m)["error"], "identity_mismatch");
    // Moved to another group, or another epoch.
    let mut moved = m.clone();
    moved["groupId"] = json!("g2");
    assert_eq!(read(&mut g.b, &g.a, &moved)["error"], "roster_unknown");
    let mut later = m.clone();
    later["epoch"] = json!(2);
    assert_eq!(read(&mut g.b, &g.a, &later)["error"], "roster_unknown");
    // Tampered: refused, and nothing changes.
    let mut bytes = B64.decode(m["body"].as_str().unwrap()).unwrap();
    let i = bytes.len() - 10;
    bytes[i] ^= 1;
    let mut tampered = m.clone();
    tampered["body"] = json!(B64.encode(bytes));
    let before = g.b.d.state.clone();
    assert_eq!(read(&mut g.b, &g.a, &tampered)["ok"], false);
    assert_eq!(g.b.d.state, before);
    assert_eq!(read(&mut g.b, &g.a, &m)["plaintext"], "from alice");
}

#[test]
fn a_sender_key_from_someone_outside_the_roster_is_refused() {
    let (mut a, mut b) = (member("alice", "phone"), member("bob", "laptop"));
    let mut mallory = member("mallory", "pc");
    know_all(&mut [&mut a, &mut b, &mut mallory]);
    let r1 = a.d.ok("rosterCreate", json!({ "groupId": "g1", "members": ["alice", "bob"] }))["roster"].clone();
    verify(&mut b, "g1", &[&r1]);
    verify(&mut mallory, "g1", &[&r1]);
    // mallory isn't a member: her engine won't share, and bob wouldn't take her key.
    assert_eq!(mallory.d.op("groupSignalShare", json!({ "groupId": "g1", "recipients": [] }))["error"], "not_a_member");
}
