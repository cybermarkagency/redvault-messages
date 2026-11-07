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
