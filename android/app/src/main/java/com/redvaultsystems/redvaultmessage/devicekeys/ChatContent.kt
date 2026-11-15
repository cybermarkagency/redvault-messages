// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

/**
 * The application envelope of protocol 3 chat (docs/multi-device-threat-model.md 9.3) and what a chat shows from its
 * stored records. A port of encodeContent, decodeEnvelope, foldChat and eraseDeleted in ui/src/lib/e2eV3/chat.ts: every
 * client must give the same result (desktop/tests/vectors/v3_chat_content_vectors.json, and for part A's replies, forwards,
 * polls, pins, receipts and disappearing messages v3_chat_content_part_a_vectors.json). No Android classes here.
 */

/**
 * What an op does to the message [target]: a reaction (an empty [emoji] removes the person's reaction), an edit or a
 * delete (9.3); a vote ([choices], empty takes it back), the end of a poll, a pin or unpin ([pinned]), or a receipt
 * for [targets] ([status] "delivered" or "read"; [target] is then the first of them). Part A:
 * docs/signal-integration/08-part-a-chat-features.md.
 */
@Serializable
data class ChatOp(
    val t: String,
    val target: String,
    val body: String? = null,
    val emoji: String? = null,
    val choices: List<Int>? = null,
    val pinned: Boolean? = null,
    val targets: List<String>? = null,
    val status: String? = null,
) {
    /** Every message this op is about. */
    val about: List<String> get() = targets ?: listOf(target)

    companion object {
        const val REACTION = "reaction"
        const val EDIT = "edit"
        const val DELETE = "delete"
        const val VOTE = "vote"
        const val END = "end"
        const val PIN = "pin"
        const val RECEIPT = "receipt"
        const val DELIVERED = "delivered"
        const val READ = "read"

        fun reaction(target: String, emoji: String) = ChatOp(REACTION, target, emoji = emoji)
        fun edit(target: String, body: String) = ChatOp(EDIT, target, body = body)
        fun delete(target: String) = ChatOp(DELETE, target)
        fun vote(target: String, choices: List<Int>) = ChatOp(VOTE, target, choices = choices)
        fun end(target: String) = ChatOp(END, target)
        fun pin(target: String, pinned: Boolean) = ChatOp(PIN, target, pinned = pinned)
        fun receipt(targets: List<String>, status: String): ChatOp {
            require(targets.isNotEmpty() && targets.size <= ChatLimits.MAX_RECEIPT_TARGETS) { "A receipt names 1 to 100 messages." }
            return ChatOp(RECEIPT, targets.first(), targets = targets, status = status)
        }
    }
}

/** What a reply answers: [target]'s id, and a few words of it for a device that never had it. */
@Serializable
data class ChatReply(val target: String, val preview: String)

/** A poll's options; its question is the record's text, and its id the message id. */
@Serializable
data class ChatPoll(val options: List<String>, val multi: Boolean)

/**
 * Part D: the story a text answers or reacts to (docs/signal-integration/12-part-d-stories.md), the same field as on
 * protocol 1: its author, its id, its kind, a short label ([preview], at most 80 characters) and whether the text is a
 * quick reaction. It never carries the story's key.
 */
@Serializable
data class ChatStoryReply(val authorId: String, val storyId: String, val kind: String, val preview: String, val reaction: Boolean)

/** The optional part A fields of a text or file message: a reply, the forwarded mark, the disappearing time in seconds; and part D's story reply (text only). */
data class MessageExtras(val reply: ChatReply? = null, val forwarded: Boolean = false, val exp: Long? = null, val storyReply: ChatStoryReply? = null)

/** What a story reply's label may hold. */
const val MAX_STORY_REPLY_PREVIEW_CHARS = 80
private val STORY_KINDS = setOf("text", "image", "video", "audio")

/** A story reply every client accepts, its label cut to fit; null when the ids aren't ids or the kind is unknown. */
fun chatStoryReply(authorId: String, storyId: String, kind: String, preview: String, reaction: Boolean): ChatStoryReply? {
    val author = authorId.lowercase().takeIf(UUID_RE::matches) ?: return null
    val story = storyId.lowercase().takeIf(UUID_RE::matches) ?: return null
    if (kind !in STORY_KINDS) return null
    val cut = if (preview.length <= MAX_STORY_REPLY_PREVIEW_CHARS) preview else {
        val end = if (Character.isHighSurrogate(preview[MAX_STORY_REPLY_PREVIEW_CHARS - 1])) MAX_STORY_REPLY_PREVIEW_CHARS - 1 else MAX_STORY_REPLY_PREVIEW_CHARS
        preview.substring(0, end)
    }
    return ChatStoryReply(author, story, kind, cut, reaction)
}

private val UUID_RE = Regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
private const val MAX_EMOJI_BYTES = 32
private const val MAX_PREVIEW_CHARS = 200
private const val MAX_QUESTION_CHARS = 300
private const val MAX_OPTION_CHARS = 100
private const val MAX_OPTIONS = 10
private const val MAX_EXP_SECONDS = 2_419_200L
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

private fun kotlinx.serialization.json.JsonObjectBuilder.extras(extras: MessageExtras) {
    extras.reply?.let { r -> put("reply", buildJsonObject { put("target", r.target); put("preview", r.preview) }) }
    if (extras.forwarded) put("fwd", true)
    extras.exp?.let { put("exp", it) }
    extras.storyReply?.let { r ->
        put("storyReply", buildJsonObject {
            put("authorId", r.authorId)
            put("storyId", r.storyId)
            put("kind", r.kind)
            put("preview", r.preview)
            put("reaction", r.reaction)
        })
    }
}

fun encodeText(conversationId: String, text: String, id: String, ts: Long, extras: MessageExtras = MessageExtras()): String =
    envelope(conversationId, "text", id, ts) {
        put("body", text)
        extras(extras)
    }

/** The envelope for a reaction, edit, delete, vote, end, pin or receipt. */
fun encodeOp(conversationId: String, op: ChatOp, id: String, ts: Long): String =
    envelope(conversationId, op.t, id, ts) {
        if (op.t == ChatOp.RECEIPT) {
            put("targets", JsonArray(op.about.map(::JsonPrimitive)))
            put("status", op.status)
            return@envelope
        }
        put("target", op.target)
        op.body?.let { put("body", it) }
        op.emoji?.let { put("emoji", it) }
        op.choices?.let { c -> put("choices", JsonArray(c.map(::JsonPrimitive))) }
        op.pinned?.let { put("pinned", it) }
    }

/** The envelope for a file: the only place its key ever travels. */
fun encodeFile(conversationId: String, file: FileDescriptor, id: String, ts: Long, extras: MessageExtras = MessageExtras()): String =
    envelope(conversationId, "file", id, ts) {
        put("file", Json.encodeToJsonElement(FileDescriptor.serializer(), file))
        // A story reply is a text (part D).
        extras(extras.copy(storyReply = null))
    }

/** The envelope for a poll; its id is the poll's. [exp] as on a text. */
fun encodePoll(conversationId: String, question: String, poll: ChatPoll, id: String, ts: Long, exp: Long? = null): String =
    envelope(conversationId, "poll", id, ts) {
        put("question", question)
        put("options", JsonArray(poll.options.map(::JsonPrimitive)))
        put("multi", poll.multi)
        exp?.let { put("exp", it) }
    }

/**
 * A call's setup over device keys (part B, docs/signal-integration/09-part-b-calls.md): one step of a one-to-one
 * call. [to] is the device it's meant for, or "*" for every device of the recipient. Ephemeral: never stored.
 */
class CallMessage(val callId: String, val step: String, val kind: String, val sdp: String, val to: String) {
    override fun equals(other: Any?): Boolean =
        other is CallMessage && callId == other.callId && step == other.step && kind == other.kind && sdp == other.sdp && to == other.to

    override fun hashCode(): Int = callId.hashCode() * 31 + step.hashCode()

    /** Never the SDP: it would end up in a log. */
    override fun toString(): String = "CallMessage(callId=$callId, step=$step, kind=$kind, to=$to)"

    companion object {
        const val OFFER = "offer"
        const val ANSWER = "answer"
        const val RESTART_OFFER = "restart-offer"
        const val RESTART_ANSWER = "restart-answer"
        val STEPS = setOf(OFFER, ANSWER, RESTART_OFFER, RESTART_ANSWER)
        const val EVERY_DEVICE = "*"
    }
}

/** A group call's room key for [epoch] (part B), sent pairwise to each participant's devices. Ephemeral: never stored. */
class CallKeyMessage(val callId: String, val epoch: Long, val key: ByteArray) {
    override fun equals(other: Any?): Boolean =
        other is CallKeyMessage && callId == other.callId && epoch == other.epoch && key.contentEquals(other.key)

    override fun hashCode(): Int = callId.hashCode() * 31 + epoch.hashCode()

    /** Never the key. */
    override fun toString(): String = "CallKeyMessage(callId=$callId, epoch=$epoch)"
}

object CallLimits {
    /** A call message older than this is refused (a replayed offer must not ring). */
    const val MAX_AGE_MS = 120_000L
    /** SDP length in UTF-16 code units, like JavaScript's `.length`. */
    const val MAX_SDP_UNITS = 49_152
    const val ROOM_KEY_BYTES = 32
}

const val CALL_TYPE = "call"
const val CALL_KEY_TYPE = "callkey"

/** The envelope for one step of a call (part B). [ts] is checked for freshness by the receiver. */
fun encodeCall(conversationId: String, call: CallMessage, id: String, ts: Long): String =
    envelope(conversationId, CALL_TYPE, id, ts) {
        put("callId", call.callId)
        put("step", call.step)
        put("kind", call.kind)
        put("sdp", call.sdp)
        put("to", call.to)
    }

/** The envelope for a group call's room key (part B). */
fun encodeCallKey(conversationId: String, key: CallKeyMessage, id: String, ts: Long): String =
    envelope(conversationId, CALL_KEY_TYPE, id, ts) {
        put("callId", key.callId)
        put("epoch", key.epoch)
        put("key", java.util.Base64.getEncoder().encodeToString(key.key))
    }

/** Whether [question] and [options] make a poll every client accepts (the same rules as [decodeEnvelope]). */
fun validPoll(question: String, options: List<String>): Boolean =
    question.length in 1..MAX_QUESTION_CHARS && options.size in 2..MAX_OPTIONS &&
        options.all { it.length in 1..MAX_OPTION_CHARS } && options.toSet().size == options.size

/** A reply to [target] with [preview] cut to fit, or null when [target] isn't a message id an envelope can carry. */
fun chatReply(target: String, preview: String): ChatReply? =
    target.lowercase().takeIf(UUID_RE::matches)?.let { ChatReply(it, replyPreview(preview)) }

/** At most 200 characters of [text] for a reply's preview, never cutting a character in half. */
fun replyPreview(text: String): String {
    if (text.length <= MAX_PREVIEW_CHARS) return text
    val cut = if (Character.isHighSurrogate(text[MAX_PREVIEW_CHARS - 1])) MAX_PREVIEW_CHARS - 1 else MAX_PREVIEW_CHARS
    return text.substring(0, cut)
}

sealed interface DecodedEnvelope {
    /** A text or file message; a file has empty [text]. */
    data class Shown(
        val id: String,
        val ts: Long,
        val text: String,
        val file: FileDescriptor? = null,
        val reply: ChatReply? = null,
        val forwarded: Boolean = false,
        val exp: Long? = null,
        /** A poll: [text] is its question. */
        val poll: ChatPoll? = null,
        /** Part D: the story this text answers or reacts to. */
        val storyReply: ChatStoryReply? = null,
    ) : DecodedEnvelope

    data class Op(val id: String, val ts: Long, val op: ChatOp) : DecodedEnvelope

    /** One step of a call's setup (part B): acknowledged and handed to the call, never stored or folded. */
    data class Call(val id: String, val ts: Long, val call: CallMessage) : DecodedEnvelope

    /** A group call's room key (part B): acknowledged and handed to the call, never stored or folded. */
    data class CallKey(val id: String, val ts: Long, val key: CallKeyMessage) : DecodedEnvelope

    /** Part D: a story's key, for the local story-key store (pairwise only, never shown or folded). */
    data class StoryKey(val id: String, val ts: Long, val key: StoryKeyMessage) : DecodedEnvelope

    /** Part D: a named sticker answer, for the story's author (and the sender's other devices). Never shown in a chat. */
    data class StoryAnswer(val id: String, val ts: Long, val answer: StoryAnswerMessage) : DecodedEnvelope

    /** A newer envelope version: keep the item and ask for an update. */
    data object Newer : DecodedEnvelope

    /** An unknown type, or a message bound to another conversation: nothing to show; the item is acknowledged. [reason] is a code, never content. */
    data class Ignore(val reason: String) : DecodedEnvelope
}

private fun JsonElement?.string(): String? = (this as? JsonPrimitive)?.takeIf { it.isString }?.content

private fun JsonElement?.number(): JsonPrimitive? = (this as? JsonPrimitive)?.takeIf { !it.isString && it.doubleOrNull?.isFinite() == true }

/** A JSON true or false (never the string "true"). */
private fun JsonElement?.flag(): Boolean? = (this as? JsonPrimitive)?.takeIf { !it.isString }?.booleanOrNull

/** A whole number written as one ("1", not "1.0" or "1e0"). */
private fun JsonElement?.whole(): Long? = number()?.longOrNull

private fun String?.fitting(maxBytes: Int): String? = this?.takeIf { it.toByteArray().size <= maxBytes }

/** The optional fields of a text or file message, or null when one is malformed (the envelope is then ignored). */
private fun extrasOf(e: JsonObject): MessageExtras? {
    var reply: ChatReply? = null
    if ("reply" in e) {
        val r = e["reply"] as? JsonObject ?: return null
        val target = r["target"].string()?.takeIf(UUID_RE::matches) ?: return null
        val preview = r["preview"].string()?.takeIf { it.length <= MAX_PREVIEW_CHARS } ?: return null
        reply = ChatReply(target, preview)
    }
    val forwarded = if ("fwd" in e) e["fwd"].flag() ?: return null else false
    val exp = expOf(e) ?: return null
    var storyReply: ChatStoryReply? = null
    if ("storyReply" in e) {
        val r = e["storyReply"] as? JsonObject ?: return null
        val authorId = r["authorId"].string()?.takeIf(UUID_RE::matches) ?: return null
        val storyId = r["storyId"].string()?.takeIf(UUID_RE::matches) ?: return null
        val kind = r["kind"].string()?.takeIf { it in STORY_KINDS } ?: return null
        val preview = r["preview"].string()?.takeIf { it.length <= MAX_STORY_REPLY_PREVIEW_CHARS } ?: return null
        val reaction = r["reaction"].flag() ?: return null
        storyReply = ChatStoryReply(authorId, storyId, kind, preview, reaction)
    }
    return MessageExtras(reply, forwarded, exp.takeIf { it > 0 }, storyReply)
}

/** The disappearing time: 0 when absent, null when malformed. */
private fun expOf(e: JsonObject): Long? {
    if ("exp" !in e) return 0
    return e["exp"].whole()?.takeIf { it in 1..MAX_EXP_SECONDS }
}

private fun ids(element: JsonElement?, max: Int): List<String>? {
    val array = element as? JsonArray ?: return null
    if (array.isEmpty() || array.size > max) return null
    return array.map { it.string()?.takeIf(UUID_RE::matches) ?: return null }
}

private val KNOWN_TYPES = setOf(
    "text", "file", "poll", ChatOp.REACTION, ChatOp.EDIT, ChatOp.DELETE, ChatOp.VOTE, ChatOp.END, ChatOp.PIN, ChatOp.RECEIPT,
    CALL_TYPE, CALL_KEY_TYPE, STORY_KEY_TYPE, STORY_ANSWER_TYPE,
)

/**
 * Part B's ephemeral types. [ts] is required here (a JSON integer, never clamped): a call message's age is what
 * decides whether it may still ring. Context checks (a call the live events announced, its other party, the room
 * key's sender and epoch) are the call's own: see CallEnvelopes.kt.
 */
private fun decodeCallEnvelope(e: JsonObject, t: String, id: String, now: Long): DecodedEnvelope {
    val ts = e["ts"].whole() ?: return DecodedEnvelope.Ignore("bad_ts")
    val callId = e["callId"].string()?.takeIf(UUID_RE::matches) ?: return DecodedEnvelope.Ignore("bad_call_id")
    if (t == CALL_KEY_TYPE) {
        val epoch = e["epoch"].whole()?.takeIf { it >= 1 } ?: return DecodedEnvelope.Ignore("bad_epoch")
        val text = e["key"].string() ?: return DecodedEnvelope.Ignore("bad_key")
        val key = runCatching { java.util.Base64.getDecoder().decode(text) }.getOrNull()
            ?.takeIf { it.size == CallLimits.ROOM_KEY_BYTES } ?: return DecodedEnvelope.Ignore("bad_key")
        return DecodedEnvelope.CallKey(id, ts, CallKeyMessage(callId, epoch, key))
    }
    if (now - ts > CallLimits.MAX_AGE_MS) return DecodedEnvelope.Ignore("stale_call")
    if (ts - now > ChatLimits.FUTURE_MS) return DecodedEnvelope.Ignore("future_call")
    val step = e["step"].string()?.takeIf { it in CallMessage.STEPS } ?: return DecodedEnvelope.Ignore("bad_step")
    val kind = e["kind"].string()?.takeIf { it == "voice" || it == "video" } ?: return DecodedEnvelope.Ignore("bad_kind")
    val sdp = e["sdp"].string()?.takeIf { it.length in 1..CallLimits.MAX_SDP_UNITS } ?: return DecodedEnvelope.Ignore("bad_sdp")
    val to = e["to"].string()?.takeIf { it == CallMessage.EVERY_DEVICE || UUID_RE.matches(it) } ?: return DecodedEnvelope.Ignore("bad_to")
    return DecodedEnvelope.Call(id, ts, CallMessage(callId, step, kind, sdp, to))
}

/** Validates an envelope against the mailbox item that carried it (9.3 and part A). [conversationId] is the item's, lowercase. */
fun decodeEnvelope(plaintext: String, conversationId: String?, now: Long): DecodedEnvelope {
    val parsed = runCatching { Json.parseToJsonElement(plaintext) }.getOrNull() ?: return DecodedEnvelope.Ignore("not_json")
    val e = parsed as? JsonObject ?: return DecodedEnvelope.Ignore("not_an_envelope")
    val version = e["v"].number()?.doubleOrNull
    if (version != 1.0) return if (version != null && version > 1) DecodedEnvelope.Newer else DecodedEnvelope.Ignore("bad_version")
    val t = e["t"].string()
    if (t !in KNOWN_TYPES) return DecodedEnvelope.Ignore("unknown_type")
    val id = e["id"].string()?.takeIf(UUID_RE::matches) ?: return DecodedEnvelope.Ignore("bad_id")
    if (conversationId == null || e["c"].string() != conversationId) return DecodedEnvelope.Ignore("wrong_conversation")
    if (t == CALL_TYPE || t == CALL_KEY_TYPE) return decodeCallEnvelope(e, t, id, now)
    val claimed = e["ts"].number()?.let { it.longOrNull ?: it.doubleOrNull?.toLong() } ?: now
    val ts = claimed.coerceIn(now - ChatLimits.PAST_MS, now + ChatLimits.FUTURE_MS)
    if (t == STORY_KEY_TYPE || t == STORY_ANSWER_TYPE) return decodeStoryEnvelope(e, t, id, ts, conversationId)
    if (t == "file" || t == "text") {
        val extras = extrasOf(e) ?: return DecodedEnvelope.Ignore("bad_field")
        if (t == "file") {
            val descriptor = e["file"] as? JsonObject ?: return DecodedEnvelope.Ignore("bad_file")
            val file = FileMessage.parse(FileMessage.PREFIX + descriptor.toString()) ?: return DecodedEnvelope.Ignore("bad_file")
            // A story reply is a text: a file that claims to be one isn't shown.
            if (extras.storyReply != null) return DecodedEnvelope.Ignore("bad_field")
            return DecodedEnvelope.Shown(id, ts, "", file, extras.reply, extras.forwarded, extras.exp)
        }
        val body = e["body"].string().fitting(ChatLimits.MAX_TEXT_BYTES) ?: return DecodedEnvelope.Ignore("bad_body")
        return DecodedEnvelope.Shown(id, ts, body, reply = extras.reply, forwarded = extras.forwarded, exp = extras.exp, storyReply = extras.storyReply)
    }
    if (t == "poll") {
        val question = e["question"].string() ?: return DecodedEnvelope.Ignore("bad_poll")
        val options = (e["options"] as? JsonArray)?.map { it.string() ?: return DecodedEnvelope.Ignore("bad_poll") } ?: return DecodedEnvelope.Ignore("bad_poll")
        if (!validPoll(question, options)) return DecodedEnvelope.Ignore("bad_poll")
        val multi = e["multi"].flag() ?: return DecodedEnvelope.Ignore("bad_poll")
        val exp = expOf(e) ?: return DecodedEnvelope.Ignore("bad_field")
        return DecodedEnvelope.Shown(id, ts, question, exp = exp.takeIf { it > 0 }, poll = ChatPoll(options, multi))
    }
    if (t == ChatOp.RECEIPT) {
        val targets = ids(e["targets"], ChatLimits.MAX_RECEIPT_TARGETS) ?: return DecodedEnvelope.Ignore("bad_target")
        val status = e["status"].string()?.takeIf { it == ChatOp.DELIVERED || it == ChatOp.READ } ?: return DecodedEnvelope.Ignore("bad_status")
        return DecodedEnvelope.Op(id, ts, ChatOp.receipt(targets.distinct(), status))
    }
    val target = e["target"].string()?.takeIf(UUID_RE::matches) ?: return DecodedEnvelope.Ignore("bad_target")
    return when (t) {
        ChatOp.DELETE -> DecodedEnvelope.Op(id, ts, ChatOp.delete(target))
        ChatOp.END -> DecodedEnvelope.Op(id, ts, ChatOp.end(target))
        ChatOp.EDIT -> {
            val body = e["body"].string().fitting(ChatLimits.MAX_TEXT_BYTES)?.takeIf { it.isNotEmpty() } ?: return DecodedEnvelope.Ignore("bad_body")
            DecodedEnvelope.Op(id, ts, ChatOp.edit(target, body))
        }
        ChatOp.VOTE -> {
            val array = e["choices"] as? JsonArray ?: return DecodedEnvelope.Ignore("bad_choices")
            if (array.size > MAX_OPTIONS) return DecodedEnvelope.Ignore("bad_choices")
            val choices = array.map { c -> c.whole()?.takeIf { it in 0 until MAX_OPTIONS }?.toInt() ?: return DecodedEnvelope.Ignore("bad_choices") }
            if (choices.toSet().size != choices.size) return DecodedEnvelope.Ignore("bad_choices")
            DecodedEnvelope.Op(id, ts, ChatOp.vote(target, choices))
        }
        ChatOp.PIN -> {
            val pinned = e["pinned"].flag() ?: return DecodedEnvelope.Ignore("bad_pinned")
            DecodedEnvelope.Op(id, ts, ChatOp.pin(target, pinned))
        }
        else -> {
            val emoji = e["emoji"].string().fitting(MAX_EMOJI_BYTES) ?: return DecodedEnvelope.Ignore("bad_emoji")
            DecodedEnvelope.Op(id, ts, ChatOp.reaction(target, emoji))
        }
    }
}

/** When a message disappears (ms), or null. */
val ChatRecord.expiresAt: Long? get() = exp?.let { ts + it * 1000 }

fun ChatRecord.expired(now: Long): Boolean = expiresAt?.let { it <= now } == true

data class ShownReaction(val userId: String, val emoji: String)

/** One person's receipt for a message: [status] is "read" if any of theirs says so, otherwise "delivered". */
data class ShownReceipt(val userId: String, val status: String)

/** A poll with its votes folded in. [choices]: each voter's counted vote (people who took theirs back aren't in it). */
data class ShownPoll(val options: List<String>, val multi: Boolean, val votes: List<Int>, val ended: Boolean, val choices: Map<String, List<Int>> = emptyMap())

/** A message as the chat shows it. [record] has the latest text; a deleted message's is empty, with no file. */
data class ShownMessage(
    val record: ChatRecord,
    /** When the text shown was written; null when it was never edited. */
    val editedAt: Long? = null,
    val deleted: Boolean = false,
    val reactions: List<ShownReaction> = emptyList(),
    /** The message this one answers (shown even when this device never had it), with its preview. */
    val replyTo: String? = null,
    val replyPreview: String? = null,
    val forwarded: Boolean = false,
    val pinned: Boolean = false,
    /** Who made the pin that stands. */
    val pinnedBy: String? = null,
    val poll: ShownPoll? = null,
    /** One per person other than its sender, by user. */
    val receipts: List<ShownReceipt> = emptyList(),
    val expiresAt: Long? = null,
)

/** Later wins: by the sender's clock, then by id, so every device settles on the same answer whatever order things arrived in. */
private fun newer(a: ChatRecord, b: ChatRecord): Boolean = a.ts > b.ts || (a.ts == b.ts && a.id > b.id)

/** The messages their own sender deleted, among [records]. */
private fun deletedIds(records: List<ChatRecord>, messages: Map<String, ChatRecord>): Set<String> =
    records.mapNotNullTo(HashSet()) { r ->
        r.op?.takeIf { it.t == ChatOp.DELETE && messages[it.target]?.fromUserId == r.fromUserId }?.target
    }

private fun validVote(choices: List<Int>, poll: ChatPoll): Boolean =
    choices.all { it in poll.options.indices } && (poll.multi || choices.size <= 1)

/**
 * Turns stored records into what a chat shows. Only the person who sent a message can edit or delete it, and only a
 * poll's author can end it. An op whose message hasn't arrived yet waits in the history, so arrival order never
 * changes the result. With [now], messages whose time is up are left out (and [planErase] erases them).
 */
fun foldChat(records: List<ChatRecord>, now: Long? = null): List<ShownMessage> {
    val messages = records.filter { it.op == null }.associateBy { it.id }
    val deleted = deletedIds(records, messages)
    val edits = HashMap<String, ChatRecord>()
    val reactions = HashMap<Pair<String, String>, ChatRecord>()
    val pins = HashMap<String, ChatRecord>()
    val ends = HashMap<String, ChatRecord>()
    val votes = HashMap<String, MutableList<ChatRecord>>()
    val receipts = HashMap<String, HashMap<String, String>>()
    for (record in records) {
        val op = record.op ?: continue
        if (op.t == ChatOp.RECEIPT) {
            val status = op.status ?: continue
            for (target in op.about) {
                val message = messages[target] ?: continue
                if (record.fromUserId == message.fromUserId) continue
                val byUser = receipts.getOrPut(target) { HashMap() }
                if (byUser[record.fromUserId] != ChatOp.READ) byUser[record.fromUserId] = status
            }
            continue
        }
        val message = messages[op.target] ?: continue
        if (op.target in deleted) continue
        when (op.t) {
            ChatOp.EDIT -> if (record.fromUserId == message.fromUserId && message.file == null && message.poll == null) {
                val held = edits[op.target]
                if (held == null || newer(record, held)) edits[op.target] = record
            }
            ChatOp.REACTION -> {
                val key = op.target to record.fromUserId
                val held = reactions[key]
                if (held == null || newer(record, held)) reactions[key] = record
            }
            ChatOp.PIN -> {
                val held = pins[op.target]
                if (held == null || newer(record, held)) pins[op.target] = record
            }
            ChatOp.END -> if (message.poll != null && record.fromUserId == message.fromUserId) {
                val held = ends[op.target]
                // The first end counts: votes before it stand.
                if (held == null || newer(held, record)) ends[op.target] = record
            }
            ChatOp.VOTE -> if (message.poll != null) votes.getOrPut(op.target) { ArrayList() } += record
        }
    }
    val byMessage = reactions.values.filter { !it.op?.emoji.isNullOrEmpty() }.groupBy { it.op!!.target }
    return messages.values
        .filter { now == null || !it.expired(now) }
        .map { message ->
            if (message.id in deleted) {
                ShownMessage(message.copy(text = "", file = null, reply = null, poll = null, storyReply = null), deleted = true, expiresAt = message.expiresAt)
            } else {
                val edit = edits[message.id]
                val pin = pins[message.id]?.takeIf { it.op?.pinned == true }
                ShownMessage(
                    record = if (edit != null) message.copy(text = edit.op?.body.orEmpty()) else message,
                    editedAt = edit?.ts,
                    reactions = byMessage[message.id].orEmpty().map { ShownReaction(it.fromUserId, it.op?.emoji.orEmpty()) }.sortedBy { it.userId },
                    replyTo = message.reply?.target,
                    replyPreview = message.reply?.preview,
                    forwarded = message.forwarded,
                    pinned = pin != null,
                    pinnedBy = pin?.fromUserId,
                    poll = message.poll?.let { foldPoll(it, votes[message.id].orEmpty(), ends[message.id]) },
                    receipts = receipts[message.id].orEmpty().map { (user, status) -> ShownReceipt(user, status) }.sortedBy { it.userId },
                    expiresAt = message.expiresAt,
                )
            }
        }
        .sortedWith(compareBy({ it.record.ts }, { it.record.arrivedAt }, { it.record.id }))
}

/** Each voter's latest valid vote from before the poll's first end; an invalid one leaves their previous vote standing. */
private fun foldPoll(poll: ChatPoll, votes: List<ChatRecord>, end: ChatRecord?): ShownPoll {
    val latest = HashMap<String, ChatRecord>()
    for (vote in votes) {
        if (end != null && !newer(end, vote)) continue
        if (!validVote(vote.op?.choices.orEmpty(), poll)) continue
        val held = latest[vote.fromUserId]
        if (held == null || newer(vote, held)) latest[vote.fromUserId] = vote
    }
    val counts = IntArray(poll.options.size)
    val choices = HashMap<String, List<Int>>()
    for ((voter, vote) in latest) {
        val picked = vote.op?.choices.orEmpty()
        if (picked.isEmpty()) continue
        picked.forEach { counts[it]++ }
        choices[voter] = picked
    }
    return ShownPoll(poll.options, poll.multi, counts.toList(), ended = end != null, choices = choices)
}

/**
 * What [ChatHistory.eraseDeleted] does: records to [remove], messages to [blank] (their text, file key, reply and
 * poll), and receipts to [trim] down to the targets that still exist (record id to its remaining targets).
 */
class ErasePlan(val remove: Set<String>, val blank: Set<String>, val trim: Map<String, List<String>> = emptyMap()) {
    val isEmpty: Boolean get() = remove.isEmpty() && blank.isEmpty() && trim.isEmpty()

    /** [record] with what this plan leaves of it. */
    fun applyTo(record: ChatRecord): ChatRecord = when {
        record.id in blank -> record.copy(text = "", file = null, reply = null, poll = null, storyReply = null)
        record.id in trim -> record.copy(op = record.op?.let { ChatOp.receipt(trim.getValue(record.id), it.status.orEmpty()) })
        else -> record
    }
}

private fun ChatRecord.hasContent() = text.isNotEmpty() || file != null || reply != null || poll != null || storyReply != null

/**
 * Everything that still repeats a message its sender deleted, or one whose time is up at [now]. A delete stays: it's
 * what keeps showing the message as deleted. An expired message stays only as a blank stub (so ops that arrive late
 * for it are erased too); everything that refers to it goes, its delete included.
 */
fun planErase(records: List<ChatRecord>, now: Long? = null): ErasePlan {
    val messages = records.filter { it.op == null }.associateBy { it.id }
    val deleted = deletedIds(records, messages)
    val expired = if (now == null) emptySet() else messages.values.filter { it.expired(now) }.mapTo(HashSet()) { it.id }
    val gone = deleted + expired
    val remove = HashSet<String>()
    val trim = HashMap<String, List<String>>()
    for (r in records) {
        val op = r.op ?: continue
        if (op.t == ChatOp.RECEIPT) {
            val left = op.about.filter { it !in gone }
            if (left.isEmpty()) remove += r.id else if (left.size != op.about.size) trim[r.id] = left
        } else if (op.target in expired || (op.t != ChatOp.DELETE && op.target in deleted)) {
            remove += r.id
        }
    }
    return ErasePlan(
        remove = remove,
        blank = gone.filterTo(HashSet()) { id -> messages.getValue(id).hasContent() },
        trim = trim,
    )
}
