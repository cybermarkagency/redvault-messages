package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

/**
 * The application envelope of protocol 3 chat (docs/multi-device-threat-model.md 9.3) and what a chat shows from its
 * stored records. A port of encodeContent, decodeEnvelope, foldChat and eraseDeleted in ui/src/lib/e2eV3/chat.ts: every
 * client must give the same result (desktop/tests/vectors/v3_chat_content_vectors.json). No Android classes here.
 */

/** What a reaction, edit or delete does to the message [target]. An empty [emoji] removes the person's reaction. */
@Serializable
data class ChatOp(val t: String, val target: String, val body: String? = null, val emoji: String? = null) {
    companion object {
        const val REACTION = "reaction"
        const val EDIT = "edit"
        const val DELETE = "delete"

        fun reaction(target: String, emoji: String) = ChatOp(REACTION, target, emoji = emoji)
        fun edit(target: String, body: String) = ChatOp(EDIT, target, body = body)
        fun delete(target: String) = ChatOp(DELETE, target)
    }
}

private val UUID_RE = Regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
private const val MAX_EMOJI_BYTES = 32
/** Envelopes fill 256-byte buckets, so their size shows less about what they hold. */
private const val PAD_BUCKET = 256

private fun envelope(conversationId: String, t: String, id: String, ts: Long, fields: kotlinx.serialization.json.JsonObjectBuilder.() -> Unit): String =
    buildJsonObject {
        put("v", 1)
        put("t", t)
        put("id", id)
        put("c", conversationId)
        put("ts", ts)
        fields()
    }.toString().let(::padded)

/** Spaces after the JSON, which every parser skips. */
private fun padded(json: String): String {
    val bytes = json.toByteArray(Charsets.UTF_8).size
    return json + " ".repeat((bytes / PAD_BUCKET + 1) * PAD_BUCKET - bytes)
}

fun encodeText(conversationId: String, text: String, id: String, ts: Long): String =
    envelope(conversationId, "text", id, ts) { put("body", text) }

/** The envelope for a reaction, edit or delete. */
fun encodeOp(conversationId: String, op: ChatOp, id: String, ts: Long): String =
    envelope(conversationId, op.t, id, ts) {
        put("target", op.target)
        op.body?.let { put("body", it) }
        op.emoji?.let { put("emoji", it) }
    }

/** The envelope for a file: the only place its key ever travels. */
fun encodeFile(conversationId: String, file: FileDescriptor, id: String, ts: Long): String =
    envelope(conversationId, "file", id, ts) { put("file", Json.encodeToJsonElement(FileDescriptor.serializer(), file)) }

sealed interface DecodedEnvelope {
    /** A text or file message; a file has empty [text]. */
    data class Shown(val id: String, val ts: Long, val text: String, val file: FileDescriptor? = null) : DecodedEnvelope

    data class Op(val id: String, val ts: Long, val op: ChatOp) : DecodedEnvelope

    /** A newer envelope version: keep the item and ask for an update. */
    data object Newer : DecodedEnvelope

    /** An unknown type, or a message bound to another conversation: nothing to show; the item is acknowledged. [reason] is a code, never content. */
    data class Ignore(val reason: String) : DecodedEnvelope
}

private fun JsonElement?.string(): String? = (this as? JsonPrimitive)?.takeIf { it.isString }?.content

private fun JsonElement?.number(): JsonPrimitive? = (this as? JsonPrimitive)?.takeIf { !it.isString && it.doubleOrNull?.isFinite() == true }

private fun String?.fitting(maxBytes: Int): String? = this?.takeIf { it.toByteArray().size <= maxBytes }

/** Validates an envelope against the mailbox item that carried it (9.3). [conversationId] is the item's, lowercase. */
fun decodeEnvelope(plaintext: String, conversationId: String?, now: Long): DecodedEnvelope {
    val parsed = runCatching { Json.parseToJsonElement(plaintext) }.getOrNull() ?: return DecodedEnvelope.Ignore("not_json")
    val e = parsed as? JsonObject ?: return DecodedEnvelope.Ignore("not_an_envelope")
    val version = e["v"].number()?.doubleOrNull
    if (version != 1.0) return if (version != null && version > 1) DecodedEnvelope.Newer else DecodedEnvelope.Ignore("bad_version")
    val t = e["t"].string()
    if (t != "text" && t != "file" && t != ChatOp.REACTION && t != ChatOp.EDIT && t != ChatOp.DELETE) return DecodedEnvelope.Ignore("unknown_type")
    val id = e["id"].string()?.takeIf(UUID_RE::matches) ?: return DecodedEnvelope.Ignore("bad_id")
    if (conversationId == null || e["c"].string() != conversationId) return DecodedEnvelope.Ignore("wrong_conversation")
    val claimed = e["ts"].number()?.let { it.longOrNull ?: it.doubleOrNull?.toLong() } ?: now
    val ts = claimed.coerceIn(now - ChatLimits.PAST_MS, now + ChatLimits.FUTURE_MS)
    if (t == "file") {
        val descriptor = e["file"] as? JsonObject ?: return DecodedEnvelope.Ignore("bad_file")
        val file = FileMessage.parse(FileMessage.PREFIX + descriptor.toString()) ?: return DecodedEnvelope.Ignore("bad_file")
        return DecodedEnvelope.Shown(id, ts, "", file)
    }
    if (t == "text") {
        val body = e["body"].string().fitting(ChatLimits.MAX_TEXT_BYTES) ?: return DecodedEnvelope.Ignore("bad_body")
        return DecodedEnvelope.Shown(id, ts, body)
    }
    val target = e["target"].string()?.takeIf(UUID_RE::matches) ?: return DecodedEnvelope.Ignore("bad_target")
    return when (t) {
        ChatOp.DELETE -> DecodedEnvelope.Op(id, ts, ChatOp.delete(target))
        ChatOp.EDIT -> {
            val body = e["body"].string().fitting(ChatLimits.MAX_TEXT_BYTES)?.takeIf { it.isNotEmpty() } ?: return DecodedEnvelope.Ignore("bad_body")
            DecodedEnvelope.Op(id, ts, ChatOp.edit(target, body))
        }
        else -> {
            val emoji = e["emoji"].string().fitting(MAX_EMOJI_BYTES) ?: return DecodedEnvelope.Ignore("bad_emoji")
            DecodedEnvelope.Op(id, ts, ChatOp.reaction(target, emoji))
        }
    }
}

data class ShownReaction(val userId: String, val emoji: String)

/** A message as the chat shows it. [record] has the latest text; a deleted message's is empty, with no file. */
data class ShownMessage(
    val record: ChatRecord,
    /** When the text shown was written; null when it was never edited. */
    val editedAt: Long? = null,
    val deleted: Boolean = false,
    val reactions: List<ShownReaction> = emptyList(),
)

/** Later wins: by the sender's clock, then by id, so every device settles on the same answer whatever order things arrived in. */
private fun newer(a: ChatRecord, b: ChatRecord): Boolean = a.ts > b.ts || (a.ts == b.ts && a.id > b.id)

/** The messages their own sender deleted, among [records]. */
private fun deletedIds(records: List<ChatRecord>, messages: Map<String, ChatRecord>): Set<String> =
    records.mapNotNullTo(HashSet()) { r ->
        r.op?.takeIf { it.t == ChatOp.DELETE && messages[it.target]?.fromUserId == r.fromUserId }?.target
    }

/**
 * Turns stored records into what a chat shows. Only the person who sent a message can edit or delete it. An op whose
 * message hasn't arrived yet waits in the history, so arrival order never changes the result.
 */
fun foldChat(records: List<ChatRecord>): List<ShownMessage> {
    val messages = records.filter { it.op == null }.associateBy { it.id }
    val deleted = deletedIds(records, messages)
    val edits = HashMap<String, ChatRecord>()
    val reactions = HashMap<Pair<String, String>, ChatRecord>()
    for (record in records) {
        val op = record.op ?: continue
        val message = messages[op.target] ?: continue
        if (op.target in deleted) continue
        if (op.t == ChatOp.EDIT && record.fromUserId == message.fromUserId && message.file == null) {
            val held = edits[op.target]
            if (held == null || newer(record, held)) edits[op.target] = record
        } else if (op.t == ChatOp.REACTION) {
            val key = op.target to record.fromUserId
            val held = reactions[key]
            if (held == null || newer(record, held)) reactions[key] = record
        }
    }
    val byMessage = reactions.values.filter { !it.op?.emoji.isNullOrEmpty() }.groupBy { it.op!!.target }
    return messages.values
        .map { message ->
            if (message.id in deleted) {
                ShownMessage(message.copy(text = "", file = null), deleted = true)
            } else {
                val edit = edits[message.id]
                ShownMessage(
                    record = if (edit != null) message.copy(text = edit.op?.body.orEmpty()) else message,
                    editedAt = edit?.ts,
                    reactions = byMessage[message.id].orEmpty().map { ShownReaction(it.fromUserId, it.op?.emoji.orEmpty()) }.sortedBy { it.userId },
                )
            }
        }
        .sortedWith(compareBy({ it.record.ts }, { it.record.arrivedAt }, { it.record.id }))
}

/** What [ChatHistory.eraseDeleted] does: records to [remove], and messages to [blank] (their text and file key). */
class ErasePlan(val remove: Set<String>, val blank: Set<String>) {
    val isEmpty: Boolean get() = remove.isEmpty() && blank.isEmpty()
}

/** Everything that still repeats a message its sender deleted. The delete itself stays: it's what keeps showing the message as deleted. */
fun planErase(records: List<ChatRecord>): ErasePlan {
    val messages = records.filter { it.op == null }.associateBy { it.id }
    val deleted = deletedIds(records, messages)
    return ErasePlan(
        remove = records.filter { r -> r.op?.let { it.t != ChatOp.DELETE && it.target in deleted } == true }.mapTo(HashSet()) { it.id },
        blank = deleted.filterTo(HashSet()) { id -> messages.getValue(id).let { it.text.isNotEmpty() || it.file != null } },
    )
}
