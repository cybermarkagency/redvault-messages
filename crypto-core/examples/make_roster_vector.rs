// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Writes tests/vectors/group_roster_chain.json: a real two-roster chain (epoch 1, then
//! epoch 2 removing a member), signed by an account's key, together with that account's
//! signed device list. The server's tests check that they verify the same bytes. The keys
//! are throwaway test keys.
//!
//! Run: cargo run --example make_roster_vector
use redvault_e2ee::engine::handle;
use serde_json::{json, Value};

fn call(v: Value) -> Value {
    let r: Value = serde_json::from_str(&handle(&v.to_string())).unwrap();
    assert_eq!(r["ok"], true, "{r}");
    r
}

fn main() {
    let key = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    let alice = "6f1c2a4e-1111-4c3b-9d2e-0a1b2c3d4e5f";
    let bob = "7a2d3b5f-2222-4d4c-8e3f-1b2c3d4e5f60";
    let group = "8b3e4c6a-3333-4e5d-9f4a-2c3d4e5f6071";
    let r = call(json!({ "op": "createAccount", "userId": alice, "deviceId": "0b9e8d7c-2222-4a5b-8c6d-7e8f9a0b1c2d", "pickleKey": key }));
    let list = r["deviceList"].clone();
    let r = call(json!({ "op": "rosterCreate", "groupId": group, "members": [alice, bob], "state": r["state"], "pickleKey": key }));
    let r1 = r["roster"].clone();
    let r = call(json!({ "op": "rosterUpdate", "groupId": group, "members": [alice], "state": r["state"], "pickleKey": key }));
    let r2 = r["roster"].clone();
    let vector = json!({
        "description": "A group roster chain (epoch 1 with two members, epoch 2 removing one), signed by the first account's key, and that account's signed device list. Throwaway test keys.",
        "deviceList": list,
        "chain": [r1, r2],
    });
    let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/vectors");
    std::fs::create_dir_all(dir).unwrap();
    let path = format!("{dir}/group_roster_chain.json");
    std::fs::write(&path, serde_json::to_string_pretty(&vector).unwrap() + "\n").unwrap();
    println!("wrote {path}");
}
