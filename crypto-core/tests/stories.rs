// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Encrypted Stories: fresh keys, authenticated publishers, audiences, expiry, archive.

mod common;

use common::{first_otk, Device};
use serde_json::{json, Value};

const HOUR: u64 = 3_600_000;
const NOW: u64 = 1_800_000_000_000;

struct World {
    alice: Device,
    bob: Device,
    carol: Device,
    bob_keys: Value,
    carol_keys: Value,
}

fn world() -> World {
    let mut alice = Device::account("alice", "phone");
    let mut bob = Device::account("bob", "phone");
    let mut carol = Device::account("carol", "phone");
    for (viewer, owner) in [(0, 1), (0, 2), (1, 0), (2, 0)] {
        let list = [&mut alice, &mut bob, &mut carol][owner].list();
        [&mut alice, &mut bob, &mut carol][viewer].ok("verifyDeviceList", json!({ "deviceList": list }));
    }
    let bob_keys = bob.prekeys(3);
    let carol_keys = carol.prekeys(3);
    World { alice, bob, carol, bob_keys, carol_keys }
}

fn publish(w: &mut World, story_id: &str, content: &str, audience: Value) -> Value {
    w.alice.ok("storyCreate", json!({ "storyId": story_id, "content": content, "expiresAt": NOW + 24 * HOUR, "audience": audience }))
}

#[test]
fn an_audience_member_opens_a_story_and_knows_who_published_it() {
    let mut w = world();
    let audience = json!([w.bob.identity, w.carol.identity]);
    let story = publish(&mut w, "s1", "beach day", audience);
    let shared = w.alice.ok("storyShareKey", json!({ "storyId": "s1", "recipient": w.bob.identity, "oneTimeKey": first_otk(&w.bob_keys) }));
    let received = w.bob.read(&w.alice.identity, &shared["message"]);
    assert_eq!(received["kind"], "storyKey", "{received}");
    let opened = w.bob.ok("storyOpen", json!({ "storyId": "s1", "ciphertext": story["ciphertext"], "now": NOW }));
    assert_eq!(opened["content"], "beach day");
    assert_eq!(opened["publisher"]["userId"], "alice");
    // Carol hasn't been sent the key yet: nothing to open with.
    assert_eq!(w.carol.op("storyOpen", json!({ "storyId": "s1", "ciphertext": story["ciphertext"], "now": NOW }))["error"], "unknown_story");
}

#[test]
fn only_the_audience_gets_keys_and_tampered_content_is_refused() {
    let mut w = world();
    let audience = json!([w.bob.identity]);
    let story = publish(&mut w, "s2", "close friends only", audience);
    let refused = w.alice.op("storyShareKey", json!({ "storyId": "s2", "recipient": w.carol.identity, "oneTimeKey": first_otk(&w.carol_keys) }));
    assert_eq!(refused["error"], "not_in_audience");
    let shared = w.alice.ok("storyShareKey", json!({ "storyId": "s2", "recipient": w.bob.identity, "oneTimeKey": first_otk(&w.bob_keys) }));
    w.bob.read(&w.alice.identity, &shared["message"]);
    let mut blob = base64_decode(story["ciphertext"].as_str().unwrap());
    let last = blob.len() - 1;
    blob[last] ^= 1;
    let tampered = w.bob.op("storyOpen", json!({ "storyId": "s2", "ciphertext": base64_encode(&blob), "now": NOW }));
    assert_eq!(tampered["error"], "content_mismatch");
}

#[test]
fn expired_stories_lose_their_keys_but_stay_in_the_publishers_archive() {
    let mut w = world();
    let audience = json!([w.bob.identity]);
    let story = publish(&mut w, "s3", "gone tomorrow", audience);
    let shared = w.alice.ok("storyShareKey", json!({ "storyId": "s3", "recipient": w.bob.identity, "oneTimeKey": first_otk(&w.bob_keys) }));
    w.bob.read(&w.alice.identity, &shared["message"]);
    let later = NOW + 25 * HOUR;
    let expired = w.bob.ok("storyOpen", json!({ "storyId": "s3", "ciphertext": story["ciphertext"], "now": later }));
    assert_eq!(expired["expired"], true);
    assert!(expired.get("content").is_none());
    // The key is gone even if the clock is turned back.
    assert_eq!(w.bob.op("storyOpen", json!({ "storyId": "s3", "ciphertext": story["ciphertext"], "now": NOW }))["error"], "unknown_story");
    // Alice's archive still opens it.
    let archived = w.alice.ok("storyOpen", json!({ "storyId": "s3", "ciphertext": story["ciphertext"], "now": later }));
    assert_eq!(archived["content"], "gone tomorrow");
    assert_eq!(w.alice.ok("storyArchive", json!({}))["stories"][0]["storyId"], "s3");
    assert_eq!(w.alice.ok("storyDelete", json!({ "storyId": "s3" }))["deleted"], true);
    assert_eq!(w.alice.ok("storyArchive", json!({}))["stories"], json!([]));
}

#[test]
fn expiry_sweeps_received_keys() {
    let mut w = world();
    let audience = json!([w.bob.identity]);
    publish(&mut w, "s4", "a", audience);
    let shared = w.alice.ok("storyShareKey", json!({ "storyId": "s4", "recipient": w.bob.identity, "oneTimeKey": first_otk(&w.bob_keys) }));
    w.bob.read(&w.alice.identity, &shared["message"]);
    assert_eq!(w.bob.ok("storyExpire", json!({ "now": NOW }))["deleted"], json!([]));
    assert_eq!(w.bob.ok("storyExpire", json!({ "now": NOW + 48 * HOUR }))["deleted"], json!(["s4"]));
}

#[test]
fn audience_changes_say_who_already_has_the_key() {
    let mut w = world();
    let audience = json!([w.bob.identity, w.carol.identity]);
    publish(&mut w, "s5", "update", audience);
    let shared = w.alice.ok("storyShareKey", json!({ "storyId": "s5", "recipient": w.carol.identity, "oneTimeKey": first_otk(&w.carol_keys) }));
    w.carol.read(&w.alice.identity, &shared["message"]);
    let changed = w.alice.ok("storySetAudience", json!({ "storyId": "s5", "audience": [w.bob.identity] }));
    assert_eq!(changed["removed"], json!([w.carol.identity]));
    assert_eq!(changed["alreadyHadKey"], json!([w.carol.identity]));
    let refused = w.alice.op("storyShareKey", json!({ "storyId": "s5", "recipient": w.carol.identity }));
    assert_eq!(refused["error"], "not_in_audience");
}

#[test]
fn another_publisher_can_not_take_over_a_story_id() {
    let mut w = world();
    let audience = json!([w.bob.identity]);
    publish(&mut w, "s6", "mine", audience);
    let shared = w.alice.ok("storyShareKey", json!({ "storyId": "s6", "recipient": w.bob.identity, "oneTimeKey": first_otk(&w.bob_keys) }));
    w.bob.read(&w.alice.identity, &shared["message"]);
    let carols = w.carol.ok("storyCreate", json!({ "storyId": "s6", "content": "fake", "expiresAt": NOW + HOUR, "audience": [w.bob.identity] }));
    assert_eq!(carols["ok"], true);
    let shared = w.carol.ok("storyShareKey", json!({ "storyId": "s6", "recipient": w.bob.identity, "oneTimeKey": w.bob_keys["oneTimeKeys"][1] }));
    assert_eq!(w.bob.read(&w.carol.identity, &shared["message"])["error"], "story_conflict");
}

fn base64_decode(text: &str) -> Vec<u8> {
    use base64::{engine::general_purpose::STANDARD, Engine as _};
    STANDARD.decode(text).unwrap()
}

fn base64_encode(bytes: &[u8]) -> String {
    use base64::{engine::general_purpose::STANDARD, Engine as _};
    STANDARD.encode(bytes)
}
