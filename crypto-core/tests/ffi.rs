// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

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

/// The sealed backup route through the C ABI (docs/tor-sealed-fallback.md): seal, open at a gateway,
/// open the reply, and the handle rules.
#[cfg(feature = "sealed-gateway")]
mod sealed {
    use redvault_e2ee::ffi::rv_e2ee_free;
    use redvault_e2ee::sealed::{Gateway, ResponseHead};

    extern "C" {
        fn rv_sealed_seal(
            key_config: *const u8, key_config_len: usize, head_json: *const u8, head_len: usize,
            body: *const u8, body_len: usize, sealed: *mut *mut u8, sealed_len: *mut usize,
            pending: *mut *mut std::ffi::c_void,
        ) -> i32;
        fn rv_sealed_open(
            pending: *mut std::ffi::c_void, reply: *const u8, reply_len: usize, head_json: *mut *mut u8,
            head_len: *mut usize, body: *mut *mut u8, body_len: *mut usize,
        ) -> i32;
        fn rv_sealed_discard(pending: *mut std::ffi::c_void);
    }

    const IKM: &[u8] = b"redvault sealed test key, never used on the live gateway";

    #[test]
    fn seal_and_open_through_the_c_abi() {
        let (config, public) = Gateway::key_from_secret(1, IKM).unwrap();
        let gw = Gateway::new(vec![config]).unwrap();
        let head = br#"{"method":"POST","path":"/api/auth/prelogin","headers":[["content-type","application/json"]]}"#;
        let body = br#"{"username":"alice"}"#;
        let (mut sealed, mut sealed_len, mut pending) = (std::ptr::null_mut(), 0usize, std::ptr::null_mut());
        let rc = unsafe {
            rv_sealed_seal(public.as_ptr(), public.len(), head.as_ptr(), head.len(), body.as_ptr(), body.len(),
                &mut sealed, &mut sealed_len, &mut pending)
        };
        assert_eq!(rc, 0);
        let request = unsafe { std::slice::from_raw_parts(sealed, sealed_len) }.to_vec();
        unsafe { rv_e2ee_free(sealed, sealed_len) };

        let (got_head, got_body, reply) = gw.open_request(&request).unwrap();
        assert_eq!(got_head.path, "/api/auth/prelogin");
        assert_eq!(got_body, body);
        let sealed_reply = reply.seal(&ResponseHead { status: 201, headers: vec![] }, b"done").unwrap();

        let (mut h, mut hl, mut b, mut bl) = (std::ptr::null_mut(), 0usize, std::ptr::null_mut(), 0usize);
        let rc = unsafe { rv_sealed_open(pending, sealed_reply.as_ptr(), sealed_reply.len(), &mut h, &mut hl, &mut b, &mut bl) };
        assert_eq!(rc, 0);
        let head_out = String::from_utf8(unsafe { std::slice::from_raw_parts(h, hl) }.to_vec()).unwrap();
        let body_out = unsafe { std::slice::from_raw_parts(b, bl) }.to_vec();
        unsafe {
            rv_e2ee_free(h, hl);
            rv_e2ee_free(b, bl);
        }
        assert!(head_out.contains(r#""status":201"#));
        assert_eq!(body_out, b"done");
    }

    #[test]
    fn bad_input_is_refused_and_handles_can_be_discarded() {
        let (_, public) = Gateway::key_from_secret(1, IKM).unwrap();
        let (mut sealed, mut sealed_len, mut pending) = (std::ptr::null_mut(), 0usize, std::ptr::null_mut());
        let junk = b"{not json";
        let rc = unsafe {
            rv_sealed_seal(public.as_ptr(), public.len(), junk.as_ptr(), junk.len(), std::ptr::null(), 0,
                &mut sealed, &mut sealed_len, &mut pending)
        };
        assert_eq!(rc, -1);
        let admin = br#"{"method":"GET","path":"/admin"}"#;
        let rc = unsafe {
            rv_sealed_seal(public.as_ptr(), public.len(), admin.as_ptr(), admin.len(), std::ptr::null(), 0,
                &mut sealed, &mut sealed_len, &mut pending)
        };
        assert_eq!(rc, -2);
        let ok = br#"{"method":"GET","path":"/api/me"}"#;
        let rc = unsafe {
            rv_sealed_seal(public.as_ptr(), public.len(), ok.as_ptr(), ok.len(), std::ptr::null(), 0,
                &mut sealed, &mut sealed_len, &mut pending)
        };
        assert_eq!(rc, 0);
        unsafe {
            rv_e2ee_free(sealed, sealed_len);
            rv_sealed_discard(pending);
            rv_sealed_discard(std::ptr::null_mut());
        }
        // A reply that isn't sealed: refused, and the handle is still consumed.
        let (mut s2, mut l2, mut p2) = (std::ptr::null_mut(), 0usize, std::ptr::null_mut());
        assert_eq!(unsafe { rv_sealed_seal(public.as_ptr(), public.len(), ok.as_ptr(), ok.len(), std::ptr::null(), 0, &mut s2, &mut l2, &mut p2) }, 0);
        unsafe { rv_e2ee_free(s2, l2) };
        let (mut h, mut hl, mut b, mut bl) = (std::ptr::null_mut(), 0usize, std::ptr::null_mut(), 0usize);
        let junk_reply = [0u8; 64];
        assert_eq!(unsafe { rv_sealed_open(p2, junk_reply.as_ptr(), junk_reply.len(), &mut h, &mut hl, &mut b, &mut bl) }, -2);
    }
}
