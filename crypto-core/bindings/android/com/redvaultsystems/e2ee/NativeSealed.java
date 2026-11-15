// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.e2ee;

import java.io.IOException;

/**
 * Sealed requests for the backup route (docs/tor-sealed-fallback.md), from the shared Rust
 * engine (crypto-core/src/jni_binding.rs, sealed-transport feature). Load the library with
 * {@link NativeE2ee#load()} first. Every failure throws IOException.
 */
public final class NativeSealed {
    private NativeSealed() {}

    /**
     * Seals one request to the gateway's key. {@code headJson} is
     * {@code {"method":"POST","path":"/api/...","headers":[["name","value"],...]}}.
     * Returns an 8-byte big-endian handle followed by the message/ohttp-req bytes to POST.
     * The handle must go to {@link #open} or {@link #discard} exactly once.
     */
    public static native byte[] seal(byte[] keyConfig, String headJson, byte[] body) throws IOException;

    /**
     * Opens a message/ohttp-res reply; always consumes the handle. Returns a 4-byte big-endian
     * length, the head JSON ({@code {"status":200,"headers":[["name","value"],...]}}), then the body.
     */
    public static native byte[] open(long handle, byte[] reply) throws IOException;

    /** Drops a handle whose reply never came. */
    public static native void discard(long handle);
}
