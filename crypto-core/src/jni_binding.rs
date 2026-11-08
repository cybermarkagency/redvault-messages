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
