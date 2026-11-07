//! Cross-platform check for rv-signal-v1: each step runs on a different
//! machine's build of the engine, and the steps hand JSON to each other through
//! a file. Only the engine's public JSON interface is used.
//!
//!   signal_xplat setup  <file>   accounts alice and bob; alice starts a session; 3 messages to bob
//!   signal_xplat bob    <file>   bob opens them (in reverse order) and replies twice
//!   signal_xplat alice  <file>   alice opens the replies and sends one more; checks a replay is refused
//!   signal_xplat final  <file>   bob opens the last message
//!
//! The file holds the test devices' encrypted states and their storage keys.
//! These are throwaway test keys, generated fresh on every run.
//!
//! Run: cargo run --features signal --example signal_xplat -- <step> <file>
use std::fs;

use redvault_e2ee::engine::handle;
use serde_json::{json, Value};

fn call(request: Value) -> Value {
    serde_json::from_str(&handle(&request.to_string())).unwrap()
}

fn key() -> String {
    use base64::Engine as _;
    let mut k = [0u8; 32];
    getrandom::fill(&mut k).unwrap();
    base64::engine::general_purpose::STANDARD.encode(k)
}

/// Runs `op` on the named device in `world`, stores its new state, and fails loudly on an error.
fn op(world: &mut Value, who: &str, op: &str, mut fields: Value) -> Value {
    fields["op"] = json!(op);
    fields["state"] = world[who]["state"].clone();
    fields["pickleKey"] = world[who]["key"].clone();
    let r = call(fields);
    assert_eq!(r["ok"], true, "{who} {op}: {r}");
    world[who]["state"] = r["state"].clone();
    r
}

/// Like `op`, but returns the refusal instead of failing (the state is not changed).
fn try_op(world: &mut Value, who: &str, op_name: &str, mut fields: Value) -> Value {
    fields["op"] = json!(op_name);
    fields["state"] = world[who]["state"].clone();
    fields["pickleKey"] = world[who]["key"].clone();
    call(fields)
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let (step, file) = (args[1].as_str(), args[2].as_str());
    let platform = format!("{}-{}", std::env::consts::OS, std::env::consts::ARCH);
    let mut world: Value = if step == "setup" { json!({ "log": [] }) } else { serde_json::from_str(&fs::read_to_string(file).unwrap()).unwrap() };
    match step {
        "setup" => {
            for (name, user, device) in [("alice", "alice", "win-phone"), ("bob", "bob", "linux-laptop")] {
                let k = key();
                let r = call(json!({ "op": "createAccount", "userId": user, "deviceId": device, "pickleKey": k }));
                world[name] = json!({ "state": r["state"], "key": k, "identity": r["identity"] });
            }
            let bob_list = op(&mut world, "bob", "deviceList", json!({}))["deviceList"].clone();
            let alice_list = op(&mut world, "alice", "deviceList", json!({}))["deviceList"].clone();
            op(&mut world, "alice", "verifyDeviceList", json!({ "deviceList": bob_list }));
            op(&mut world, "bob", "verifyDeviceList", json!({ "deviceList": alice_list }));
            world["aliceCert"] = op(&mut world, "alice", "signalEnable", json!({}))["certificate"].clone();
            world["bobCert"] = op(&mut world, "bob", "signalEnable", json!({}))["certificate"].clone();
            let up = op(&mut world, "bob", "signalPrekeys", json!({ "count": 2 }))["upload"].clone();
            let bundle = json!({
                "registrationId": up["registrationId"], "identityKey": up["identityKey"], "signedPreKey": up["signedPreKey"],
                "kyberPreKey": up["kyberPreKeys"][0], "preKey": up["preKeys"][0],
            });
            let (bob_id, bob_cert) = (world["bob"]["identity"].clone(), world["bobCert"].clone());
            op(&mut world, "alice", "signalStartSession", json!({ "peer": bob_id, "certificate": bob_cert, "bundle": bundle }));
            let mut to_bob = Vec::new();
            for i in 0..3 {
                let bob_id = world["bob"]["identity"].clone();
                to_bob.push(op(&mut world, "alice", "signalEncrypt", json!({ "peer": bob_id, "plaintext": format!("from {platform} #{i}") }))["message"].clone());
            }
            world["toBob"] = json!(to_bob);
        }
        "bob" => {
            let alice_id = world["alice"]["identity"].clone();
            let cert = world["aliceCert"].clone();
            let messages = world["toBob"].as_array().unwrap().clone();
            let mut opened = Vec::new();
            for m in messages.iter().rev() {
                let r = op(&mut world, "bob", "signalDecrypt", json!({ "sender": alice_id, "certificate": cert, "message": m }));
                opened.push(r["plaintext"].clone());
            }
            println!("bob on {platform} opened: {opened:?}");
            let mut to_alice = Vec::new();
            for i in 0..2 {
                to_alice.push(op(&mut world, "bob", "signalEncrypt", json!({ "peer": alice_id, "plaintext": format!("reply from {platform} #{i}") }))["message"].clone());
            }
            world["toAlice"] = json!(to_alice);
        }
        "alice" => {
            let bob_id = world["bob"]["identity"].clone();
            let messages = world["toAlice"].as_array().unwrap().clone();
            for m in &messages {
                let r = op(&mut world, "alice", "signalDecrypt", json!({ "sender": bob_id, "message": m }));
                println!("alice on {platform} opened: {}", r["plaintext"]);
            }
            let replay = try_op(&mut world, "alice", "signalDecrypt", json!({ "sender": bob_id, "message": messages[0] }));
            assert_eq!(replay["error"], "duplicate", "{replay}");
            println!("alice on {platform}: replay refused ({})", replay["error"]);
            let mut tampered = messages[1].clone();
            let body = tampered["body"].as_str().unwrap().to_owned();
            let flipped = if body.starts_with('A') { format!("B{}", &body[1..]) } else { format!("A{}", &body[1..]) };
            tampered["body"] = json!(flipped);
            let refused = try_op(&mut world, "alice", "signalDecrypt", json!({ "sender": bob_id, "message": tampered }));
            assert_eq!(refused["ok"], false, "{refused}");
            println!("alice on {platform}: tampered message refused ({})", refused["error"]);
            world["last"] = op(&mut world, "alice", "signalEncrypt", json!({ "peer": bob_id, "plaintext": format!("last from {platform}") }))["message"].clone();
        }
        "final" => {
            let alice_id = world["alice"]["identity"].clone();
            let last = world["last"].clone();
            let r = op(&mut world, "bob", "signalDecrypt", json!({ "sender": alice_id, "message": last }));
            println!("bob on {platform} opened: {}", r["plaintext"]);
        }
        _ => panic!("unknown step"),
    }
    world["log"].as_array_mut().unwrap().push(json!(format!("{step} on {platform}")));
    fs::write(file, serde_json::to_string_pretty(&world).unwrap()).unwrap();
    println!("{step} ok on {platform}");
}
