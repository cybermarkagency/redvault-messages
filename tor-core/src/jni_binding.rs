// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! JNI binding for Android: the native methods of com.redvaultsystems.tor.NativeTor.

use std::panic::{catch_unwind, AssertUnwindSafe};

use jni::objects::{JClass, JString};
use jni::sys::{jboolean, jlong, jstring, JNI_FALSE};
use jni::JNIEnv;

use crate::json_config::Handle;

fn handle<'a>(ptr: jlong) -> Option<&'a Handle> {
    // SAFETY: the Java side only passes values returned by nativeStart and not yet stopped.
    unsafe { (ptr as *const Handle).as_ref() }
}

fn new_string(env: &mut JNIEnv, text: String) -> jstring {
    env.new_string(text).map(|s| s.into_raw()).unwrap_or(std::ptr::null_mut())
}

/// `static native long nativeStart(String configJson)`; throws IllegalStateException on failure.
#[no_mangle]
pub extern "system" fn Java_com_redvaultsystems_tor_NativeTor_nativeStart<'local>(
    mut env: JNIEnv<'local>,
    _class: JClass<'local>,
    config: JString<'local>,
) -> jlong {
    let text: String = match env.get_string(&config) {
        Ok(value) => value.into(),
        Err(_) => {
            let _ = env.throw_new("java/lang/IllegalStateException", "unreadable config");
            return 0;
        }
    };
    let result = catch_unwind(AssertUnwindSafe(|| Handle::start(&text)))
        .unwrap_or_else(|_| Err("the Tor gateway failed to start".to_owned()));
    match result {
        Ok(handle) => Box::into_raw(Box::new(handle)) as jlong,
        Err(message) => {
            let _ = env.throw_new("java/lang/IllegalStateException", message);
            0
        }
    }
}

/// `static native String nativeEndpoint(long handle)`.
#[no_mangle]
pub extern "system" fn Java_com_redvaultsystems_tor_NativeTor_nativeEndpoint<'local>(
    mut env: JNIEnv<'local>,
    _class: JClass<'local>,
    ptr: jlong,
) -> jstring {
    let text = handle(ptr).map(Handle::endpoint_json).unwrap_or_else(|| "null".to_owned());
    new_string(&mut env, text)
}

/// `static native String nativeStatus(long handle)`.
#[no_mangle]
pub extern "system" fn Java_com_redvaultsystems_tor_NativeTor_nativeStatus<'local>(
    mut env: JNIEnv<'local>,
    _class: JClass<'local>,
    ptr: jlong,
) -> jstring {
    let text = handle(ptr)
        .map(Handle::status_json)
        .unwrap_or_else(|| r#"{"state":"unavailable","progress":0,"detail":"Tor is stopped"}"#.to_owned());
    new_string(&mut env, text)
}

/// `static native void nativeSetDormant(long handle, boolean dormant)`.
#[no_mangle]
pub extern "system" fn Java_com_redvaultsystems_tor_NativeTor_nativeSetDormant<'local>(
    _env: JNIEnv<'local>,
    _class: JClass<'local>,
    ptr: jlong,
    dormant: jboolean,
) {
    if let Some(handle) = handle(ptr) {
        let _ = catch_unwind(AssertUnwindSafe(|| handle.set_dormant(dormant != JNI_FALSE)));
    }
}

/// `static native void nativeStop(long handle)`; the handle is invalid afterwards.
#[no_mangle]
pub extern "system" fn Java_com_redvaultsystems_tor_NativeTor_nativeStop<'local>(
    _env: JNIEnv<'local>,
    _class: JClass<'local>,
    ptr: jlong,
) {
    if ptr != 0 {
        // SAFETY: ptr came from nativeStart and is stopped once.
        let boxed = unsafe { Box::from_raw(ptr as *mut Handle) };
        let _ = catch_unwind(AssertUnwindSafe(move || boxed.stop()));
    }
}
