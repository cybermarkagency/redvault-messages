package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.CallCrypto
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import kotlinx.serialization.Serializable
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json

/** How a call ended, from this side. */
enum class CallOutcome { answered, missed, declined, unanswered, failed }

/** One call in this phone's history (same fields as the web's lib/callHistory.ts). */
@Serializable
data class CallRecord(
    val id: String,
    /** The contact's user id (or a link guest's). */
    val peerUserId: String,
    val peerHandle: String,
    val conversationId: String? = null,
    /** Set for calls through one of your call links (or a link you joined). */
    val linkName: String? = null,
    /** "voice" or "video". */
    val kind: String,
    /** "outgoing" or "incoming". */
    val direction: String,
    val outcome: CallOutcome,
    /** Epoch ms the call started ringing. */
    val startedAt: Long,
    /** Seconds connected; 0 when it never connected. */
    val durationSec: Int,
)

/**
 * Call history, kept only on this phone and encrypted there. The server
 * never records calls, so this is the only record of them anywhere: who,
 * when, how long. It's sealed with a key derived one way from the account's
 * private key, so it's unreadable without signing in.
 */
class CallHistoryStore(context: Context, private val e2e: E2eCrypto, private val calls: CallCrypto) {
    private val prefs = context.getSharedPreferences("callHistory", Context.MODE_PRIVATE)
    private val json = Json { ignoreUnknownKeys = true }
    private val serializer = ListSerializer(CallRecord.serializer())

    @Serializable
    private data class Sealed(val c: String, val n: String)

    fun load(userId: String, privateKey: ByteArray): List<CallRecord> = try {
        val raw = prefs.getString(userId, null)
        if (raw == null) emptyList() else {
            val sealed = json.decodeFromString(Sealed.serializer(), raw)
            val text = e2e.decryptWithConversationKey(Base64Codec.decode(sealed.c), Base64Codec.decode(sealed.n), calls.historyKey(privateKey))
            json.decodeFromString(serializer, text)
        }
    } catch (e: Exception) {
        emptyList()
    }

    /** Adds or replaces [record]; newest first, at most [MAX_RECORDS]. */
    fun record(userId: String, privateKey: ByteArray, record: CallRecord): List<CallRecord> {
        val next = (listOf(record) + load(userId, privateKey).filter { it.id != record.id }).sortedByDescending { it.startedAt }.take(MAX_RECORDS)
        save(userId, privateKey, next)
        return next
    }

    fun clear(userId: String, privateKey: ByteArray) = save(userId, privateKey, emptyList())

    private fun save(userId: String, privateKey: ByteArray, records: List<CallRecord>) {
        runCatching {
            val sealed = e2e.encryptWithConversationKey(json.encodeToString(serializer, records), calls.historyKey(privateKey))
            prefs.edit().putString(userId, json.encodeToString(Sealed.serializer(), Sealed(Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce)))).apply()
        }
    }

    companion object {
        const val MAX_RECORDS = 500

        fun formatDuration(seconds: Int): String = when {
            seconds < 60 -> "$seconds s"
            seconds < 3600 -> "${Math.round(seconds / 60.0)} min"
            else -> "${seconds / 3600} h ${(seconds % 3600) / 60} min"
        }
    }
}
