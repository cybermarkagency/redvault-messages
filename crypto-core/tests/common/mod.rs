// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Test helper: a device as an app would hold it (its encrypted state and storage key).
#![allow(dead_code)]

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use redvault_e2ee::engine::handle;
use serde_json::{json, Value};

pub struct Device {
    pub state: String,
    pub key: String,
    pub identity: Value,
}

pub fn call(request: Value) -> Value {
    serde_json::from_str(&handle(&request.to_string())).unwrap()
}

fn storage_key() -> String {
    let mut key = [0u8; 32];
    getrandom::fill(&mut key).unwrap();
    B64.encode(key)
}

impl Device {
    /// A device not (yet) linked to an account.
    pub fn new(user: &str, device: &str) -> Device {
        let key = storage_key();
        let created = call(json!({ "op": "createDevice", "userId": user, "deviceId": device, "pickleKey": key }));
        assert_eq!(created["ok"], true, "{created}");
        Device { state: created["state"].as_str().unwrap().to_owned(), key, identity: created["identity"].clone() }
    }

    /// The first device of a new account (holds the account key, list version 1).
    pub fn account(user: &str, device: &str) -> Device {
        let key = storage_key();
        let created = call(json!({ "op": "createAccount", "userId": user, "deviceId": device, "pickleKey": key }));
        assert_eq!(created["ok"], true, "{created}");
        assert_eq!(created["deviceList"]["version"], 1);
        Device { state: created["state"].as_str().unwrap().to_owned(), key, identity: created["identity"].clone() }
    }

    /// A copy with the same stored state (another process, or a forked backup).
    pub fn clone_state(&self) -> Device {
        Device { state: self.state.clone(), key: self.key.clone(), identity: self.identity.clone() }
    }

    pub fn op(&mut self, op: &str, mut fields: Value) -> Value {
        fields["op"] = json!(op);
        fields["state"] = json!(self.state);
        fields["pickleKey"] = json!(self.key);
        let response = call(fields);
        if response["ok"] == true {
            self.state = response["state"].as_str().unwrap().to_owned();
        }
        response
    }

    pub fn ok(&mut self, op: &str, fields: Value) -> Value {
        let response = self.op(op, fields);
        assert_eq!(response["ok"], true, "{op}: {response}");
        response
    }

    pub fn prekeys(&mut self, count: u64) -> Value {
        self.ok("prekeys", json!({ "count": count }))["upload"].clone()
    }

    pub fn list(&mut self) -> Value {
        self.ok("deviceList", json!({}))["deviceList"].clone()
    }

    pub fn send(&mut self, to: &Value, otk: Option<&Value>, text: &str) -> Value {
        let mut request = json!({ "recipient": to, "plaintext": text });
        if let Some(otk) = otk {
            request["oneTimeKey"] = otk.clone();
        }
        self.ok("encrypt", request)["message"].clone()
    }

    pub fn read(&mut self, from: &Value, message: &Value) -> Value {
        self.op("decrypt", json!({ "sender": from, "message": message }))
    }
}

pub fn first_otk(upload: &Value) -> Value {
    upload["oneTimeKeys"][0].clone()
}

/// Links `new` into `trusted`'s account by QR (offer, approval, delivery); returns the new list.
pub fn link(trusted: &mut Device, new: &mut Device) -> Value {
    let offer = new.ok("linkOffer", json!({}));
    let approved = trusted.ok("approveDevice", json!({ "qr": offer["qrText"], "verifiedBy": "qr" }));
    let accepted = new.read(&trusted.identity, &approved["message"]);
    assert_eq!(accepted["ok"], true, "{accepted}");
    assert_eq!(accepted["kind"], "link");
    approved["deviceList"].clone()
}
