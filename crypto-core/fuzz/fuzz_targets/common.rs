// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Shared set-up for the fuzz targets: real devices made once per process through the engine's
//! public JSON interface (no private access), with a Signal session and a group between them.
#![allow(dead_code)]

use std::sync::OnceLock;

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use redvault_e2ee::engine::handle;
use serde_json::{json, Value};

pub fn call(v: &Value) -> Value {
    serde_json::from_str(&handle(&v.to_string())).expect("the engine always answers JSON")
}

#[derive(Clone)]
pub struct Dev {
    pub state: String,
    pub key: String,
    pub identity: Value,
}

impl Dev {
    fn account(user: &str, device: &str) -> Dev {
        let mut k = [0u8; 32];
        getrandom::fill(&mut k).unwrap();
        let key = B64.encode(k);
        let r = call(&json!({ "op": "createAccount", "userId": user, "deviceId": device, "pickleKey": key }));
        Dev { state: r["state"].as_str().unwrap().to_owned(), key, identity: r["identity"].clone() }
    }
    pub fn op(&mut self, op: &str, mut fields: Value) -> Value {
        fields["op"] = json!(op);
        fields["state"] = json!(self.state);
        fields["pickleKey"] = json!(self.key);
        let r = call(&fields);
        if r["ok"] == true {
            if let Some(s) = r["state"].as_str() {
                self.state = s.to_owned();
            }
        }
        r
    }
    fn ok(&mut self, op: &str, fields: Value) -> Value {
        let r = self.op(op, fields);
        assert_eq!(r["ok"], true, "{op}: {r}");
        r
    }
}

pub struct World {
    /// The receiver, in a state that has a Signal session with `alice` and alice's group key.
    pub bob: Dev,
    pub alice: Dev,
    /// A real first message, a later message and a group message from alice to bob.
    pub first: Value,
    pub later: Value,
    pub group: Value,
    pub alice_cert: Value,
    pub roster: Value,
}

pub fn world() -> &'static World {
    static W: OnceLock<World> = OnceLock::new();
    W.get_or_init(|| {
        let mut alice = Dev::account("alice", "phone");
        let mut bob = Dev::account("bob", "laptop");
        let alice_cert = alice.ok("signalEnable", json!({}))["certificate"].clone();
        let bob_cert = bob.ok("signalEnable", json!({}))["certificate"].clone();
        let up = bob.ok("signalPrekeys", json!({ "count": 2 }))["upload"].clone();
        alice.ok("setSignalCapability", json!({ "certificate": alice_cert }));
        bob.ok("setSignalCapability", json!({ "certificate": bob_cert }));
        let al = alice.ok("deviceList", json!({}))["deviceList"].clone();
        let bl = bob.ok("deviceList", json!({}))["deviceList"].clone();
        alice.ok("verifyDeviceList", json!({ "deviceList": bl }));
        bob.ok("verifyDeviceList", json!({ "deviceList": al }));
        let bundle = json!({
            "registrationId": up["registrationId"], "identityKey": up["identityKey"], "signedPreKey": up["signedPreKey"],
            "kyberPreKey": up["kyberPreKeys"][0], "preKey": up["preKeys"][0],
        });
        alice.ok("signalStartSession", json!({ "peer": bob.identity, "certificate": up["certificate"], "bundle": bundle }));
        let first = alice.ok("signalEncrypt", json!({ "peer": bob.identity, "plaintext": "first" }))["message"].clone();
        let mut opened = bob.clone();
        opened.ok("signalDecrypt", json!({ "sender": alice.identity, "certificate": alice_cert, "message": first }));
        let reply = opened.ok("signalEncrypt", json!({ "peer": alice.identity, "plaintext": "reply" }))["message"].clone();
        alice.ok("signalDecrypt", json!({ "sender": opened.identity, "message": reply }));
        let later = alice.ok("signalEncrypt", json!({ "peer": bob.identity, "plaintext": "later" }))["message"].clone();
        // A group of two: alice's roster, verified by bob, and alice's sender key at bob.
        let roster = alice.ok("rosterCreate", json!({ "groupId": "g", "members": ["alice", "bob"] }))["roster"].clone();
        opened.ok("rosterVerify", json!({ "groupId": "g", "chain": [roster] }));
        let shared = alice.ok("groupSignalShare", json!({ "groupId": "g", "recipients": [{ "identity": opened.identity }] }));
        opened.ok("decrypt", json!({ "sender": alice.identity, "message": shared["results"][0]["message"] }));
        let group = alice.ok("groupSignalEncrypt", json!({ "groupId": "g", "plaintext": "group" }))["message"].clone();
        World { bob: opened, alice, first, later, group, alice_cert, roster }
    })
}
