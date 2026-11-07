//! The C ABI the desktop app calls. Also clean under Miri:
//! `MIRIFLAGS=-Zmiri-strict-provenance cargo +nightly miri test --features ffi --test ffi`.
#![cfg(feature = "ffi")]

use redvault_e2ee::ffi::{rv_e2ee_call, rv_e2ee_free};

fn roundtrip(request: &[u8]) -> String {
    let mut len = 0usize;
    let ptr = unsafe { rv_e2ee_call(request.as_ptr(), request.len(), &mut len) };
    assert!(!ptr.is_null());
    let out = unsafe { std::slice::from_raw_parts(ptr, len) }.to_vec();
    unsafe { rv_e2ee_free(ptr, len) };
    String::from_utf8(out).unwrap()
}

#[test]
fn answers_every_request_with_json_it_allocated() {
    assert!(roundtrip(br#"{"op":"version"}"#).contains(r#""ok":true"#));
    assert!(roundtrip(b"\xff\xfe not UTF-8").contains("bad_request"));
    assert!(roundtrip(b"{not json").contains("bad_request"));
    assert!(roundtrip(br#"{"op":"decrypt","state":"x","pickleKey":"AAAA"}"#).contains(r#""ok":false"#));
}

#[test]
fn null_pointers_are_refused_not_followed() {
    let mut len = 0usize;
    let ptr = unsafe { rv_e2ee_call(std::ptr::null(), 0, &mut len) };
    assert!(!ptr.is_null());
    unsafe { rv_e2ee_free(ptr, len) };
    // No out_len: nothing is allocated, so nothing could be freed with the right length.
    let request = br#"{"op":"version"}"#;
    assert!(unsafe { rv_e2ee_call(request.as_ptr(), request.len(), std::ptr::null_mut()) }.is_null());
    unsafe { rv_e2ee_free(std::ptr::null_mut(), 0) };
}
