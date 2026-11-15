// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! JNI binding for Android (and the JVM, for host tests): the native method of
//! com.redvaultsystems.e2ee.NativeE2ee.

use std::panic::{catch_unwind, AssertUnwindSafe};

use jni::objects::{JClass, JString};
use jni::sys::jstring;
use jni::JNIEnv;

use crate::engine;

/// `static native String call(String request)`.
#[no_mangle]
pub extern "system" fn Java_com_redvaultsystems_e2ee_NativeE2ee_call<'local>(
    mut env: JNIEnv<'local>,
    _class: JClass<'local>,
    request: JString<'local>,
) -> jstring {
    let text: String = match env.get_string(&request) {
        Ok(value) => value.into(),
        Err(_) => r#"{"ok":false,"error":"bad_request","message":"unreadable request"}"#.to_owned(),
    };
    let response = catch_unwind(AssertUnwindSafe(|| engine::handle(&text)))
        .unwrap_or_else(|_| r#"{"ok":false,"error":"internal","message":"the engine failed"}"#.to_owned());
    match env.new_string(response) {
        Ok(value) => value.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

// --- Sealed requests for the backup route (docs/tor-sealed-fallback.md):
// the native methods of com.redvaultsystems.e2ee.NativeSealed. Failures throw IOException.

#[cfg(feature = "sealed-transport")]
mod sealed_jni {
    use std::panic::{catch_unwind, AssertUnwindSafe};

    use jni::objects::{JByteArray, JClass, JString};
    use jni::sys::{jbyteArray, jlong};
    use jni::JNIEnv;

    use crate::sealed::{seal, PendingReply, RequestHead};

    fn fail(env: &mut JNIEnv, message: &str) -> jbyteArray {
        let _ = env.throw_new("java/io/IOException", message);
        std::ptr::null_mut()
    }

    /// `static native byte[] seal(byte[] keyConfig, String headJson, byte[] body)`:
    /// returns the 8-byte handle (big-endian long) followed by the bytes to POST.
    #[no_mangle]
    pub extern "system" fn Java_com_redvaultsystems_e2ee_NativeSealed_seal<'local>(
        mut env: JNIEnv<'local>,
        _class: JClass<'local>,
        key_config: JByteArray<'local>,
        head_json: JString<'local>,
        body: JByteArray<'local>,
    ) -> jbyteArray {
        let (Ok(key), Ok(head), Ok(body)) = (
            env.convert_byte_array(&key_config),
            env.get_string(&head_json).map(String::from),
            env.convert_byte_array(&body),
        ) else {
            return fail(&mut env, "unreadable arguments");
        };
        let Ok(head) = serde_json::from_str::<RequestHead>(&head) else {
            return fail(&mut env, "bad request head");
        };
        match catch_unwind(AssertUnwindSafe(|| seal(&key, &head, &body))) {
            Ok(Ok((sealed, reply))) => {
                let handle = Box::into_raw(Box::new(reply)) as jlong;
                let mut out = Vec::with_capacity(8 + sealed.len());
                out.extend_from_slice(&handle.to_be_bytes());
                out.extend_from_slice(&sealed);
                match env.byte_array_from_slice(&out) {
                    Ok(array) => array.into_raw(),
                    Err(_) => {
                        // SAFETY: the handle was made just above and never handed out.
                        unsafe { drop(Box::from_raw(handle as *mut PendingReply)) };
                        fail(&mut env, "out of memory")
                    }
                }
            }
            Ok(Err(e)) => fail(&mut env, &e.message),
            Err(_) => fail(&mut env, "sealing failed"),
        }
    }

    /// `static native byte[] open(long handle, byte[] reply)`: always consumes the handle. Returns a
    /// 4-byte big-endian length, the head JSON (`{"status":..,"headers":[..]}`), then the body.
    #[no_mangle]
    pub extern "system" fn Java_com_redvaultsystems_e2ee_NativeSealed_open<'local>(
        mut env: JNIEnv<'local>,
        _class: JClass<'local>,
        handle: jlong,
        reply: JByteArray<'local>,
    ) -> jbyteArray {
        if handle == 0 {
            return fail(&mut env, "no handle");
        }
        // SAFETY: handles only come from seal above and are used once (the Kotlin wrapper enforces it).
        let pending = unsafe { Box::from_raw(handle as *mut PendingReply) };
        let Ok(reply) = env.convert_byte_array(&reply) else {
            return fail(&mut env, "unreadable reply");
        };
        match catch_unwind(AssertUnwindSafe(|| pending.open(&reply))) {
            Ok(Ok((head, body))) => {
                let json = serde_json::to_vec(&head).unwrap_or_default();
                let mut out = Vec::with_capacity(4 + json.len() + body.len());
                out.extend_from_slice(&(json.len() as u32).to_be_bytes());
                out.extend_from_slice(&json);
                out.extend_from_slice(&body);
                match env.byte_array_from_slice(&out) {
                    Ok(array) => array.into_raw(),
                    Err(_) => fail(&mut env, "out of memory"),
                }
            }
            Ok(Err(e)) => fail(&mut env, &e.message),
            Err(_) => fail(&mut env, "opening failed"),
        }
    }

    /// `static native void discard(long handle)`: drops a handle whose reply never came.
    #[no_mangle]
    pub extern "system" fn Java_com_redvaultsystems_e2ee_NativeSealed_discard<'local>(
        _env: JNIEnv<'local>,
        _class: JClass<'local>,
        handle: jlong,
    ) {
        if handle != 0 {
            // SAFETY: as in open.
            unsafe { drop(Box::from_raw(handle as *mut PendingReply)) };
        }
    }
}
