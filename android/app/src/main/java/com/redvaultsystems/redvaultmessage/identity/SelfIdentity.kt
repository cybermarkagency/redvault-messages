package com.redvaultsystems.redvaultmessage.identity

import com.redvaultsystems.redvaultmessage.crypto.WrappedPrivateKey

/**
 * The signed-in user. Like the web client, the unwrapped private key only
 * ever lives in memory: it's gone when the process dies, and getting it
 * back takes the password again.
 */
class SelfIdentity(
    val handle: String,
    val userId: String,
    val publicKey: ByteArray,
    val privateKey: ByteArray,
    val avatarUrl: String?,
    val role: String,
    val subscriptionTier: String,
    val mfaEnabled: Boolean,
    /**
     * The private key as the server stores it (still encrypted under the
     * password). Kept so the password vault can check the account password
     * on this phone, like the desktop app, without asking the server.
     */
    @Volatile var wrappedKey: WrappedPrivateKey? = null,
) {
    /** Overwrites the private key bytes. Call when signing out. */
    fun wipe() {
        privateKey.fill(0)
    }
}
