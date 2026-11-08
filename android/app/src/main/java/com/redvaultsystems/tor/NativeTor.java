// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.tor;

/**
 * Android's door to the embedded Tor gateway (tor-core/src/jni_binding.rs).
 * In the app the library is packaged as jniLibs and loaded with
 * System.loadLibrary; the test probe loads it from an explicit path.
 *
 * OkHttp then uses it as Proxy(Proxy.Type.SOCKS, 127.0.0.1:port) with the
 * returned credentials (java.net.Authenticator answers the SOCKS5
 * username/password request), so hostnames go to the gateway unresolved.
 */
public final class NativeTor {
    private NativeTor() {}

    /** Loads libredvault_tor from the path in system property rv.lib, or by name. */
    public static void load() {
        String path = System.getProperty("rv.lib");
        if (path != null && !path.isEmpty()) {
            System.load(path);
        } else {
            System.loadLibrary("redvault_tor");
        }
    }

    /** Starts Tor and the SOCKS gate; throws IllegalStateException on a bad config. */
    public static native long nativeStart(String configJson);

    /** {"host":"127.0.0.1","port":N,"username":"…","password":"…"} */
    public static native String nativeEndpoint(long handle);

    /** {"state":"connecting|connected|unavailable","progress":…,"detail":…,…} */
    public static native String nativeStatus(long handle);

    /** True while the app is backgrounded with nothing to send. */
    public static native void nativeSetDormant(long handle, boolean dormant);

    /** Closes the SOCKS port and stops Tor; the handle is invalid afterwards. */
    public static native void nativeStop(long handle);
}
