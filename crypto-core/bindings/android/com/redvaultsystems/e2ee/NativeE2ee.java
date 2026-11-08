// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.e2ee;

/**
 * Android's (and the JVM's) door to the shared Rust engine (crypto-core/src/jni_binding.rs).
 * One JSON request in, one JSON response out; see docs/vodozemac-poc.md. In the app
 * the library would be packaged as jniLibs and loaded with System.loadLibrary;
 * the test driver loads it from an explicit path instead.
 */
public final class NativeE2ee {
    private NativeE2ee() {}

    /** Loads libredvault_e2ee from the path in system property rv.lib, or by name. */
    public static void load() {
        String path = System.getProperty("rv.lib");
        if (path != null && !path.isEmpty()) {
            System.load(path);
        } else {
            System.loadLibrary("redvault_e2ee");
        }
    }

    /** Runs one JSON request against the engine and returns its JSON response. */
    public static native String call(String request);
}
