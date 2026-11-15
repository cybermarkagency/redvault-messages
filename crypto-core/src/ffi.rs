// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! C ABI for the desktop C++ app (Windows and Linux): see include/redvault_e2ee.h.

use std::panic::{catch_unwind, AssertUnwindSafe};

use crate::engine;

/// Runs one JSON request (UTF-8, `len` bytes). Returns a newly allocated UTF-8
/// JSON response (not NUL-terminated) and its length in `out_len`; free it with
/// rv_e2ee_free. Returns null only when `out_len` is null (nothing is allocated then);
/// a panic becomes an `internal` error response.
///
/// # Safety
/// `request` must point to `len` readable bytes and `out_len` to a writable size_t.
#[no_mangle]
pub unsafe extern "C" fn rv_e2ee_call(request: *const u8, len: usize, out_len: *mut usize) -> *mut u8 {
    if out_len.is_null() {
        return std::ptr::null_mut();
    }
    let response = catch_unwind(AssertUnwindSafe(|| {
        if request.is_null() {
            return r#"{"ok":false,"error":"bad_request","message":"no request"}"#.to_owned();
        }
        let bytes = std::slice::from_raw_parts(request, len);
        match std::str::from_utf8(bytes) {
            Ok(text) => engine::handle(text),
            Err(_) => r#"{"ok":false,"error":"bad_request","message":"the request isn't UTF-8"}"#.to_owned(),
        }
    }))
    .unwrap_or_else(|_| r#"{"ok":false,"error":"internal","message":"the engine failed"}"#.to_owned());
    let boxed = response.into_bytes().into_boxed_slice();
    *out_len = boxed.len();
    // into_raw hands over ownership without the move that invalidated an earlier as_mut_ptr (Miri, Stacked Borrows).
    Box::into_raw(boxed).cast::<u8>()
}

/// Frees a response from rv_e2ee_call, wiping it first (it holds encrypted state and may hold plaintext).
///
/// # Safety
/// `ptr` and `len` must come from one rv_e2ee_call, freed once.
#[no_mangle]
pub unsafe extern "C" fn rv_e2ee_free(ptr: *mut u8, len: usize) {
    if ptr.is_null() {
        return;
    }
    let mut boxed = Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len));
    zeroize::Zeroize::zeroize(&mut boxed[..]);
    drop(boxed);
}

// --- Sealed requests for the backup route (docs/tor-sealed-fallback.md) ---

#[cfg(feature = "sealed-transport")]
mod sealed_ffi {
    use std::ffi::c_void;
    use std::panic::{catch_unwind, AssertUnwindSafe};

    use crate::sealed::{seal, PendingReply, RequestHead};

    /// Status codes of the rv_sealed_* calls.
    const OK: i32 = 0;
    const BAD_INPUT: i32 = -1;
    const FAILED: i32 = -2;

    unsafe fn bytes<'a>(ptr: *const u8, len: usize) -> &'a [u8] {
        if ptr.is_null() || len == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(ptr, len)
        }
    }

    unsafe fn hand_over(data: Vec<u8>, out: *mut *mut u8, out_len: *mut usize) {
        let boxed = data.into_boxed_slice();
        *out_len = boxed.len();
        *out = Box::into_raw(boxed).cast::<u8>();
    }

    /// Seals one request. `head_json` is `{"method":..,"path":"/api/..","headers":[[name,value],..]}`.
    /// On success (0): `*sealed`/`*sealed_len` hold the bytes to POST (free with rv_e2ee_free) and
    /// `*pending` the handle that opens the reply (rv_sealed_open or rv_sealed_discard, exactly once).
    ///
    /// # Safety
    /// Every pointer must be valid for its length; the out pointers must be writable.
    #[no_mangle]
    pub unsafe extern "C" fn rv_sealed_seal(
        key_config: *const u8,
        key_config_len: usize,
        head_json: *const u8,
        head_len: usize,
        body: *const u8,
        body_len: usize,
        sealed: *mut *mut u8,
        sealed_len: *mut usize,
        pending: *mut *mut c_void,
    ) -> i32 {
        if sealed.is_null() || sealed_len.is_null() || pending.is_null() {
            return BAD_INPUT;
        }
        let result = catch_unwind(AssertUnwindSafe(|| {
            let head: RequestHead = match serde_json::from_slice(bytes(head_json, head_len)) {
                Ok(head) => head,
                Err(_) => return Err(BAD_INPUT),
            };
            seal(bytes(key_config, key_config_len), &head, bytes(body, body_len)).map_err(|_| FAILED)
        }));
        match result {
            Ok(Ok((data, reply))) => {
                hand_over(data, sealed, sealed_len);
                *pending = Box::into_raw(Box::new(reply)).cast::<c_void>();
                OK
            }
            Ok(Err(code)) => code,
            Err(_) => FAILED,
        }
    }

    /// Opens a sealed reply with the handle from rv_sealed_seal, which this always consumes.
    /// On success (0): `*head_json` holds `{"status":..,"headers":[[name,value],..]}` and `*body`
    /// the body; free both with rv_e2ee_free.
    ///
    /// # Safety
    /// `pending` must come from rv_sealed_seal and not have been used; the rest as rv_sealed_seal.
    #[no_mangle]
    pub unsafe extern "C" fn rv_sealed_open(
        pending: *mut c_void,
        reply: *const u8,
        reply_len: usize,
        head_json: *mut *mut u8,
        head_len: *mut usize,
        body: *mut *mut u8,
        body_len: *mut usize,
    ) -> i32 {
        if pending.is_null() {
            return BAD_INPUT;
        }
        let handle = Box::from_raw(pending.cast::<PendingReply>());
        if head_json.is_null() || head_len.is_null() || body.is_null() || body_len.is_null() {
            return BAD_INPUT;
        }
        let result = catch_unwind(AssertUnwindSafe(|| handle.open(bytes(reply, reply_len))));
        match result {
            Ok(Ok((head, data))) => {
                let json = serde_json::to_vec(&head).unwrap_or_default();
                hand_over(json, head_json, head_len);
                hand_over(data, body, body_len);
                OK
            }
            _ => FAILED,
        }
    }

    /// Drops a handle from rv_sealed_seal whose reply never came (a failed or cancelled request).
    ///
    /// # Safety
    /// `pending` must come from rv_sealed_seal and not have been used.
    #[no_mangle]
    pub unsafe extern "C" fn rv_sealed_discard(pending: *mut c_void) {
        if !pending.is_null() {
            drop(Box::from_raw(pending.cast::<PendingReply>()));
        }
    }
}
