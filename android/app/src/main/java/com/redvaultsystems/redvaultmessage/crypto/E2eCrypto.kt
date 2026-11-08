// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium
import com.goterl.lazysodium.interfaces.PwHash
import com.sun.jna.NativeLong
import java.security.MessageDigest

// Android port of ui/src/lib/e2eCrypto.ts (and client/src/crypto.ts, which
// documents the design in full). Same libsodium primitives, same parameters,
// same byte layouts, so web and Android clients can talk to each other:
//
//   - X25519 keypair per user
//   - private key wrapped with an Argon2id-derived key (from the password)
//   - random symmetric conversation key, sealed once per participant with
//     crypto_box_seal (anonymous ECIES over X25519)
//   - message bodies encrypted with the conversation key via
//     XChaCha20-Poly1305 (AEAD)
//
// Compatibility with the web client is pinned by
// androidTest/.../crypto/E2eCryptoCompatTest.kt, whose vectors were
// produced by the web client's own libsodium build.
//
// Nothing here ever sends a private key or plaintext message anywhere.

class KeyPair(val publicKey: ByteArray, val privateKey: ByteArray)

class WrappedPrivateKey(
    val encryptedPrivateKey: ByteArray,
    val nonce: ByteArray,
    val kdfSalt: ByteArray,
    /** Null on data from before settings were stored: those all used [E2eCrypto.KDF_INTERACTIVE]. */
    val kdf: KdfSettings? = null,
)

/** Argon2id's cost: passes, and memory in bytes. */
data class KdfSettings(val ops: Long, val mem: Long)

/** The server asked for key-stretching settings this app won't use. Its message is safe to show, and nothing was derived or sent. */
class UnsafeKdfException : Exception(E2eCrypto.SIGN_IN_DETAILS_WRONG)

class AeadCiphertext(val ciphertext: ByteArray, val nonce: ByteArray)

/** An account's Ed25519 message-signing key pair; [secretKey] is libsodium's 64-byte form (seed ‖ public key). */
class SigningKeyPair(val publicKey: ByteArray, val secretKey: ByteArray)

class CryptoException(message: String) : Exception(message)

class E2eCrypto(private val sodium: LazySodium) {

    /** Generates a fresh X25519 keypair for E2E messaging. */
    fun generateKeyPair(): KeyPair {
        val publicKey = ByteArray(BOX_PUBLIC_KEY_BYTES)
        val privateKey = ByteArray(BOX_SECRET_KEY_BYTES)
        if (!sodium.cryptoBoxKeypair(publicKey, privateKey)) throw CryptoException("Keypair generation failed")
        return KeyPair(publicKey, privateKey)
    }

    /**
     * Derives a symmetric key from a password + salt via Argon2id. Deliberately
     * slow (64 MiB at the least, a noticeable fraction of a second): call it
     * off the main thread. [kdf] may come from the server, so it's checked first.
     */
    fun deriveWrapKey(password: String, kdfSalt: ByteArray, kdf: KdfSettings = KDF_INTERACTIVE): ByteArray {
        requireSafeKdf(kdf)
        if (kdfSalt.size != PWHASH_SALT_BYTES) throw CryptoException("kdfSalt must be $PWHASH_SALT_BYTES bytes")
        // libsodium.js hashes a string password as its UTF-8 bytes.
        val passwordBytes = password.toByteArray(Charsets.UTF_8)
        val key = ByteArray(AEAD_KEY_BYTES)
        try {
            val ok = sodium.cryptoPwHash(
                key,
                key.size,
                passwordBytes,
                passwordBytes.size,
                kdfSalt,
                kdf.ops,
                NativeLong(kdf.mem),
                PwHash.Alg.PWHASH_ALG_ARGON2ID13,
            )
            if (!ok) throw CryptoException("Key derivation failed")
        } finally {
            passwordBytes.fill(0)
        }
        return key
    }

    /** Wraps a private key with a password-derived key under a fresh random salt. Store all three outputs. */
    /**
     * The value sent to the server instead of the password (docs/auth-key-split.md).
     * One-way from the unlock key, so the server can't get from it to the private key.
     */
    fun deriveSignInKey(password: String, kdfSalt: ByteArray, kdf: KdfSettings = KDF_INTERACTIVE): String {
        val wrapKey = deriveWrapKey(password, kdfSalt, kdf)
        val signInKey = ByteArray(SIGN_IN_KEY_BYTES)
        try {
            val context = SIGN_IN_KDF_CONTEXT.toByteArray(Charsets.US_ASCII)
            if (sodium.cryptoKdfDeriveFromKey(signInKey, signInKey.size, SIGN_IN_KDF_SUBKEY_ID, context, wrapKey) != 0) {
                throw CryptoException("Sign-in key derivation failed")
            }
            return Base64Codec.encode(signInKey)
        } finally {
            wrapKey.fill(0)
            signInKey.fill(0)
        }
    }

    fun wrapPrivateKey(privateKey: ByteArray, password: String): WrappedPrivateKey {
        val kdfSalt = randomBytes(PWHASH_SALT_BYTES)
        val wrapKey = deriveWrapKey(password, kdfSalt, KDF_CURRENT)
        try {
            val sealed = aeadEncrypt(privateKey, wrapKey)
            return WrappedPrivateKey(sealed.ciphertext, sealed.nonce, kdfSalt, KDF_CURRENT)
        } finally {
            wrapKey.fill(0)
        }
    }

    /** Unwraps a private key given the password and the stored salt/nonce. Throws on a wrong password or tampering. */
    /** The public key that goes with a private key. */
    fun publicKeyOf(privateKey: ByteArray): ByteArray {
        val publicKey = ByteArray(BOX_PUBLIC_KEY_BYTES)
        if (!sodium.cryptoScalarMultBase(publicKey, privateKey)) throw CryptoException("couldn't derive the public key")
        return publicKey
    }

    fun unwrapPrivateKey(wrapped: WrappedPrivateKey, password: String): ByteArray {
        val wrapKey = deriveWrapKey(password, wrapped.kdfSalt, wrapped.kdf ?: KDF_INTERACTIVE)
        try {
            return aeadDecrypt(wrapped.encryptedPrivateKey, wrapped.nonce, wrapKey)
        } finally {
            wrapKey.fill(0)
        }
    }

    /** A public fingerprint of a conversation key. One-way: it shows two people hold the same key without revealing it. */
    fun keyCommitment(conversationKey: ByteArray): ByteArray {
        if (conversationKey.size != AEAD_KEY_BYTES) throw CryptoException("A conversation key is $AEAD_KEY_BYTES bytes")
        val commitment = ByteArray(KEY_COMMITMENT_BYTES)
        val context = KEY_COMMITMENT_CONTEXT.toByteArray(Charsets.US_ASCII)
        if (sodium.cryptoKdfDeriveFromKey(commitment, commitment.size, KEY_COMMITMENT_SUBKEY_ID, context, conversationKey) != 0) {
            throw CryptoException("Key commitment failed")
        }
        return commitment
    }

    /** Generates a fresh random symmetric key for a new conversation. */
    fun generateConversationKey(): ByteArray = randomBytes(AEAD_KEY_BYTES)

    /** Seals a conversation key to one participant's public key. Call once per participant. */
    fun sealConversationKey(conversationKey: ByteArray, recipientPublicKey: ByteArray): ByteArray {
        val sealed = ByteArray(conversationKey.size + BOX_SEAL_BYTES)
        if (!sodium.cryptoBoxSeal(sealed, conversationKey, conversationKey.size.toLong(), recipientPublicKey)) {
            throw CryptoException("Failed to seal conversation key")
        }
        return sealed
    }

    /** Unseals a conversation key using the recipient's own keypair. */
    fun unsealConversationKey(sealed: ByteArray, recipientPublicKey: ByteArray, recipientPrivateKey: ByteArray): ByteArray {
        if (sealed.size < BOX_SEAL_BYTES) throw CryptoException("Sealed key too short")
        val opened = ByteArray(sealed.size - BOX_SEAL_BYTES)
        if (!sodium.cryptoBoxSealOpen(opened, sealed, sealed.size.toLong(), recipientPublicKey, recipientPrivateKey)) {
            throw CryptoException("Failed to unseal conversation key: wrong keypair or corrupted data")
        }
        return opened
    }

    /** Encrypts a message (or conversation name) body, as UTF-8, with the conversation key. */
    fun encryptWithConversationKey(plaintext: String, conversationKey: ByteArray): AeadCiphertext =
        aeadEncrypt(plaintext.toByteArray(Charsets.UTF_8), conversationKey)

    /** Decrypts a message (or conversation name) body with the conversation key. */
    fun decryptWithConversationKey(ciphertext: ByteArray, nonce: ByteArray, conversationKey: ByteArray): String =
        String(aeadDecrypt(ciphertext, nonce, conversationKey), Charsets.UTF_8)

    /**
     * Order-independent 60-digit fingerprint of both participants' real
     * public keys, as 12 groups of 5 digits. Must match the web client's
     * safetyNumberDigits exactly, since two people compare it across apps:
     * SHA-256 over "base64(lower key):base64(higher key)" (keys ordered by
     * unsigned byte comparison), first 30 bytes, each byte % 100 as 2 digits.
     */
    fun safetyNumberDigits(publicKeyA: ByteArray, publicKeyB: ByteArray): List<String> {
        val (first, second) = if (compareUnsigned(publicKeyA, publicKeyB) <= 0) publicKeyA to publicKeyB else publicKeyB to publicKeyA
        val hash = sha256("${Base64Codec.encode(first)}:${Base64Codec.encode(second)}".toByteArray(Charsets.UTF_8))
        val digits = buildString {
            for (i in 0 until 30) append(((hash[i].toInt() and 0xff) % 100).toString().padStart(2, '0'))
        }
        return (0 until 12).map { digits.substring(it * 5, it * 5 + 5) }
    }

    // --- Message signatures (db/migrations/030, docs/group-keys-and-signatures.md) ---
    //
    // Everyone in a chat holds its key, so encryption alone can't tell members
    // apart. Each account also has an Ed25519 signing key pair, derived from its
    // X25519 private key with libsodium's KDF (so every one of its apps gets the
    // same pair), and every message carries a detached signature over
    // messageSignatureInput. Pinned to the web by MessageSignatureVectorTest.

    /** The account's signing key pair; [SigningKeyPair.secretKey] is libsodium's 64-byte form. */
    fun deriveSigningKeyPair(accountPrivateKey: ByteArray): SigningKeyPair {
        val seed = ByteArray(SIGN_SEED_BYTES)
        val publicKey = ByteArray(SIGN_PUBLIC_KEY_BYTES)
        val secretKey = ByteArray(SIGN_SECRET_KEY_BYTES)
        try {
            val context = SIGNING_KDF_CONTEXT.toByteArray(Charsets.US_ASCII)
            if (sodium.cryptoKdfDeriveFromKey(seed, seed.size, SIGNING_KDF_SUBKEY_ID, context, accountPrivateKey) != 0) {
                throw CryptoException("Signing key derivation failed")
            }
            if (!sodium.cryptoSignSeedKeypair(publicKey, secretKey, seed)) throw CryptoException("Signing key derivation failed")
        } finally {
            seed.fill(0)
        }
        return SigningKeyPair(publicKey, secretKey)
    }

    fun signMessage(input: ByteArray, secretKey: ByteArray): ByteArray {
        val signature = ByteArray(SIGNATURE_BYTES)
        if (!sodium.cryptoSignDetached(signature, input, input.size.toLong(), secretKey)) throw CryptoException("Signing failed")
        return signature
    }

    fun verifyMessageSignature(signature: ByteArray, input: ByteArray, publicKey: ByteArray): Boolean {
        if (signature.size != SIGNATURE_BYTES || publicKey.size != SIGN_PUBLIC_KEY_BYTES) return false
        return try {
            sodium.cryptoSignVerifyDetached(signature, input, input.size, publicKey)
        } catch (_: Exception) {
            false
        }
    }

    fun randomBytes(size: Int): ByteArray = sodium.randomBytesBuf(size)

    internal fun aeadEncrypt(
        plaintext: ByteArray,
        key: ByteArray,
        nonce: ByteArray = randomBytes(AEAD_NONCE_BYTES),
    ): AeadCiphertext {
        val ciphertext = ByteArray(plaintext.size + AEAD_TAG_BYTES)
        val ciphertextLen = LongArray(1)
        val ok = sodium.cryptoAeadXChaCha20Poly1305IetfEncrypt(
            ciphertext, ciphertextLen, plaintext, plaintext.size.toLong(), null, 0L, null, nonce, key,
        )
        if (!ok) throw CryptoException("AEAD encryption failed")
        return AeadCiphertext(ciphertext.copyOf(ciphertextLen[0].toInt()), nonce)
    }

    internal fun aeadDecrypt(ciphertext: ByteArray, nonce: ByteArray, key: ByteArray): ByteArray {
        if (ciphertext.size < AEAD_TAG_BYTES || nonce.size != AEAD_NONCE_BYTES || key.size != AEAD_KEY_BYTES) {
            throw CryptoException("AEAD decryption failed: malformed input")
        }
        val plaintext = ByteArray(ciphertext.size - AEAD_TAG_BYTES)
        val plaintextLen = LongArray(1)
        val ok = sodium.cryptoAeadXChaCha20Poly1305IetfDecrypt(
            plaintext, plaintextLen, null, ciphertext, ciphertext.size.toLong(), null, 0L, nonce, key,
        )
        if (!ok) throw CryptoException("AEAD decryption failed: wrong key, wrong nonce, or tampered ciphertext")
        return plaintext.copyOf(plaintextLen[0].toInt())
    }

    companion object {
        const val BOX_PUBLIC_KEY_BYTES = 32
        const val BOX_SECRET_KEY_BYTES = 32
        const val BOX_SEAL_BYTES = 48 // ephemeral public key (32) + MAC (16)
        const val AEAD_KEY_BYTES = 32
        const val AEAD_NONCE_BYTES = 24
        const val AEAD_TAG_BYTES = 16
        const val PWHASH_SALT_BYTES = 16

        // libsodium's crypto_pwhash_argon2id_{OPS,MEM}LIMIT_INTERACTIVE, the
        // values the web client uses. Changing them would lock every
        // existing account out of its private key.
        const val PWHASH_OPSLIMIT_INTERACTIVE = 2L
        const val PWHASH_MEMLIMIT_INTERACTIVE = 67_108_864L

        /** The settings of every account so far, and the lowest any app accepts. */
        val KDF_INTERACTIVE = KdfSettings(PWHASH_OPSLIMIT_INTERACTIVE, PWHASH_MEMLIMIT_INTERACTIVE)

        /**
         * Used for every new wrap. Raising it takes a benchmark on real low-end phones first: Argon2 is memory-hard, and a
         * setting a phone can't finish locks that person out of their account (SECURITY_DESIGN_AND_MIGRATION.md).
         */
        val KDF_CURRENT = KDF_INTERACTIVE
        val KDF_MAX = KdfSettings(10, 1_073_741_824)
        const val SIGN_IN_DETAILS_WRONG = "This account's sign-in details look wrong. Your password wasn't sent."

        /** Settings come from the server. Too low would make a captured sign-in key cheap to attack, too high would hang the app. */
        fun requireSafeKdf(kdf: KdfSettings) {
            if (kdf.ops !in KDF_INTERACTIVE.ops..KDF_MAX.ops || kdf.mem !in KDF_INTERACTIVE.mem..KDF_MAX.mem) throw UnsafeKdfException()
        }

        const val SIGN_SEED_BYTES = 32
        const val SIGN_PUBLIC_KEY_BYTES = 32
        const val SIGN_SECRET_KEY_BYTES = 64
        const val SIGNATURE_BYTES = 64

        /** crypto_kdf context and subkey id for the signing seed. Never change: every app must derive the same key. */
        private const val SIGN_IN_KDF_CONTEXT = "rvauthk1"
        private const val SIGN_IN_KDF_SUBKEY_ID = 1L
        private const val SIGN_IN_KEY_BYTES = 32
        private const val SIGNING_KDF_CONTEXT = "rvsignk1"
        private const val SIGNING_KDF_SUBKEY_ID = 1L
        /** crypto_kdf context and subkey id for a key commitment. Never change: every app must derive the same value. */
        private const val KEY_COMMITMENT_CONTEXT = "rvkeycm1"
        private const val KEY_COMMITMENT_SUBKEY_ID = 1L
        private const val KEY_COMMITMENT_BYTES = 32
        const val KEY_NOTICE_CONTEXT = "redvault-group-key-v1"
        const val MESSAGE_SIGNATURE_CONTEXT = "redvault-message-signature-v1"

        private val UUID_PATTERN = Regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")

        /**
         * The exact bytes a message signature covers, identical on every app and
         * the server (server/src/crypto/messageSignature.ts): context, 0x00,
         * conversation id and sender id (36 lowercase ASCII chars each), key
         * epoch (u32 BE), nonce length (u32 BE), nonce, ciphertext.
         */
        fun messageSignatureInput(conversationId: String, senderId: String, keyEpoch: Long, nonce: ByteArray, ciphertext: ByteArray): ByteArray {
            val conversation = conversationId.lowercase()
            val sender = senderId.lowercase()
            if (!UUID_PATTERN.matches(conversation) || !UUID_PATTERN.matches(sender)) throw CryptoException("ids must be UUIDs")
            if (keyEpoch < 0 || keyEpoch > 0xffffffffL) throw CryptoException("bad key epoch")
            val head = "$MESSAGE_SIGNATURE_CONTEXT\u0000$conversation$sender".toByteArray(Charsets.US_ASCII)
            val out = java.nio.ByteBuffer.allocate(head.size + 8 + nonce.size + ciphertext.size)
            out.put(head)
            out.putInt(keyEpoch.toInt())
            out.putInt(nonce.size)
            out.put(nonce)
            out.put(ciphertext)
            return out.array()
        }

        /**
         * The exact bytes a group key notice signs, identical on every app (data/KeyTrust.kt): context, 0x00,
         * conversation id and issuer id (36 lowercase ASCII chars each), epoch, seq and member count (u32 BE),
         * each member id, sorted, 36 chars, then the commitment (32 bytes).
         */
        fun keyNoticeInput(conversationId: String, issuerId: String, epoch: Long, seq: Long, members: List<String>, commitment: ByteArray): ByteArray {
            val conversation = conversationId.lowercase()
            val issuer = issuerId.lowercase()
            val sorted = members.map { it.lowercase() }.sorted()
            if (!UUID_PATTERN.matches(conversation) || !UUID_PATTERN.matches(issuer) || !sorted.all(UUID_PATTERN::matches)) throw CryptoException("ids must be UUIDs")
            if (epoch !in 0..0xffffffffL || seq !in 0..0xffffffffL) throw CryptoException("bad epoch or sequence number")
            val head = "$KEY_NOTICE_CONTEXT\u0000$conversation$issuer".toByteArray(Charsets.US_ASCII)
            val ids = sorted.joinToString("").toByteArray(Charsets.US_ASCII)
            val out = java.nio.ByteBuffer.allocate(head.size + 12 + ids.size + commitment.size)
            out.put(head)
            out.putInt(epoch.toInt())
            out.putInt(seq.toInt())
            out.putInt(sorted.size)
            out.put(ids)
            out.put(commitment)
            return out.array()
        }

        fun sha256(input: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(input)

        private fun compareUnsigned(a: ByteArray, b: ByteArray): Int {
            for (i in 0 until minOf(a.size, b.size)) {
                val diff = (a[i].toInt() and 0xff) - (b[i].toInt() and 0xff)
                if (diff != 0) return diff
            }
            return a.size - b.size
        }
    }
}
