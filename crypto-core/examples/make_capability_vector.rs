//! Writes tests/vectors/signal_capability_list.json: a real signed device list
//! with an rv-signal-v1 capability, and the device certificate it pins. The
//! engine's tests (in every build) and the server's tests read this file, to
//! check that all three agree on the signed bytes. The keys are throwaway test keys.
//!
//! Run: cargo run --features signal --example make_capability_vector
use redvault_e2ee::engine::handle;
use serde_json::{json, Value};

fn call(v: Value) -> Value {
    let r: Value = serde_json::from_str(&handle(&v.to_string())).unwrap();
    assert_eq!(r["ok"], true, "{r}");
    r
}

fn main() {
    let key = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    let user = "6f1c2a4e-1111-4c3b-9d2e-0a1b2c3d4e5f";
    let device = "0b9e8d7c-2222-4a5b-8c6d-7e8f9a0b1c2d";
    let r = call(json!({ "op": "createAccount", "userId": user, "deviceId": device, "pickleKey": key }));
    let r = call(json!({ "op": "signalEnable", "state": r["state"], "pickleKey": key }));
    let cert = r["certificate"].clone();
    let r = call(json!({ "op": "setSignalCapability", "certificate": cert, "state": r["state"], "pickleKey": key }));
    let vector = json!({
        "description": "Protocol-3 signed device list (version 2) with an rv-signal-v1 capability pinning the device certificate. Throwaway test keys.",
        "deviceList": r["deviceList"],
        "certificate": cert,
    });
    let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/vectors");
    std::fs::create_dir_all(dir).unwrap();
    let path = format!("{dir}/signal_capability_list.json");
    std::fs::write(&path, serde_json::to_string_pretty(&vector).unwrap() + "\n").unwrap();
    println!("wrote {path}");
}
