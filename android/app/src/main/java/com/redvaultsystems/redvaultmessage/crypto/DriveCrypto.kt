// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium
import com.goterl.lazysodium.interfaces.PwHash
import com.sun.jna.NativeLong
import java.util.Base64

// Encrypted cloud storage ("Files"), on the phone. A port of the desktop
// app's crypto/drive_crypto (desktop/core/src/drive_crypto.cpp): the same
// keys, padding and byte layouts, so a file stored from one app opens in
// the other.
//
//   - Each file is encrypted with its own random key (FileCrypto, the same
//     format as files sent in chats).
//   - Its metadata (name, folder, type, size, dates, version and that key)
//     is JSON sealed with XChaCha20-Poly1305 under the drive key, derived
//     from the account's private key ("redvault-drive-v1"), padded to
//     128-byte buckets.
//   - Share link: the metadata sealed under a key from the link's secret
//     (only ever in the URL fragment), mixed with an Argon2id hash of a
//     password when the link has one.
//   - Locked files: the file's key can also be wrapped under a key of the
//     person's own or a password (see data/FileLock.kt).

class PasswordKdf(val salt: ByteArray, val opsLimit: Long, val memLimit: Long)

class WrappedKey(val ciphertext: ByteArray, val nonce: ByteArray)

class DriveCrypto(private val sodium: LazySodium, private val e2e: E2eCrypto) {

    /** BLAKE2b(32, label, key = private key): a key only this account's devices can derive. */
    fun deriveLocalKey(privateKey: ByteArray, label: String): ByteArray {
        val message = label.toByteArray(Charsets.UTF_8)
        val out = ByteArray(32)
        if (!sodium.cryptoGenericHash(out, out.size, message, message.size.toLong(), privateKey, privateKey.size)) {
            throw CryptoException("Key derivation failed")
        }
        return out
    }

    fun driveKey(privateKey: ByteArray): ByteArray = deriveLocalKey(privateKey, "redvault-drive-v1")

    /** Pads with trailing spaces to the next 128-byte bucket (JSON parsers ignore them), then seals. */
    fun sealMeta(json: String, key: ByteArray): AeadCiphertext {
        val bytes = json.toByteArray(Charsets.UTF_8).size
        val target = ((bytes / META_BUCKET) + 1) * META_BUCKET
        return e2e.encryptWithConversationKey(json + " ".repeat(target - bytes), key)
    }

    fun openMeta(ciphertext: ByteArray, nonce: ByteArray, key: ByteArray): String =
        e2e.decryptWithConversationKey(ciphertext, nonce, key).trimEnd(' ')

    fun newPasswordSalt(): ByteArray = e2e.randomBytes(PWHASH_SALT_BYTES)

    /**
     * The key a share link's metadata is sealed under. No password: the
     * secret itself. With one: BLAKE2b(32, secret, key = Argon2id(password,
     * salt, interactive)). Slow with a password: call off the main thread.
     */
    fun linkKey(secret: ByteArray, password: String, salt: ByteArray): ByteArray {
        if (password.isEmpty()) return secret.copyOf()
        if (salt.size != PWHASH_SALT_BYTES) throw CryptoException("Wrong salt size")
        val stretched = argon2(password, salt, OPS_INTERACTIVE, MEM_INTERACTIVE)
        try {
            val out = ByteArray(32)
            if (!sodium.cryptoGenericHash(out, out.size, secret, secret.size.toLong(), stretched, stretched.size)) {
                throw CryptoException("Key derivation failed")
            }
            return out
        } finally {
            stretched.fill(0)
        }
    }

    fun newLinkSecret(): ByteArray = e2e.randomBytes(32)

    /** The secret as it travels in a link's fragment: base64url without padding. */
    fun linkSecretToFragment(secret: ByteArray): String = Base64.getUrlEncoder().withoutPadding().encodeToString(secret)

    fun linkSecretFromFragment(fragment: String): ByteArray? =
        runCatching { Base64.getUrlDecoder().decode(fragment.removePrefix("#")) }.getOrNull()?.takeIf { it.size == 32 }

    /** https://host/redvaultmessages/?file=<linkId>#<secret, base64url>. */
    fun fileLinkUrl(webAppBase: String, linkId: String, secret: ByteArray): String {
        val base = if (webAppBase.endsWith("/")) webAppBase else "$webAppBase/"
        return "${base}?file=$linkId#${linkSecretToFragment(secret)}"
    }

    // --- Locking a file's key under a key or password of the person's own ---

    fun newFileLockKey(): ByteArray = e2e.randomBytes(FILE_LOCK_KEY_BYTES)

    /**
     * A 256-bit key made outside the app: 64 hex characters (optionally
     * grouped with dashes or colons) or base64 / base64url of 32 bytes.
     * Whitespace is ignored. Null for anything else.
     */
    fun parseFileLockKey(text: String): ByteArray? {
        val compact = text.filterNot { it.isWhitespace() }
        val hex = compact.filterNot { it == '-' || it == ':' }
        if (hex.length == FILE_LOCK_KEY_BYTES * 2 && hex.all { it.isHexDigit() }) {
            return ByteArray(FILE_LOCK_KEY_BYTES) { i -> hex.substring(i * 2, i * 2 + 2).toInt(16).toByte() }
        }
        val decoders = listOf(
            { s: String -> Base64.getDecoder().decode(s) },
            { s: String -> Base64.getUrlDecoder().decode(s) },
        )
        for (decode in decoders) {
            val bytes = runCatching { decode(compact) }.getOrNull()
            if (bytes != null && bytes.size == FILE_LOCK_KEY_BYTES) return bytes
        }
        return null
    }

    fun fileLockKeyHex(key: ByteArray): String = key.joinToString("") { "%02x".format(it.toInt() and 0xff) }

    /** "7F3A-19C2-E04B-D5A8": BLAKE2b(8, "redvault-file-key-fingerprint-v1", key = the key). */
    fun fileLockKeyFingerprint(key: ByteArray): String {
        val label = "redvault-file-key-fingerprint-v1".toByteArray(Charsets.UTF_8)
        val digest = ByteArray(8)
        if (!sodium.cryptoGenericHash(digest, digest.size, label, label.size.toLong(), key, key.size)) {
            throw CryptoException("Fingerprint failed")
        }
        return digest.toList().chunked(2).joinToString("-") { pair -> pair.joinToString("") { "%02X".format(it.toInt() and 0xff) } }
    }

    /** A fresh salt with libsodium's MODERATE limits (about 256 MiB). */
    fun newPasswordKdf(): PasswordKdf = PasswordKdf(newPasswordSalt(), OPS_MODERATE, MEM_MODERATE)

    /**
     * Argon2id(password) under [kdf]. Refuses settings outside libsodium's
     * interactive..sensitive range, so stored metadata can't make this app
     * spend unbounded time or memory. Slow: call off the main thread.
     */
    fun passwordKey(password: String, kdf: PasswordKdf): ByteArray {
        if (kdf.salt.size != PWHASH_SALT_BYTES) throw CryptoException("Wrong salt size")
        if (kdf.opsLimit < OPS_INTERACTIVE || kdf.opsLimit > OPS_SENSITIVE || kdf.memLimit < MEM_INTERACTIVE || kdf.memLimit > MEM_SENSITIVE) {
            throw CryptoException("Password settings out of range")
        }
        return argon2(password, kdf.salt, kdf.opsLimit, kdf.memLimit)
    }

    /** Locks [key] under [wrappingKey] with XChaCha20-Poly1305; [context] is authenticated with it, not stored. */
    fun wrapKey(key: ByteArray, wrappingKey: ByteArray, context: String): WrappedKey {
        if (wrappingKey.size != 32) throw CryptoException("Wrong wrapping key size")
        val nonce = e2e.randomBytes(24)
        val ad = context.toByteArray(Charsets.UTF_8)
        val out = ByteArray(key.size + 16)
        val len = LongArray(1)
        if (!sodium.cryptoAeadXChaCha20Poly1305IetfEncrypt(out, len, key, key.size.toLong(), ad, ad.size.toLong(), null, nonce, wrappingKey)) {
            throw CryptoException("Couldn't lock the key")
        }
        return WrappedKey(out.copyOf(len[0].toInt()), nonce)
    }

    /** Throws [CryptoException] for a wrong key or anything altered. */
    fun unwrapKey(wrapped: WrappedKey, wrappingKey: ByteArray, context: String): ByteArray {
        if (wrappingKey.size != 32 || wrapped.nonce.size != 24 || wrapped.ciphertext.size <= 16) throw CryptoException("Couldn't unlock the key")
        val ad = context.toByteArray(Charsets.UTF_8)
        val out = ByteArray(wrapped.ciphertext.size - 16)
        val len = LongArray(1)
        if (!sodium.cryptoAeadXChaCha20Poly1305IetfDecrypt(
                out, len, null, wrapped.ciphertext, wrapped.ciphertext.size.toLong(), ad, ad.size.toLong(), wrapped.nonce, wrappingKey,
            )
        ) {
            throw CryptoException("Couldn't unlock the key")
        }
        return out.copyOf(len[0].toInt())
    }

    private fun argon2(password: String, salt: ByteArray, ops: Long, mem: Long): ByteArray {
        val passwordBytes = password.toByteArray(Charsets.UTF_8)
        val key = ByteArray(32)
        try {
            val ok = sodium.cryptoPwHash(
                key, key.size, passwordBytes, passwordBytes.size, salt, ops, NativeLong(mem), PwHash.Alg.PWHASH_ALG_ARGON2ID13,
            )
            if (!ok) throw CryptoException("Out of memory deriving the password key")
        } finally {
            passwordBytes.fill(0)
        }
        return key
    }

    companion object {
        const val FILE_LOCK_KEY_BYTES = 32
        const val PWHASH_SALT_BYTES = 16
        private const val META_BUCKET = 128

        // libsodium's crypto_pwhash_argon2id limits.
        const val OPS_INTERACTIVE = 2L
        const val MEM_INTERACTIVE = 67_108_864L
        const val OPS_MODERATE = 3L
        const val MEM_MODERATE = 268_435_456L
        const val OPS_SENSITIVE = 4L
        const val MEM_SENSITIVE = 1_073_741_824L

        private fun Char.isHexDigit() = this in '0'..'9' || this in 'a'..'f' || this in 'A'..'F'
    }
}
