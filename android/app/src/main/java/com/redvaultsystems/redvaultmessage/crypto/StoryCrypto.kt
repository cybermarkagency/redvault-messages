// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium
import com.redvaultsystems.redvaultmessage.data.model.isJsSpace

class StoryCryptoException(message: String) : Exception(message)

// Android port of the Stories half of ui/src/lib/storyCrypto.ts (design in docs/stories.md and
// docs/stories-advanced/architecture.md). Only libsodium's own primitives, nothing of our own:
//
//   - every story has a random 32-byte key K;
//   - everything a story shows is one JSON blob sealed with K (XChaCha20-Poly1305), bound to the
//     story's id through the additional data "redvault-story-meta-v1" || story id (16 bytes);
//   - K reaches each viewer as nonce(24) || crypto_box(payload, viewer public key, author private
//     key) with payload = "RVST" | 1 | story id (16) | K (32): 93 bytes. crypto_box is authenticated,
//     so the viewer knows the author made it, and the id inside stops a key being moved onto another
//     story;
//   - photos and videos are crypto_secretstream files (FileCrypto), their key and header inside the
//     sealed details.
//
// Compatibility with the web app is pinned by test/.../stories/StoryCryptoVectorsTest.kt against
// vectors the web code made. Nothing here sends a key anywhere.
class StoryCrypto(private val sodium: LazySodium) {

    /** A fresh random story id (a v4 UUID): the author's app picks it so the key can be bound to it before upload. */
    fun newStoryId(): String {
        val b = sodium.randomBytesBuf(ID_BYTES)
        b[6] = ((b[6].toInt() and 0x0f) or 0x40).toByte()
        b[8] = ((b[8].toInt() and 0x3f) or 0x80).toByte()
        val hex = b.joinToString("") { "%02x".format(it.toInt() and 0xff) }
        return "${hex.substring(0, 8)}-${hex.substring(8, 12)}-${hex.substring(12, 16)}-${hex.substring(16, 20)}-${hex.substring(20)}"
    }

    fun newStoryKey(): ByteArray = sodium.randomBytesBuf(KEY_BYTES)

    /** The bound payload K travels in (exposed for tests). */
    fun storyKeyPayload(storyId: String, key: ByteArray): ByteArray {
        if (key.size != KEY_BYTES) throw StoryCryptoException("story keys are 32 bytes")
        return MAGIC + byteArrayOf(VERSION) + uuidBytes(storyId) + key
    }

    /** Seals a story's key to one viewer (the author's own copy is sealed to themself). */
    fun wrapStoryKey(storyId: String, key: ByteArray, recipientPublicKey: ByteArray, authorPrivateKey: ByteArray): ByteArray {
        val payload = storyKeyPayload(storyId, key)
        val nonce = sodium.randomBytesBuf(BOX_NONCE_BYTES)
        val box = ByteArray(payload.size + BOX_MAC_BYTES)
        if (!sodium.cryptoBoxEasy(box, payload, payload.size.toLong(), nonce, recipientPublicKey, authorPrivateKey)) {
            throw StoryCryptoException("Failed to seal the story key")
        }
        payload.fill(0)
        return nonce + box
    }

    /** Opens a viewer's copy, checking the author made it and that it's for exactly this story. */
    fun unwrapStoryKey(storyId: String, wrapped: ByteArray, authorPublicKey: ByteArray, viewerPrivateKey: ByteArray): ByteArray {
        if (wrapped.size != BOX_NONCE_BYTES + PAYLOAD_BYTES + BOX_MAC_BYTES) throw StoryCryptoException("malformed story key")
        val expectedId = uuidBytes(storyId)
        val nonce = wrapped.copyOfRange(0, BOX_NONCE_BYTES)
        val box = wrapped.copyOfRange(BOX_NONCE_BYTES, wrapped.size)
        val payload = ByteArray(PAYLOAD_BYTES)
        if (!sodium.cryptoBoxOpenEasy(payload, box, box.size.toLong(), nonce, authorPublicKey, viewerPrivateKey)) {
            throw StoryCryptoException("this story key wasn't made by its author for you")
        }
        try {
            if (!constantTimeEquals(payload.copyOfRange(0, 4), MAGIC) || payload[4] != VERSION) throw StoryCryptoException("unknown story key format")
            if (!constantTimeEquals(payload.copyOfRange(5, 5 + ID_BYTES), expectedId)) throw StoryCryptoException("this key belongs to another story")
            return payload.copyOfRange(5 + ID_BYTES, PAYLOAD_BYTES)
        } finally {
            payload.fill(0)
        }
    }

    /** Seals a story's details (the JSON text) with its key. [nonce] is random unless a test pins it. */
    fun sealStoryMeta(storyId: String, key: ByteArray, metaJson: String, nonce: ByteArray = sodium.randomBytesBuf(AEAD_NONCE_BYTES)): AeadCiphertext =
        AeadCiphertext(aeadSeal(metaJson.toByteArray(Charsets.UTF_8), ad(META_AD, storyId), nonce, key), nonce)

    /** The story's details as JSON text; throws if they weren't sealed with this key for this story. */
    fun openStoryMeta(storyId: String, key: ByteArray, ciphertext: ByteArray, nonce: ByteArray): String {
        val plain = aeadOpen(ciphertext, ad(META_AD, storyId), nonce, key) ?: throw StoryCryptoException("this story can't be opened")
        return String(plain, Charsets.UTF_8)
    }

    /**
     * The author's story settings as JSON text (close friends, hidden from, muted, whether views are shared),
     * sealed with a key derived one-way from the account's private key: the server can't read them.
     */
    fun openStoryPrefs(ciphertext: ByteArray, nonce: ByteArray, privateKey: ByteArray): String {
        val key = prefsKey(privateKey)
        try {
            val plain = aeadOpen(ciphertext, PREFS_AD.toByteArray(Charsets.UTF_8), nonce, key) ?: throw StoryCryptoException("your story settings can't be opened")
            return String(plain, Charsets.UTF_8)
        } finally {
            key.fill(0)
        }
    }

    /** Seals the author's settings (the JSON text) so only this account's devices can read them. [nonce] is random unless a test pins it. */
    fun sealStoryPrefs(json: String, privateKey: ByteArray, nonce: ByteArray = sodium.randomBytesBuf(AEAD_NONCE_BYTES)): AeadCiphertext {
        val key = prefsKey(privateKey)
        try {
            return AeadCiphertext(aeadSeal(json.toByteArray(Charsets.UTF_8), PREFS_AD.toByteArray(Charsets.UTF_8), nonce, key), nonce)
        } finally {
            key.fill(0)
        }
    }

    /** A highlight's title, sealed with its cover story's key: whoever can see the cover can read the title. At most 40 characters. */
    fun sealHighlightTitle(title: String, coverStoryId: String, coverKey: ByteArray, nonce: ByteArray = sodium.randomBytesBuf(AEAD_NONCE_BYTES)): AeadCiphertext {
        val t = title.trim { isJsSpace(it) }.take(MAX_HIGHLIGHT_TITLE)
        return AeadCiphertext(aeadSeal(t.toByteArray(Charsets.UTF_8), ad(TITLE_AD, coverStoryId), nonce, coverKey), nonce)
    }

    /** Null when it can't be read (for instance because this viewer can't see the cover story). */
    fun openHighlightTitle(ciphertext: ByteArray, nonce: ByteArray, coverStoryId: String, coverKey: ByteArray): String? {
        val plain = aeadOpen(ciphertext, ad(TITLE_AD, coverStoryId), nonce, coverKey) ?: return null
        return String(plain, Charsets.UTF_8).take(MAX_HIGHLIGHT_TITLE)
    }

    /**
     * Seals an unfinished story (the JSON text) for this phone: a key derived one-way from the account's private key, bound to the account and
     * the draft's id so a draft can't be moved to another account or another draft. [nonce] is random unless a test pins it.
     */
    fun sealDraft(json: String, userId: String, draftId: String, privateKey: ByteArray, nonce: ByteArray = sodium.randomBytesBuf(AEAD_NONCE_BYTES)): AeadCiphertext {
        val key = draftKey(privateKey)
        try {
            return AeadCiphertext(aeadSeal(json.toByteArray(Charsets.UTF_8), draftAd(userId, draftId), nonce, key), nonce)
        } finally {
            key.fill(0)
        }
    }

    /** Null when it isn't this account's, isn't this draft's, or was changed. */
    fun openDraft(ciphertext: ByteArray, nonce: ByteArray, userId: String, draftId: String, privateKey: ByteArray): String? {
        val key = draftKey(privateKey)
        try {
            return aeadOpen(ciphertext, draftAd(userId, draftId), nonce, key)?.let { String(it, Charsets.UTF_8) }
        } finally {
            key.fill(0)
        }
    }

    private fun draftKey(privateKey: ByteArray): ByteArray {
        val out = ByteArray(KEY_BYTES)
        val label = DRAFT_KEY_LABEL.toByteArray(Charsets.UTF_8)
        if (!sodium.cryptoGenericHash(out, out.size, label, label.size.toLong(), privateKey, privateKey.size)) throw StoryCryptoException("Key derivation failed")
        return out
    }

    private fun draftAd(userId: String, draftId: String): ByteArray = "$DRAFT_AD:$userId:$draftId".toByteArray(Charsets.UTF_8)

    private fun prefsKey(privateKey: ByteArray): ByteArray {
        // A dedicated, one-way-derived key: the private key itself is never used directly.
        val out = ByteArray(KEY_BYTES)
        val label = PREFS_AD.toByteArray(Charsets.UTF_8)
        if (!sodium.cryptoGenericHash(out, out.size, label, label.size.toLong(), privateKey, privateKey.size)) {
            throw StoryCryptoException("Key derivation failed")
        }
        return out
    }

    private fun ad(label: String, storyId: String): ByteArray = label.toByteArray(Charsets.UTF_8) + uuidBytes(storyId)

    private fun aeadSeal(plain: ByteArray, additionalData: ByteArray, nonce: ByteArray, key: ByteArray): ByteArray {
        if (nonce.size != AEAD_NONCE_BYTES || key.size != KEY_BYTES) throw StoryCryptoException("AEAD encryption failed: malformed input")
        val cipher = ByteArray(plain.size + AEAD_TAG_BYTES)
        val cipherLen = LongArray(1)
        val ok = sodium.cryptoAeadXChaCha20Poly1305IetfEncrypt(
            cipher, cipherLen, plain, plain.size.toLong(), additionalData, additionalData.size.toLong(), null, nonce, key,
        )
        if (!ok) throw StoryCryptoException("AEAD encryption failed")
        return cipher.copyOf(cipherLen[0].toInt())
    }

    /** Null when the ciphertext, nonce, key or additional data don't match (or the input is malformed). */
    private fun aeadOpen(cipher: ByteArray, additionalData: ByteArray, nonce: ByteArray, key: ByteArray): ByteArray? {
        if (cipher.size < AEAD_TAG_BYTES || nonce.size != AEAD_NONCE_BYTES || key.size != KEY_BYTES) return null
        val plain = ByteArray(cipher.size - AEAD_TAG_BYTES)
        val plainLen = LongArray(1)
        val ok = sodium.cryptoAeadXChaCha20Poly1305IetfDecrypt(
            plain, plainLen, null, cipher, cipher.size.toLong(), additionalData, additionalData.size.toLong(), nonce, key,
        )
        return if (ok) plain.copyOf(plainLen[0].toInt()) else null
    }

    companion object {
        private val MAGIC = byteArrayOf(0x52, 0x56, 0x53, 0x54) // "RVST"
        private const val VERSION: Byte = 1
        const val KEY_BYTES = 32
        const val ID_BYTES = 16
        private const val PAYLOAD_BYTES = 4 + 1 + ID_BYTES + KEY_BYTES
        private const val BOX_NONCE_BYTES = 24
        private const val BOX_MAC_BYTES = 16
        private const val AEAD_NONCE_BYTES = 24
        private const val AEAD_TAG_BYTES = 16
        private const val META_AD = "redvault-story-meta-v1"
        private const val PREFS_AD = "redvault-story-prefs-v1"
        private const val TITLE_AD = "redvault-story-highlight-v1"
        private const val DRAFT_KEY_LABEL = "redvault-story-drafts-v1"
        private const val DRAFT_AD = "redvault-story-draft-v1"
        const val MAX_HIGHLIGHT_TITLE = 40

        /** A story id as its 16 bytes (dashes optional, any case); throws if it isn't one. */
        fun uuidBytes(id: String): ByteArray {
            val hex = id.replace("-", "").lowercase()
            if (hex.length != 32 || !hex.all { it in '0'..'9' || it in 'a'..'f' }) throw StoryCryptoException("not a story id")
            return ByteArray(ID_BYTES) { hex.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
        }

        private fun constantTimeEquals(a: ByteArray, b: ByteArray): Boolean {
            if (a.size != b.size) return false
            var diff = 0
            for (i in a.indices) diff = diff or (a[i].toInt() xor b[i].toInt())
            return diff == 0
        }
    }
}
