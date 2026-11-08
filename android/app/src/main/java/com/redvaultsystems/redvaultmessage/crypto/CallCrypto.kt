// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

// End-to-end protection for call setup: a port of the web's lib/callCrypto.ts,
// lib/callLinkCrypto.ts and lib/sas.ts, byte for byte, so calls work between
// this app, the web and the desktop.
//
// The server relays offers and answers but must not be able to read,
// change or forge them: the SDP they carry includes each side's DTLS
// certificate fingerprint, and WebRTC only completes the encrypted media
// connection with the peer holding that exact certificate.
//
//   - Between contacts: crypto_box (X25519 + XSalsa20-Poly1305) between the
//     sender's identity private key and the recipient's identity public key,
//     the keys the safety number fingerprints.
//   - Through a call link: XChaCha20-Poly1305 under the link's 32-byte secret
//     (which travels only in the link's URL fragment).
//
// Each payload names the call, the conversation, both people and its
// purpose, and is refused if any of those don't match or it's stale.

@Serializable
data class CallSignal(
    val v: Int = 1,
    /** "offer" or "answer"; "restart-offer" / "restart-answer" for an ICE restart with fresh TURN credentials. */
    val type: String,
    val callId: String,
    /** The 1:1 chat's id, or "link:<id>" for a call link. */
    val conversationId: String,
    val from: String,
    val to: String,
    /** "voice" or "video". */
    val kind: String,
    val sdp: String,
    /** Epoch ms when sealed. */
    val sentAt: Long,
)

class SealedSignal(val ciphertext: ByteArray, val nonce: ByteArray)

class CallSignalException(message: String) : Exception(message)

class CallCrypto(private val sodium: LazySodium, private val e2e: E2eCrypto) {
    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }

    // --- Between contacts ---

    fun seal(signal: CallSignal, recipientPublicKey: ByteArray, senderPrivateKey: ByteArray): SealedSignal {
        val message = json.encodeToString(CallSignal.serializer(), signal).toByteArray(Charsets.UTF_8)
        val nonce = e2e.randomBytes(BOX_NONCE_BYTES)
        val out = ByteArray(message.size + BOX_MAC_BYTES)
        if (!sodium.cryptoBoxEasy(out, message, message.size.toLong(), nonce, recipientPublicKey, senderPrivateKey)) {
            throw CryptoException("Couldn't seal the call setup")
        }
        return SealedSignal(out, nonce)
    }

    /** Opens a contact's sealed signal and checks it's exactly the one expected. Throws [CallSignalException] otherwise. */
    fun open(
        sealed: SealedSignal,
        senderPublicKey: ByteArray,
        recipientPrivateKey: ByteArray,
        expected: Expected,
        nowMs: Long = System.currentTimeMillis(),
    ): CallSignal {
        if (sealed.ciphertext.size < BOX_MAC_BYTES || sealed.nonce.size != BOX_NONCE_BYTES) {
            throw CallSignalException("This call couldn't be verified as coming from your contact's key.")
        }
        val plain = ByteArray(sealed.ciphertext.size - BOX_MAC_BYTES)
        if (!sodium.cryptoBoxOpenEasy(plain, sealed.ciphertext, sealed.ciphertext.size.toLong(), sealed.nonce, senderPublicKey, recipientPrivateKey)) {
            throw CallSignalException("This call couldn't be verified as coming from your contact's key.")
        }
        val signal = parse(plain.toString(Charsets.UTF_8))
        check(signal, expected, expected.conversationId, nowMs)
        return signal
    }

    // --- Call links ---

    fun sealForLink(signal: CallSignal, secret: ByteArray): SealedSignal {
        val sealed = e2e.encryptWithConversationKey(json.encodeToString(CallSignal.serializer(), signal), secret)
        return SealedSignal(sealed.ciphertext, sealed.nonce)
    }

    fun openForLink(sealed: SealedSignal, secret: ByteArray, linkId: String, expected: Expected, nowMs: Long = System.currentTimeMillis()): CallSignal {
        val text = try {
            e2e.decryptWithConversationKey(sealed.ciphertext, sealed.nonce, secret)
        } catch (e: Exception) {
            throw CallSignalException("This request couldn't be verified with the link's secret.")
        }
        val signal = parse(text)
        check(signal, expected, "link:$linkId", nowMs)
        return signal
    }

    fun newLinkSecret(): ByteArray = e2e.randomBytes(LINK_SECRET_BYTES)

    /** The owner's copy of a link's secret: sealed to their own public key, so any of their devices can show the link. */
    fun sealSecretForOwner(secret: ByteArray, ownerPublicKey: ByteArray): ByteArray = e2e.sealConversationKey(secret, ownerPublicKey)

    fun openOwnedSecret(sealed: ByteArray, publicKey: ByteArray, privateKey: ByteArray): ByteArray =
        e2e.unsealConversationKey(sealed, publicKey, privateKey)

    fun sealLinkName(name: String, secret: ByteArray): AeadCiphertext = e2e.encryptWithConversationKey(name, secret)

    fun openLinkName(ciphertext: ByteArray, nonce: ByteArray, secret: ByteArray): String = e2e.decryptWithConversationKey(ciphertext, nonce, secret)

    /** The shareable URL: the web app's address, the link id, and the secret in the fragment (never sent to any server). */
    fun linkUrl(webAppBase: String, linkId: String, secret: ByteArray): String {
        val base = if (webAppBase.endsWith("/")) webAppBase else "$webAppBase/"
        val fragment = android.util.Base64.encodeToString(secret, android.util.Base64.URL_SAFE or android.util.Base64.NO_PADDING or android.util.Base64.NO_WRAP)
        return "$base?join=${java.net.URLEncoder.encode(linkId, "UTF-8")}#$fragment"
    }

    // --- Safety words ---

    /** Four words from both DTLS fingerprints: the same on both screens only if nobody is relaying the media with their own keys. */
    fun sasWords(fingerprintA: String, fingerprintB: String, count: Int = 4): List<String> {
        val (first, second) = listOf(fingerprintA.uppercase(), fingerprintB.uppercase()).sorted()
        val message = "redvault-call-sas-v1|$first|$second".toByteArray(Charsets.UTF_8)
        val digest = ByteArray(16)
        if (!sodium.cryptoGenericHash(digest, digest.size, message, message.size.toLong(), null, 0)) throw CryptoException("Hashing failed")
        return (0 until count).map { WordList.words[digest[it].toInt() and 0xFF] }
    }

    /** A key for this phone's encrypted call history, one-way from the private key. */
    fun historyKey(privateKey: ByteArray): ByteArray {
        val label = "redvault-call-history-v1".toByteArray(Charsets.UTF_8)
        val out = ByteArray(32)
        if (!sodium.cryptoGenericHash(out, out.size, label, label.size.toLong(), privateKey, privateKey.size)) throw CryptoException("Hashing failed")
        return out
    }

    // --- Checks ---

    class Expected(val type: String, val callId: String, val conversationId: String, val from: String, val to: String, val kind: String? = null)

    private fun parse(text: String): CallSignal = try {
        json.decodeFromString(CallSignal.serializer(), text)
    } catch (e: Exception) {
        throw CallSignalException("Malformed call setup.")
    }

    private fun check(signal: CallSignal, expected: Expected, conversationId: String, nowMs: Long) {
        val matches = signal.v == 1 &&
            signal.type == expected.type &&
            signal.callId == expected.callId &&
            signal.conversationId == conversationId &&
            signal.from == expected.from &&
            signal.to == expected.to &&
            (expected.kind == null || signal.kind == expected.kind) &&
            (signal.kind == "voice" || signal.kind == "video")
        if (!matches) throw CallSignalException("Call setup didn't match this call.")
        if (kotlin.math.abs(nowMs - signal.sentAt) > MAX_SIGNAL_AGE_MS) throw CallSignalException("Call setup is too old.")
        // As on the desktop: a relay-only setup is a few kilobytes, so a huge one never rings.
        if (signal.sdp.length > MAX_SDP_CHARS) throw CallSignalException("Call setup is too large.")
        // Without a DTLS fingerprint there'd be nothing binding the media to this key.
        if (!FINGERPRINT_LINE.containsMatchIn(signal.sdp)) throw CallSignalException("Call setup is missing its encryption fingerprint.")
    }

    companion object {
        const val MAX_SIGNAL_AGE_MS = 5 * 60 * 1000L
        const val MAX_SDP_CHARS = 32 * 1024
        const val BOX_NONCE_BYTES = 24
        const val BOX_MAC_BYTES = 16
        const val LINK_SECRET_BYTES = 32
        private val FINGERPRINT_LINE = Regex("^a=fingerprint:", RegexOption.MULTILINE)
        private val FINGERPRINT = Regex("^a=fingerprint:(\\S+ [0-9A-Fa-f:]+)", RegexOption.MULTILINE)

        /** The DTLS fingerprint in an SDP ("sha-256 AB:CD:…"), or null. */
        fun sdpFingerprint(sdp: String?): String? = sdp?.let { FINGERPRINT.find(it)?.groupValues?.get(1) }
    }
}
