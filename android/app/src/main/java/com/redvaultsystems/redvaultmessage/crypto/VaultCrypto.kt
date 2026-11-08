// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium
import java.net.URLDecoder
import java.security.MessageDigest
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec
import kotlin.math.ln
import kotlin.math.max

// The password manager's cryptography and password tools, on the phone. A
// port of the desktop app's crypto/vault_crypto (desktop/core/src/
// vault_crypto.cpp), with the same key, padding and formats:
//
//   - Items are JSON padded to a 256-byte bucket, sealed with
//     XChaCha20-Poly1305 under a key derived from the account's private key
//     ("redvault-password-vault-v1"). A shared copy is sealed to the
//     recipient's public key instead (crypto_box_seal).
//   - One-time codes: RFC 6238 TOTP (SHA-1/256/512), from a base32 secret
//     or an otpauth:// URI.
//   - Breach check: SHA-1 of the password, of which only the first 5 hex
//     characters ever leave the phone (k-anonymity).
//   - Strength estimate and generator.

data class TotpParams(
    val secret: ByteArray,
    val algorithm: String = "SHA1",
    val digits: Int = 6,
    val period: Int = 30,
    val issuer: String = "",
    val account: String = "",
)

enum class Strength { Weak, Fair, Strong }

data class StrengthEstimate(val strength: Strength, val bits: Double) {
    val label: String get() = strength.name
}

data class GeneratorOptions(
    val length: Int = 24,
    val upper: Boolean = true,
    val digits: Boolean = true,
    val symbols: Boolean = true,
    /** Words instead of characters: [length] words (4-12), separated by "-", with a number. */
    val words: Boolean = false,
)

class VaultCrypto(private val sodium: LazySodium, private val e2e: E2eCrypto, private val drive: DriveCrypto) {

    // --- Items ---

    fun vaultKey(privateKey: ByteArray): ByteArray = drive.deriveLocalKey(privateKey, "redvault-password-vault-v1")

    fun sealItem(json: String, key: ByteArray): AeadCiphertext = e2e.encryptWithConversationKey(padded(json), key)

    fun openItem(ciphertext: ByteArray, nonce: ByteArray, key: ByteArray): String =
        e2e.decryptWithConversationKey(ciphertext, nonce, key).trimEnd(' ')

    /** A copy for someone else: crypto_box_seal to their public key (padded the same way). */
    fun sealForRecipient(json: String, recipientPublicKey: ByteArray): ByteArray {
        val plain = padded(json).toByteArray(Charsets.UTF_8)
        val out = ByteArray(plain.size + E2eCrypto.BOX_SEAL_BYTES)
        if (!sodium.cryptoBoxSeal(out, plain, plain.size.toLong(), recipientPublicKey)) throw CryptoException("Couldn't seal the item")
        return out
    }

    fun openFromSender(sealed: ByteArray, publicKey: ByteArray, privateKey: ByteArray): String {
        if (sealed.size < E2eCrypto.BOX_SEAL_BYTES) throw CryptoException("Sealed item is too short")
        val out = ByteArray(sealed.size - E2eCrypto.BOX_SEAL_BYTES)
        if (!sodium.cryptoBoxSealOpen(out, sealed, sealed.size.toLong(), publicKey, privateKey)) throw CryptoException("Couldn't open the shared item")
        return String(out, Charsets.UTF_8).trimEnd(' ')
    }

    private fun padded(json: String): String {
        val bytes = json.toByteArray(Charsets.UTF_8).size
        val target = ((bytes / ITEM_BUCKET) + 1) * ITEM_BUCKET
        return json + " ".repeat(target - bytes)
    }

    // --- One-time codes ---

    fun parseTotp(input: String): TotpParams? {
        val text = input.trim()
        var params: TotpParams
        if (text.startsWith("otpauth://")) {
            val typeEnd = text.indexOf('/', 10)
            if (typeEnd < 0 || text.substring(10, typeEnd) != "totp") return null
            val query = text.indexOf('?', typeEnd)
            val label = percentDecode(if (query < 0) text.substring(typeEnd + 1) else text.substring(typeEnd + 1, query))
            var issuer = ""
            val account: String
            val colon = label.indexOf(':')
            if (colon >= 0) {
                issuer = label.substring(0, colon)
                account = label.substring(colon + 1).trimStart(' ')
            } else {
                account = label
            }
            var secret = ""
            var algorithm = "SHA1"
            var digits = 6
            var period = 30
            if (query >= 0) {
                for (pair in text.substring(query + 1).split('&')) {
                    val eq = pair.indexOf('=')
                    if (eq < 0) continue
                    val value = percentDecode(pair.substring(eq + 1))
                    when (pair.substring(0, eq).lowercase()) {
                        "secret" -> secret = value
                        "issuer" -> issuer = value
                        "algorithm" -> algorithm = value.uppercase()
                        "digits" -> digits = value.toIntOrNull() ?: 0
                        "period" -> period = value.toIntOrNull() ?: 0
                    }
                }
            }
            val decoded = base32Decode(secret) ?: return null
            params = TotpParams(decoded, algorithm, digits, period, issuer, account)
        } else {
            params = TotpParams(base32Decode(text) ?: return null)
        }
        if (params.algorithm !in setOf("SHA1", "SHA256", "SHA512")) return null
        if (params.digits !in 6..8) params = params.copy(digits = 6)
        if (params.period !in 1..300) params = params.copy(period = 30)
        return params
    }

    fun totpCode(params: TotpParams, unixSeconds: Long): String {
        val counter = max(unixSeconds, 0) / params.period
        val message = ByteArray(8) { i -> (counter ushr ((7 - i) * 8)).toByte() }
        val algorithm = when (params.algorithm) {
            "SHA256" -> "HmacSHA256"
            "SHA512" -> "HmacSHA512"
            else -> "HmacSHA1"
        }
        val mac = Mac.getInstance(algorithm).apply { init(SecretKeySpec(params.secret, algorithm)) }.doFinal(message)
        val offset = mac.last().toInt() and 0x0F
        val binary = ((mac[offset].toInt() and 0x7F) shl 24) or ((mac[offset + 1].toInt() and 0xFF) shl 16) or
            ((mac[offset + 2].toInt() and 0xFF) shl 8) or (mac[offset + 3].toInt() and 0xFF)
        var modulo = 1L
        repeat(params.digits) { modulo *= 10 }
        return (binary.toLong() % modulo).toString().padStart(params.digits, '0')
    }

    fun totpSecondsLeft(params: TotpParams, unixSeconds: Long): Int = params.period - (unixSeconds % params.period).toInt()

    // --- Breach check ---

    /** Uppercase hex SHA-1 of the password: the first 5 characters are sent, the rest compared on the phone. */
    fun breachHash(password: String): String =
        MessageDigest.getInstance("SHA-1").digest(password.toByteArray(Charsets.UTF_8)).joinToString("") { "%02X".format(it.toInt() and 0xff) }

    /** How many times [hashHex] appears in a range response ("SUFFIX:COUNT" lines). 0: not breached. */
    fun breachCount(hashHex: String, rangeResponse: String): Long {
        if (hashHex.length != 40) return 0
        val suffix = hashHex.substring(5)
        for (raw in rangeResponse.split('\n')) {
            val line = raw.trimEnd('\r')
            val colon = line.indexOf(':')
            // Padding entries have a count of 0: not a real breach.
            if (colon == 35 && line.substring(0, 35) == suffix) return line.substring(colon + 1).trim().toLongOrNull() ?: 0
        }
        return 0
    }

    // --- Strength ---

    fun estimateStrength(password: String): StrengthEstimate {
        fun finish(bits: Double) = StrengthEstimate(
            when {
                bits >= 75 -> Strength.Strong
                bits >= 50 -> Strength.Fair
                else -> Strength.Weak
            },
            bits,
        )
        if (password.isEmpty()) return finish(0.0)
        val lower = password.lowercase()
        if (lower in COMMON) return finish(8.0)
        // Byte-wise like the desktop app, so both rate a password the same.
        val bytes = password.toByteArray(Charsets.UTF_8).map { it.toInt() and 0xff }
        var hasLower = false
        var hasUpper = false
        var hasDigit = false
        var hasSymbol = false
        var hasOther = false
        for (c in bytes) {
            when {
                c in 'a'.code..'z'.code -> hasLower = true
                c in 'A'.code..'Z'.code -> hasUpper = true
                c in '0'.code..'9'.code -> hasDigit = true
                c < 128 -> hasSymbol = true
                else -> hasOther = true
            }
        }
        val pool = (if (hasLower) 26 else 0) + (if (hasUpper) 26 else 0) + (if (hasDigit) 10 else 0) + (if (hasSymbol) 33 else 0) + (if (hasOther) 64 else 0)
        var effective = 1.0
        for (i in 1 until bytes.size) {
            val a = bytes[i - 1]
            val b = bytes[i]
            effective += when {
                a == b -> 0.2
                b == a + 1 || b + 1 == a -> 0.35
                else -> 1.0
            }
        }
        val log2Pool = ln(max(pool, 2).toDouble()) / ln(2.0)
        var bits = effective * log2Pool
        // A common password with a few characters added is still mostly guessable.
        for (common in COMMON_ORDERED) {
            if (common.length >= 5 && lower.contains(common)) {
                bits -= common.length * log2Pool * 0.8
                break
            }
        }
        return finish(max(bits, 0.0))
    }

    // --- Generator ---

    fun generatePassword(options: GeneratorOptions): String {
        if (options.words) {
            val count = options.length.coerceIn(4, 12)
            val words = (0 until count).map { i ->
                val word = WordList.words[uniform(256)]
                if (options.upper && i == 0) word.replaceFirstChar { it.uppercaseChar() } else word
            }
            return words.joinToString("-") + if (options.digits) "-${uniform(100)}" else ""
        }
        val lowerSet = "abcdefghijkmnopqrstuvwxyz" // no l: it reads like 1 and I
        val upperSet = "ABCDEFGHJKLMNPQRSTUVWXYZ" // no I or O
        val digitSet = "23456789" // no 0 or 1
        val symbolSet = "!@#$%^&*-_=+?"
        val sets = buildList {
            add(lowerSet)
            if (options.upper) add(upperSet)
            if (options.digits) add(digitSet)
            if (options.symbols) add(symbolSet)
        }
        val all = sets.joinToString("")
        val length = options.length.coerceIn(8, 128)
        // Redraw until every chosen class appears (rarely needs a second try).
        while (true) {
            val out = buildString { repeat(length) { append(all[uniform(all.length)]) } }
            if (sets.all { set -> out.any { it in set } }) return out
        }
    }

    private fun uniform(upperBound: Int): Int = sodium.randomBytesUniform(upperBound).toInt()

    companion object {
        private const val ITEM_BUCKET = 256

        /** The passwords people pick most: an instant "Weak" whatever their length. The desktop app's list, in its (sorted) order. */
        private val COMMON_ORDERED = listOf(
            "123456", "123456789", "12345678", "password", "qwerty", "qwerty123", "1q2w3e4r", "12345", "111111", "123123",
            "1234567890", "1234567", "abc123", "password1", "password123", "iloveyou", "admin", "welcome", "monkey", "dragon",
            "letmein", "football", "baseball", "sunshine", "princess", "000000", "654321", "superman", "qazwsx", "trustno1",
            "master", "hello", "freedom", "whatever", "shadow", "michael", "charlie", "passw0rd", "p@ssw0rd", "zaq12wsx",
            "asdfghjkl", "qwertyuiop", "starwars", "login", "welcome1", "secret", "changeme", "default", "root", "test",
        ).sorted()
        private val COMMON = COMMON_ORDERED.toSet()

        fun base32Decode(text: String): ByteArray? {
            val out = ArrayList<Byte>()
            var buffer = 0L
            var bits = 0
            var any = false
            for (raw in text) {
                if (raw == ' ' || raw == '-' || raw == '=') continue
                val c = raw.uppercaseChar()
                val value = when (c) {
                    in 'A'..'Z' -> c - 'A'
                    in '2'..'7' -> c - '2' + 26
                    else -> return null
                }
                any = true
                buffer = ((buffer shl 5) or value.toLong()) and 0xFFFFFFFFL
                bits += 5
                if (bits >= 8) {
                    out.add((buffer ushr (bits - 8)).toByte())
                    bits -= 8
                }
            }
            if (!any || out.isEmpty()) return null
            return out.toByteArray()
        }

        private fun percentDecode(text: String): String = runCatching { URLDecoder.decode(text, "UTF-8") }.getOrDefault(text)
    }
}
