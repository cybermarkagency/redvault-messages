package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium

// Port of ui/src/lib/recoveryKey.ts; RecoveryKeyTest pins both to the same
// vector. The key is 32 random bytes, shown once as base32 in 13 groups of
// 4, and never leaves the device. From it, keyed BLAKE2b derives:
//   wrapKey   — encrypts a second copy of the private key (server can't open it)
//   authToken — proves the key to the server, which stores only an Argon2id hash

class RecoveryMaterial(
    val recoveryAuth: String,
    val recoveryEncryptedPrivateKey: String,
    val recoveryPrivateKeyNonce: String,
)

class RecoveryKey(private val sodium: LazySodium, private val crypto: E2eCrypto) {

    fun generate(): ByteArray = crypto.randomBytes(KEY_BYTES)

    fun wrapKey(key: ByteArray): ByteArray = keyedHash(key, "redvault-recovery-wrap-v1")

    fun authToken(key: ByteArray): ByteArray = keyedHash(key, "redvault-recovery-auth-v1")

    fun authFor(key: ByteArray): String = Base64Codec.encode(authToken(key))

    /** What the server needs so this key can restore the private key later. */
    fun materialFor(key: ByteArray, privateKey: ByteArray): RecoveryMaterial {
        val sealed = crypto.aeadEncrypt(privateKey, wrapKey(key))
        return RecoveryMaterial(authFor(key), Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce))
    }

    /**
     * Unwraps the private key and checks it really is this account's key
     * (its public key must match), so a wrong or tampered copy is never
     * adopted. Throws [CryptoException] on any mismatch.
     */
    fun unwrap(key: ByteArray, encryptedPrivateKeyB64: String, nonceB64: String, publicKeyB64: String): ByteArray {
        val privateKey = crypto.aeadDecrypt(Base64Codec.decode(encryptedPrivateKeyB64), Base64Codec.decode(nonceB64), wrapKey(key))
        val derived = ByteArray(E2eCrypto.BOX_PUBLIC_KEY_BYTES)
        if (!sodium.cryptoScalarMultBase(derived, privateKey) || Base64Codec.encode(derived) != publicKeyB64) {
            privateKey.fill(0)
            throw CryptoException("Recovered key doesn't match this account")
        }
        return privateKey
    }

    private fun keyedHash(key: ByteArray, label: String): ByteArray {
        val input = label.toByteArray(Charsets.UTF_8)
        val out = ByteArray(32)
        if (!sodium.cryptoGenericHash(out, out.size, input, input.size.toLong(), key, key.size)) {
            throw CryptoException("Key derivation failed")
        }
        return out
    }

    companion object {
        const val KEY_BYTES = 32
        private const val ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"

        /** "ABCD-EFGH-…" (52 base32 characters, 13 groups). */
        fun format(key: ByteArray): String {
            val out = StringBuilder()
            var bits = 0
            var value = 0
            for (b in key) {
                value = (value shl 8) or (b.toInt() and 0xff)
                bits += 8
                while (bits >= 5) {
                    out.append(ALPHABET[(value ushr (bits - 5)) and 31])
                    bits -= 5
                }
                value = value and ((1 shl bits) - 1)
            }
            if (bits > 0) out.append(ALPHABET[(value shl (5 - bits)) and 31])
            return out.chunked(4).joinToString("-")
        }

        /** Accepts any spacing, dashes and case, and the look-alikes 0/1/8 for O/I/B. Null if it isn't a valid key. */
        fun parse(text: String): ByteArray? {
            val clean = text.uppercase().filter { it.isLetterOrDigit() }
                .replace('0', 'O').replace('1', 'I').replace('8', 'B')
            if (clean.length != 52) return null
            val out = ByteArray(KEY_BYTES)
            var bits = 0
            var value = 0
            var index = 0
            for (ch in clean) {
                val v = ALPHABET.indexOf(ch)
                if (v < 0) return null
                value = (value shl 5) or v
                bits += 5
                if (bits >= 8) {
                    out[index++] = ((value ushr (bits - 8)) and 0xff).toByte()
                    bits -= 8
                    value = value and ((1 shl bits) - 1)
                }
            }
            // The 4 spare bits must be zero, as format() writes them.
            return if (index == KEY_BYTES && value == 0) out else null
        }
    }
}
