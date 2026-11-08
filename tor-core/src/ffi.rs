// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! C ABI for the desktop C++ app (Windows and Linux): see include/redvault_tor.h.

use std::ffi::{c_char, c_int, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use crate::json_config::Handle;

/// Opaque to C.
pub struct RvTorGateway(Handle);

fn to_c(text: String) -> *mut c_char {
    CString::new(text.replace('\0', " ")).map(CString::into_raw).unwrap_or(ptr::null_mut())
}

/// Starts Tor and the SOCKS gate. Returns NULL on failure and, when `error` is
/// not NULL, stores a message there (free it with rv_tor_free_string).
///
/// # Safety
/// `config_json` must be a NUL-terminated UTF-8 string; `error` NULL or writable.
#[no_mangle]
pub unsafe extern "C" fn rv_tor_start(config_json: *const c_char, error: *mut *mut c_char) -> *mut RvTorGateway {
    let result = catch_unwind(AssertUnwindSafe(|| {
        if config_json.is_null() {
            return Err("no config".to_owned());
        }
        let text = CStr::from_ptr(config_json).to_str().map_err(|_| "the config isn't UTF-8".to_owned())?;
        Handle::start(text)
    }))
    .unwrap_or_else(|_| Err("the Tor gateway failed to start".to_owned()));
    match result {
        Ok(handle) => Box::into_raw(Box::new(RvTorGateway(handle))),
        Err(message) => {
            if !error.is_null() {
                *error = to_c(message);
            }
            ptr::null_mut()
        }
    }
}

/// `{"host":"127.0.0.1","port":N,"username":"…","password":"…"}`. Free with rv_tor_free_string.
///
/// # Safety
/// `gateway` must come from rv_tor_start and not be stopped.
#[no_mangle]
pub unsafe extern "C" fn rv_tor_endpoint(gateway: *const RvTorGateway) -> *mut c_char {
    match gateway.as_ref() {
        Some(gateway) => to_c(gateway.0.endpoint_json()),
        None => ptr::null_mut(),
    }
}

/// `{"state":"connecting|connected|unavailable","progress":0-100,"detail":"…","onionReachable":…,"stats":{…}}`.
///
/// # Safety
/// `gateway` must come from rv_tor_start and not be stopped.
#[no_mangle]
pub unsafe extern "C" fn rv_tor_status(gateway: *const RvTorGateway) -> *mut c_char {
    match gateway.as_ref() {
        Some(gateway) => to_c(gateway.0.status_json()),
        None => ptr::null_mut(),
    }
}

/// # Safety
/// `gateway` must come from rv_tor_start and not be stopped.
#[no_mangle]
pub unsafe extern "C" fn rv_tor_set_dormant(gateway: *const RvTorGateway, dormant: c_int) {
    if let Some(gateway) = gateway.as_ref() {
        let _ = catch_unwind(AssertUnwindSafe(|| gateway.0.set_dormant(dormant != 0)));
    }
}

/// Closes the SOCKS port, stops Tor and frees the gateway.
///
/// # Safety
/// `gateway` must come from rv_tor_start; it is invalid afterwards.
#[no_mangle]
pub unsafe extern "C" fn rv_tor_stop(gateway: *mut RvTorGateway) {
    if gateway.is_null() {
        return;
    }
    let boxed = Box::from_raw(gateway);
    let _ = catch_unwind(AssertUnwindSafe(move || boxed.0.stop()));
}

/// # Safety
/// `text` must come from this library, freed once.
#[no_mangle]
pub unsafe extern "C" fn rv_tor_free_string(text: *mut c_char) {
    if !text.is_null() {
        drop(CString::from_raw(text));
    }
}
