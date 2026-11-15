// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

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

    // --- Part C1: private channels encrypted with the link (docs/signal-integration/10-part-c1-private-channels.md) ---
    //
    // wrapKey = BLAKE2b-256(key = secret, "redvault-channel-v1:wrap"). Each epoch's 32-byte content key is wrapped
    // under it (XChaCha20-Poly1305, AD "RVCHK|v1|<channelId>|<epoch>", stored as nonce ‖ ciphertext), and posts are
    // sealed with the content key (AD "RVCHP|v1|<channelId>|<epoch>"). Pinned by ChannelE2eeVectorsTest.

    /** The key that wraps every epoch's content key, from the link's secret (base64url, as after the #). */
    fun wrapKey(secret: String): ByteArray {
        val key = GroupInviteLink.secretFromFragment(secret) ?: throw CryptoException("That isn't a channel link's secret")
        val message = WRAP_CONTEXT.toByteArray(Charsets.UTF_8)
        val out = ByteArray(KEY_BYTES)
        if (!sodium.cryptoGenericHash(out, out.size, message, message.size.toLong(), key, key.size)) throw CryptoException("Hashing failed")
        return out
    }

    /** A fresh content key for a new epoch. */
    fun newContentKey(): ByteArray = e2e.randomBytes(KEY_BYTES)

    /** [contentKey] wrapped for [epoch] of [channelId] under [wrapKey]: base64 of nonce(24) ‖ ciphertext(48). */
    fun wrapContentKey(contentKey: ByteArray, channelId: String, epoch: Int, wrapKey: ByteArray, nonce: ByteArray = e2e.randomBytes(NONCE_BYTES)): String {
        if (contentKey.size != KEY_BYTES) throw CryptoException("A content key is $KEY_BYTES bytes")
        return Base64Codec.encode(nonce + seal(contentKey, keyAd(channelId, epoch), nonce, wrapKey))
    }

    /** The content key of [epoch], or null when it doesn't open (another link, channel or epoch, or changed bytes). */
    fun unwrapContentKey(wrapped: String, channelId: String, epoch: Int, wrapKey: ByteArray): ByteArray? {
        val bytes = runCatching { Base64Codec.decode(wrapped) }.getOrNull() ?: return null
        if (bytes.size != WRAPPED_BYTES) return null
        return open(bytes.copyOfRange(NONCE_BYTES, bytes.size), keyAd(channelId, epoch), bytes.copyOfRange(0, NONCE_BYTES), wrapKey)
            ?.takeIf { it.size == KEY_BYTES }
    }

    /** A post's (or suggestion's) plaintext JSON sealed under [epoch]'s content key, with a fresh nonce. */
    fun sealPost(plaintext: String, contentKey: ByteArray, channelId: String, epoch: Int, nonce: ByteArray = e2e.randomBytes(NONCE_BYTES)): SealedPost =
        SealedPost(epoch, Base64Codec.encode(nonce), Base64Codec.encode(seal(plaintext.toByteArray(Charsets.UTF_8), postAd(channelId, epoch), nonce, contentKey)))

    /** The plaintext JSON, or null when it doesn't open with this key, channel and epoch. */
    fun openPost(nonce: String, ciphertext: String, contentKey: ByteArray, channelId: String, epoch: Int): String? {
        val n = runCatching { Base64Codec.decode(nonce) }.getOrNull() ?: return null
        val c = runCatching { Base64Codec.decode(ciphertext) }.getOrNull() ?: return null
        val plain = open(c, postAd(channelId, epoch), n, contentKey) ?: return null
        return runCatching { Charsets.UTF_8.newDecoder().decode(java.nio.ByteBuffer.wrap(plain)).toString() }.getOrNull()
    }

    private fun seal(plain: ByteArray, ad: ByteArray, nonce: ByteArray, key: ByteArray): ByteArray {
        if (nonce.size != NONCE_BYTES || key.size != KEY_BYTES) throw CryptoException("AEAD encryption failed: malformed input")
        val cipher = ByteArray(plain.size + TAG_BYTES)
        val len = LongArray(1)
        if (!sodium.cryptoAeadXChaCha20Poly1305IetfEncrypt(cipher, len, plain, plain.size.toLong(), ad, ad.size.toLong(), null, nonce, key)) {
            throw CryptoException("AEAD encryption failed")
        }
        return cipher.copyOf(len[0].toInt())
    }

    private fun open(cipher: ByteArray, ad: ByteArray, nonce: ByteArray, key: ByteArray): ByteArray? {
        if (cipher.size < TAG_BYTES || nonce.size != NONCE_BYTES || key.size != KEY_BYTES) return null
        val plain = ByteArray(cipher.size - TAG_BYTES)
        val len = LongArray(1)
        val ok = sodium.cryptoAeadXChaCha20Poly1305IetfDecrypt(plain, len, null, cipher, cipher.size.toLong(), ad, ad.size.toLong(), nonce, key)
        return if (ok) plain.copyOf(len[0].toInt()) else null
    }

    /** A sealed post or suggestion, as the server stores it (standard base64). */
    data class SealedPost(val keyEpoch: Int, val nonce: String, val ciphertext: String)

    companion object {
        const val SECRET_BYTES = 32
        const val KEY_BYTES = 32
        const val NONCE_BYTES = 24
        private const val TAG_BYTES = 16
        /** nonce(24) ‖ content key(32) ‖ tag(16). */
        const val WRAPPED_BYTES = NONCE_BYTES + KEY_BYTES + TAG_BYTES
        private const val WRAP_CONTEXT = "redvault-channel-v1:wrap"

        /** The wrapped key's additional data: the lowercase channel id and the epoch. */
        fun keyAd(channelId: String, epoch: Int): ByteArray = "RVCHK|v1|${channelId.lowercase()}|$epoch".toByteArray(Charsets.UTF_8)

        /** A post's (and suggestion's) additional data. */
        fun postAd(channelId: String, epoch: Int): ByteArray = "RVCHP|v1|${channelId.lowercase()}|$epoch".toByteArray(Charsets.UTF_8)
        private const val TOKEN_BYTES = 32
        private const val ACCESS_CONTEXT = "redvault-channel-v1:access"

        /** The secret in a link's fragment (with or without its "#"), still base64url, or null if it isn't one. */
        fun secretFromFragment(fragment: String): String? =
            GroupInviteLink.secretFromFragment(fragment)?.let(GroupInviteLink::encodeSecret)
    }
}
