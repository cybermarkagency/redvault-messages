package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium

// Private channel links: a port of the private-link half of the web's
// lib/channels.ts, byte for byte, so a link made on any of the apps opens on
// the others.
//
// A private channel's link is …/?channel=<id>#<secret>. The 32-byte secret
// stays in the URL fragment, which never reaches a server; readers show the
// server an access token derived from it (X-Channel-Access), of which the
// server keeps only a hash. The owner keeps the secret sealed to their own
// public key on the server, so each of their devices can show the link again.
//
// Pinned to the web by test/.../ChannelCryptoInteropTest.kt, whose vectors
// the web's own code made (desktop/tests/vectors/channel_vectors.json).

class ChannelCrypto(private val sodium: LazySodium, private val e2e: E2eCrypto) {

    /** A new private link's secret, base64url without padding (what goes after the #). */
    fun newSecret(): String = GroupInviteLink.encodeSecret(e2e.randomBytes(SECRET_BYTES))

    /** What a reader shows the server (standard base64): derived from the secret, which can't be worked back from it. */
    fun accessToken(secret: String): String {
        val key = GroupInviteLink.secretFromFragment(secret) ?: throw CryptoException("That isn't a channel link's secret")
        val message = ACCESS_CONTEXT.toByteArray(Charsets.UTF_8)
        val out = ByteArray(TOKEN_BYTES)
        if (!sodium.cryptoGenericHash(out, out.size, message, message.size.toLong(), key, key.size)) throw CryptoException("Hashing failed")
        return Base64Codec.encode(out)
    }

    /** The secret (its base64url text, as the web seals it) sealed to the owner's public key, standard base64. */
    fun sealSecret(secret: String, ownerPublicKey: ByteArray): String =
        Base64Codec.encode(e2e.sealConversationKey(secret.toByteArray(Charsets.UTF_8), ownerPublicKey))

    /** The owner's private link secret, unsealed with their key pair; null when there's none, it won't open, or it isn't a secret. */
    fun openSecret(sealed: String?, publicKey: ByteArray, privateKey: ByteArray): String? {
        if (sealed.isNullOrEmpty()) return null
        return runCatching {
            String(e2e.unsealConversationKey(Base64Codec.decode(sealed), publicKey, privateKey), Charsets.UTF_8)
        }.getOrNull()?.takeIf { GroupInviteLink.secretFromFragment(it) != null }
    }

    companion object {
        const val SECRET_BYTES = 32
        private const val TOKEN_BYTES = 32
        private const val ACCESS_CONTEXT = "redvault-channel-v1:access"

        /** The secret in a link's fragment (with or without its "#"), still base64url, or null if it isn't one. */
        fun secretFromFragment(fragment: String): String? =
            GroupInviteLink.secretFromFragment(fragment)?.let(GroupInviteLink::encodeSecret)
    }
}
