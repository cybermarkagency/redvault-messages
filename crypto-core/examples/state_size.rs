//! Prints the encrypted state size of a Signal-enabled device for several one-time key counts.
use redvault_e2ee::engine::handle;
use serde_json::{json, Value};
fn call(v: Value) -> Value { serde_json::from_str(&handle(&v.to_string())).unwrap() }
fn main() {
    for count in [0u64, 20, 100] {
        let key = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
        let r = call(json!({ "op": "createAccount", "userId": "u", "deviceId": "d", "pickleKey": key }));
        let r = call(json!({ "op": "signalEnable", "state": r["state"], "pickleKey": key }));
        let r = call(json!({ "op": "signalPrekeys", "count": count, "state": r["state"], "pickleKey": key }));
        let t = std::time::Instant::now();
        let s = call(json!({ "op": "signalStatus", "state": r["state"], "pickleKey": key }));
        println!("one-time keys {count:>3}: state {:>8} bytes, one call {:?}", s["state"].as_str().unwrap().len(), t.elapsed());
    }
}
