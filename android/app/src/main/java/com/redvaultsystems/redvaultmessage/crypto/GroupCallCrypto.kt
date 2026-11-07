package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

// End-to-end encryption for group calls: a port of the web's
// lib/groupCallCrypto.ts and groupSasWords in lib/sas.ts, byte for byte, so a
// room key sealed by the web opens here and the other way round.
//
// Group calls go through a LiveKit SFU, which forwards each person's media to
// the others. With DTLS alone the SFU would see and hear everything, so every
// audio/video frame is also encrypted in the app (LiveKit's E2EE, AES-GCM)
// under a room key the server never holds:
//
//   - The first person in the call (the "keyholder") makes a random 32-byte
//     key for each epoch and seals it with crypto_box to every other
//     participant's identity key, the same key their safety number covers.
//     The server relays the sealed copies but can't open or forge them.
//   - The server bumps the epoch whenever someone joins or leaves, and the
//     keyholder sends a fresh key. Someone who left can't decrypt what comes
//     after; someone who joined can't decrypt what came before.
//   - The sealed payload names the call, group, epoch, sender and recipient,
//     and is refused if any don't match or it's stale, so it can't be
//     replayed into another call or epoch.

/** What a sealed room key says about itself; the receiver checks every field. */
@Serializable
internal data class RoomKeyMessage(
    val v: Int,
    val type: String,
    val callId: String,
    val conversationId: String,
    val epoch: Long,
    val from: String,
    val to: String,
    /** The 32-byte key, standard base64. */
    val key: String,
    /** Epoch ms when sealed. */
    val sentAt: Long,
)

/** What a received room key has to say it is. */
class ExpectedRoomKey(val callId: String, val conversationId: String, val epoch: Long, val from: String, val to: String)

/** A room key that couldn't be used; the message is safe to show. */
class GroupKeyException(message: String) : Exception(message)

class GroupCallCrypto(private val sodium: LazySodium, private val e2e: E2eCrypto) {
    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }

    fun newRoomKey(): ByteArray = e2e.randomBytes(ROOM_KEY_BYTES)

    fun sealRoomKey(
        message: ExpectedRoomKey,
        key: ByteArray,
        recipientPublicKey: ByteArray,
        senderPrivateKey: ByteArray,
        nowMs: Long = System.currentTimeMillis(),
    ): SealedSignal {
        val payload = RoomKeyMessage(
            v = 1,
            type = TYPE,
            callId = message.callId,
            conversationId = message.conversationId,
            epoch = message.epoch,
            from = message.from,
            to = message.to,
            key = Base64Codec.encode(key),
            sentAt = nowMs,
        )
        val plain = json.encodeToString(RoomKeyMessage.serializer(), payload).toByteArray(Charsets.UTF_8)
        val nonce = e2e.randomBytes(BOX_NONCE_BYTES)
        val out = ByteArray(plain.size + BOX_MAC_BYTES)
        if (!sodium.cryptoBoxEasy(out, plain, plain.size.toLong(), nonce, recipientPublicKey, senderPrivateKey)) {
            throw GroupKeyException("Couldn't seal the call key.")
        }
        return SealedSignal(out, nonce)
    }

    /** Opens a room key from [senderPublicKey] and checks it's exactly the one [expected]. Throws [GroupKeyException] otherwise. */
    fun openRoomKey(
        sealed: SealedSignal,
        senderPublicKey: ByteArray,
        recipientPrivateKey: ByteArray,
        expected: ExpectedRoomKey,
        nowMs: Long = System.currentTimeMillis(),
    ): ByteArray {
        if (sealed.ciphertext.size < BOX_MAC_BYTES || sealed.nonce.size != BOX_NONCE_BYTES) throw unverifiable()
        val plain = ByteArray(sealed.ciphertext.size - BOX_MAC_BYTES)
        if (!sodium.cryptoBoxOpenEasy(plain, sealed.ciphertext, sealed.ciphertext.size.toLong(), sealed.nonce, senderPublicKey, recipientPrivateKey)) {
            throw unverifiable()
        }
        return checkRoomKey(plain.toString(Charsets.UTF_8), expected, nowMs)
    }

    /**
     * Safety words for a group call. There's no end-to-end DTLS connection to
     * pin in a call through an SFU; what the media is pinned to instead is the
     * identity key each participant's room key was sealed to. So the words come
     * from the call and every participant's identity key as this app knows it:
     * if the server had swapped anyone's key (to receive the room key itself),
     * that person's screen would show different words from everyone else's.
     */
    fun groupSasWords(callId: String, participants: List<Pair<String, ByteArray>>, count: Int = 4): List<String> {
        val entries = participants.map { (userId, publicKey) -> "$userId:${hex(publicKey)}" }.sorted()
        val message = "redvault-group-call-sas-v1|$callId|${entries.joinToString("|")}".toByteArray(Charsets.UTF_8)
        val digest = ByteArray(16)
        if (!sodium.cryptoGenericHash(digest, digest.size, message, message.size.toLong(), null, 0)) throw CryptoException("Hashing failed")
        return (0 until count).map { WordList.words[digest[it].toInt() and 0xFF] }
    }

    private fun unverifiable() = GroupKeyException("The call key couldn't be verified as coming from the keyholder's identity key.")

    private fun hex(bytes: ByteArray): String = buildString(bytes.size * 2) {
        for (b in bytes) {
            append(HEX[(b.toInt() shr 4) and 0xF])
            append(HEX[b.toInt() and 0xF])
        }
    }

    companion object {
        private val checkJson = Json { ignoreUnknownKeys = true }

        /**
         * What's left of a sealed room key once it's been opened: parses [payload] and checks it's exactly
         * the one [expected] (call, group, epoch, sender, recipient), not stale, and 32 bytes. These checks
         * don't need libsodium. Throws [GroupKeyException] otherwise, with the web's wording.
         */
        internal fun checkRoomKey(payload: String, expected: ExpectedRoomKey, nowMs: Long): ByteArray {
            val message = try {
                checkJson.decodeFromString(RoomKeyMessage.serializer(), payload)
            } catch (e: Exception) {
                throw GroupKeyException("The call key couldn't be verified as coming from the keyholder's identity key.")
            }
            val matches = message.v == 1 &&
                message.type == TYPE &&
                message.callId == expected.callId &&
                message.conversationId == expected.conversationId &&
                message.epoch == expected.epoch &&
                message.from == expected.from &&
                message.to == expected.to
            if (!matches) throw GroupKeyException("The call key didn't match this call.")
            if (kotlin.math.abs(nowMs - message.sentAt) > MAX_KEY_AGE_MS) throw GroupKeyException("The call key is too old.")
            val key = try {
                Base64Codec.decode(message.key)
            } catch (e: IllegalArgumentException) {
                throw GroupKeyException("The call key has the wrong length.")
            }
            if (key.size != ROOM_KEY_BYTES) throw GroupKeyException("The call key has the wrong length.")
            return key
        }

        const val ROOM_KEY_BYTES = 32
        const val MAX_KEY_AGE_MS = 5 * 60 * 1000L
        const val BOX_NONCE_BYTES = 24
        const val BOX_MAC_BYTES = 16
        const val TYPE = "group-call-key"

        /** LiveKit's keyring size: an epoch's key goes in slot (epoch mod this), so frames in flight under the previous key still decrypt. */
        const val KEYRING_SIZE = 16

        private const val HEX = "0123456789abcdef"

        fun keySlot(epoch: Long): Int = (epoch % KEYRING_SIZE).toInt()
    }
}
