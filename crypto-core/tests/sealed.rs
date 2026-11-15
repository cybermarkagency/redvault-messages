// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Sealed requests for the backup route (docs/tor-sealed-fallback.md).
//! Run with: cargo test --features sealed-gateway --test sealed

#![cfg(feature = "sealed-gateway")]

use redvault_e2ee::sealed::{
    decode_request, encode_request, padded_len, seal, valid_path, Gateway, RequestHead, ResponseHead,
};

const IKM: &[u8] = b"redvault sealed test key, never used on the live gateway";

fn gateway(key_id: u8) -> (Gateway, Vec<u8>) {
    let (config, public) = Gateway::key_from_secret(key_id, IKM).unwrap();
    (Gateway::new(vec![config]).unwrap(), public)
}

fn head(method: &str, path: &str) -> RequestHead {
    RequestHead {
        method: method.into(),
        path: path.into(),
        headers: vec![("content-type".into(), "application/json".into()), ("cookie".into(), "session=abc".into())],
    }
}

#[test]
fn a_request_and_its_reply_go_round() {
    let (gw, public) = gateway(1);
    let body = br#"{"username":"alice"}"#;
    let (sealed, pending) = seal(&public, &head("POST", "/api/auth/prelogin"), body).unwrap();

    let (got_head, got_body, reply) = gw.open_request(&sealed).unwrap();
    assert_eq!(got_head, head("POST", "/api/auth/prelogin"));
    assert_eq!(got_body, body);

    let answer = ResponseHead { status: 200, headers: vec![("set-cookie".into(), "session=def; HttpOnly".into())] };
    let sealed_reply = reply.seal(&answer, b"{\"ok\":true}").unwrap();
    let (opened_head, opened_body) = pending.open(&sealed_reply).unwrap();
    assert_eq!(opened_head, answer);
    assert_eq!(opened_body, b"{\"ok\":true}");
}

#[test]
fn every_message_is_padded_to_a_bucket() {
    assert_eq!(padded_len(1), 1024);
    assert_eq!(padded_len(1024), 1024);
    assert_eq!(padded_len(1025), 4096);
    assert_eq!(padded_len(70_000), 256 * 1024);
    assert_eq!(padded_len(300_000), 512 * 1024);
    let (_, public) = gateway(1);
    let small = seal(&public, &head("GET", "/api/me"), b"").unwrap().0;
    let larger = seal(&public, &head("POST", "/api/messages"), &[7u8; 900]).unwrap().0;
    // Both inner messages fit 1 KB, so both sealed requests are the same size.
    assert_eq!(small.len(), larger.len());
}

#[test]
fn a_tampered_request_is_refused() {
    let (gw, public) = gateway(1);
    let (mut sealed, _) = seal(&public, &head("GET", "/api/me"), b"").unwrap();
    let last = sealed.len() - 1;
    sealed[last] ^= 1;
    assert!(gw.open_request(&sealed).is_err());
}

#[test]
fn a_tampered_reply_is_refused() {
    let (gw, public) = gateway(1);
    let (sealed, pending) = seal(&public, &head("GET", "/api/me"), b"").unwrap();
    let (_, _, reply) = gw.open_request(&sealed).unwrap();
    let mut sealed_reply = reply.seal(&ResponseHead { status: 200, headers: vec![] }, b"hi").unwrap();
    let last = sealed_reply.len() - 1;
    sealed_reply[last] ^= 1;
    assert!(pending.open(&sealed_reply).is_err());
}

#[test]
fn a_request_to_another_key_is_refused() {
    let (_, other_public) = {
        let (config, public) = Gateway::key_from_secret(9, b"a different secret, at least 32 bytes long....").unwrap();
        (config, public)
    };
    let (gw, _) = gateway(1);
    let (sealed, _) = seal(&other_public, &head("GET", "/api/me"), b"").unwrap();
    assert!(gw.open_request(&sealed).is_err());
}

#[test]
fn the_gateway_accepts_current_and_next_keys() {
    let (current, current_public) = Gateway::key_from_secret(1, IKM).unwrap();
    let (next, next_public) = Gateway::key_from_secret(2, b"the next key's secret, also at least 32 bytes").unwrap();
    let gw = Gateway::new(vec![current, next]).unwrap();
    for public in [current_public, next_public] {
        let (sealed, _) = seal(&public, &head("GET", "/api/me"), b"").unwrap();
        assert!(gw.open_request(&sealed).is_ok());
    }
}

#[test]
fn only_api_paths_and_plain_methods_are_sealed() {
    let (_, public) = gateway(1);
    for path in ["/admin", "/api/../etc/passwd", "/api//x", "/api/a#b", "api/me", "/api/a\\b", "/api/ a"] {
        assert!(!valid_path(path), "{path}");
        assert!(seal(&public, &head("GET", path), b"").is_err(), "{path}");
    }
    assert!(seal(&public, &head("CONNECT", "/api/me"), b"").is_err());
    assert!(seal(&public, &head("TRACE", "/api/me"), b"").is_err());
}

#[test]
fn identifying_and_hop_by_hop_headers_are_refused() {
    let (_, public) = gateway(1);
    for name in ["host", "user-agent", "x-forwarded-for", "forwarded", "via", "transfer-encoding"] {
        let mut h = head("GET", "/api/me");
        h.headers.push((name.into(), "x".into()));
        assert!(seal(&public, &h, b"").is_err(), "{name}");
    }
    let mut h = head("GET", "/api/me");
    h.headers.push(("x-test".into(), "a\r\nb".into()));
    assert!(seal(&public, &h, b"").is_err());
}

#[test]
fn padding_must_be_zero() {
    let mut inner = encode_request(&head("GET", "/api/me"), b"").unwrap();
    assert!(decode_request(&inner).is_ok());
    let last = inner.len() - 1;
    inner[last] = 1;
    assert!(decode_request(&inner).is_err());
}

#[test]
fn junk_is_refused() {
    let (gw, _) = gateway(1);
    assert!(gw.open_request(b"").is_err());
    assert!(gw.open_request(&[0u8; 100]).is_err());
    assert!(gw.open_request(&vec![1u8; 10 * 1024 * 1024]).is_err());
}

#[test]
fn a_large_file_part_fits() {
    let (gw, public) = gateway(1);
    let part = vec![42u8; 2 * 1024 * 1024];
    let (sealed, pending) = seal(&public, &head("PUT", "/api/files/upload/part"), &part).unwrap();
    let (_, body, reply) = gw.open_request(&sealed).unwrap();
    assert_eq!(body, part);
    let sealed_reply = reply.seal(&ResponseHead { status: 204, headers: vec![] }, b"").unwrap();
    assert_eq!(pending.open(&sealed_reply).unwrap().0.status, 204);
}

#[test]
fn the_same_request_never_seals_the_same_way_twice() {
    let (_, public) = gateway(1);
    let a = seal(&public, &head("GET", "/api/me"), b"").unwrap().0;
    let b = seal(&public, &head("GET", "/api/me"), b"").unwrap().0;
    assert_ne!(a, b);
}

/// The public key configuration the test secret produces: the apps' and gateway's tests use the
/// same one (tests/vectors/sealed_test_key.json), so all three agree on the format.
#[test]
fn the_shared_test_key_matches() {
    let (_, public) = gateway(1);
    let text = std::fs::read_to_string("tests/vectors/sealed_test_key.json").unwrap();
    let v: serde_json::Value = serde_json::from_str(&text).unwrap();
    let hex: String = public.iter().map(|b| format!("{b:02x}")).collect();
    assert_eq!(v["keyConfigHex"].as_str().unwrap(), hex);
}

mod random_input {
    use proptest::prelude::*;
    use redvault_e2ee::sealed::{decode_request, decode_response, Gateway};

    proptest! {
        #![proptest_config(ProptestConfig::with_cases(2000))]
        /// The gateway's internet-facing parsers never panic on arbitrary bytes.
        #[test]
        fn arbitrary_bytes_never_panic(data in proptest::collection::vec(any::<u8>(), 0..4096)) {
            let _ = decode_request(&data);
            let _ = decode_response(&data);
            let (config, _) = Gateway::key_from_secret(1, super::IKM).unwrap();
            let gw = Gateway::new(vec![config]).unwrap();
            prop_assert!(gw.open_request(&data).is_err());
        }
    }
}
