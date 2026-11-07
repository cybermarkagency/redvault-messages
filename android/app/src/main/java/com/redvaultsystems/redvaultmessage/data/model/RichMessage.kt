package com.redvaultsystems.redvaultmessage.data.model

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonObjectBuilder
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import kotlinx.serialization.json.putJsonObject

// Port of ui/src/lib/richMessage.ts: replies and reactions carried inside
// the ordinary E2EE body, so the server learns nothing new. Byte-for-byte
// the same wire format (pinned by RichMessageTest); change both together.
//
//   reply:    "\u0000rv-msg:" + {"type":"text","version":1,"text":…,"replyTo":{"messageId","senderId","preview"}}
//   reaction: "\u0000rv-reaction:" + {"type":"reaction","version":1,"messageId","emoji"}  any single emoji
//
// Plain (non-reply) text stays plain. Removing a reaction deletes the
// reaction message itself. Everything parsed is untrusted input.
//
// Optional fields of the "text" type (the same rules as the web and desktop
// apps). In each, "text" is what an app that doesn't know the field shows:
//
//   edit:      {…,"text":<new text>,"edits":<messageId>}  only the original sender may edit
//   forwarded: {…,"forwarded":true}
//   pin:       {…,"text":"📌 Pinned: …","pin":{"messageId","preview"}}
//   unpin:     {…,"text":"Unpinned a message","unpin":<messageId>}
//   poll:      {…,"text":"📊 Poll: …","poll":{"question","options":[…],"multiple":bool}}
//   vote:      {…,"text":"Voted: …","vote":{"pollId","options":[indexes]}}  empty takes it back
//   end poll:  {…,"text":"Ended the poll","endPoll":<pollId>}  only the poll's creator
//   channel:   {…,"forwarded":true,"channelPost":{"channelId","postId","channelName"}}
//              a channel post forwarded into a chat; "text" is the post. Each
//              copy that's seen counts as a view of the post.

@Serializable
data class ReplyRef(val messageId: String, val senderId: String, val preview: String)

data class PinRef(val messageId: String, val preview: String)

/** A reply or reaction to someone's story, sent to its author as a chat message. Only the story's id and a short label travel with it, never its key. */
data class StoryReplyRef(val authorId: String, val storyId: String, val kind: String, val preview: String, val reaction: Boolean)

data class PollDef(val question: String, val options: List<String>, val multiple: Boolean)

/** [options]: indexes into the poll's options; empty takes the vote back. */
data class VoteRef(val pollId: String, val options: List<Int>)

/** The channel post a forwarded message came from: shown as "Forwarded from <channel>", and counted as a view of it. */
data class ChannelPostRef(val channelId: String, val postId: String, val channelName: String)

@Serializable
private data class ReplyBody(val type: String, val version: Int, val text: String, val replyTo: ReplyRef)

@Serializable
private data class ReactionBody(val type: String, val version: Int, val messageId: String, val emoji: String)

sealed interface ParsedBody {
    /** Ids in the optional fields are lowercased; at most one of them is normally set. */
    data class Text(
        val text: String,
        val replyTo: ReplyRef?,
        /** This is an edit: the new text of that message. */
        val edits: String? = null,
        val forwarded: Boolean = false,
        val pin: PinRef? = null,
        val unpin: String? = null,
        val poll: PollDef? = null,
        val vote: VoteRef? = null,
        val endPoll: String? = null,
        val channelPost: ChannelPostRef? = null,
        val storyReply: StoryReplyRef? = null,
    ) : ParsedBody
    data class Reaction(val messageId: String, val emoji: String) : ParsedBody
    data object Invalid : ParsedBody
}

object RichMessage {
    private const val TEXT_PREFIX = "\u0000rv-msg:"
    private const val REACTION_PREFIX = "\u0000rv-reaction:"
    /** The quick reactions, offered first and shown first, in this order. Any other single emoji is valid too ([isReactionEmoji]). */
    val REACTION_EMOJI = listOf("👍", "❤️", "😂", "😮", "😢", "🙏")
    const val MAX_POLL_OPTIONS = 10
    const val MAX_POLL_QUESTION = 300
    const val MAX_POLL_OPTION = 100
    private const val MAX_CHANNEL_NAME = 80

    private val json = Json { encodeDefaults = true }
    private val UUID_RE = Regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", RegexOption.IGNORE_CASE)
    private const val MAX_TEXT = 65_536
    private const val MAX_PREVIEW = 200
    private const val MAX_STORY_REPLY_PREVIEW = 80
    private val HIDDEN_CHARS = Regex("[\\u0000-\\u0008\\u000b-\\u001f\\u007f-\\u009f\\u200b-\\u200f\\u202a-\\u202e\\u2060-\\u2069\\ufeff]")

    fun isStructured(plaintext: String) = plaintext.startsWith(TEXT_PREFIX) || plaintext.startsWith(REACTION_PREFIX)

    /** A short, safe quote: hidden characters stripped, whitespace collapsed, capped at 200 UTF-16 units like the web. */
    fun makePreview(text: String): String {
        val clean = text.replace(HIDDEN_CHARS, "").replace(Regex("\\s+"), " ").trim()
        return if (clean.length > MAX_PREVIEW) clean.substring(0, MAX_PREVIEW - 1) + "…" else clean
    }

    fun serializeReply(text: String, replyTo: ReplyRef): String =
        TEXT_PREFIX + json.encodeToString(ReplyBody.serializer(), ReplyBody("text", 1, text, replyTo.copy(preview = makePreview(replyTo.preview))))

    /** A "text" body with [fallback] as its text, then [extra]'s fields, in the web's key order. */
    private fun textBody(fallback: String, extra: JsonObjectBuilder.() -> Unit): String =
        TEXT_PREFIX + json.encodeToString(
            JsonObject.serializer(),
            buildJsonObject {
                put("type", "text")
                put("version", 1)
                put("text", fallback)
                extra()
            },
        )

    /** A new version of the text of one of your own messages. */
    fun serializeEdit(text: String, messageId: String): String = textBody(text) { put("edits", messageId) }

    /** Text forwarded from another chat (a reply's quote isn't carried over). */
    fun serializeForwarded(text: String): String = textBody(text) { put("forwarded", true) }

    /** A channel post forwarded into a chat, keeping where it came from (also when forwarded on again). */
    fun serializeChannelForward(text: String, ref: ChannelPostRef): String {
        val channelName = makePreview(ref.channelName).take(MAX_CHANNEL_NAME)
        return textBody(text) {
            put("forwarded", true)
            putJsonObject("channelPost") {
                put("channelId", ref.channelId.lowercase())
                put("postId", ref.postId.lowercase())
                put("channelName", channelName)
            }
        }
    }

    fun serializePin(messageId: String, preview: String): String {
        val quote = makePreview(preview)
        return textBody("📌 Pinned: $quote") {
            putJsonObject("pin") {
                put("messageId", messageId)
                put("preview", quote)
            }
        }
    }

    fun serializeUnpin(messageId: String): String = textBody("Unpinned a message") { put("unpin", messageId) }

    /** Throws unless there's a question and 2-10 distinct, non-empty options. */
    fun serializePoll(question: String, options: List<String>, multiple: Boolean): String {
        val q = question.trim()
        val clean = options.map { it.trim() }.filter { it.isNotEmpty() }
        require(q.isNotEmpty() && q.length <= MAX_POLL_QUESTION) { "Poll question" }
        require(clean.size in 2..MAX_POLL_OPTIONS && clean.toSet().size == clean.size && clean.all { it.length <= MAX_POLL_OPTION }) {
            "A poll needs 2 to 10 different options"
        }
        val fallback = (listOf("📊 Poll: $q") + clean.mapIndexed { i, o -> "${i + 1}. $o" }).joinToString("\n")
        return textBody(fallback) {
            putJsonObject("poll") {
                put("question", q)
                putJsonArray("options") { clean.forEach { add(JsonPrimitive(it)) } }
                put("multiple", multiple)
            }
        }
    }

    /** [labels] only make the fallback text; empty [options] takes the vote back. */
    fun serializeVote(pollId: String, options: List<Int>, labels: List<String>): String {
        val fallback = if (options.isEmpty()) "Took back a poll vote"
        else "Voted: " + options.joinToString(", ") { labels.getOrNull(it) ?: (it + 1).toString() }
        return textBody(fallback) {
            putJsonObject("vote") {
                put("pollId", pollId)
                putJsonArray("options") { options.forEach { add(JsonPrimitive(it)) } }
            }
        }
    }

    fun serializeEndPoll(pollId: String): String = textBody("Ended the poll") { put("endPoll", pollId) }

    /** A reply (or a quick reaction emoji) to a story, as a message to its author (the web's serializeStoryReply). */
    fun serializeStoryReply(text: String, ref: StoryReplyRef): String = textBody(text) {
        putJsonObject("storyReply") {
            put("authorId", ref.authorId.lowercase())
            put("storyId", ref.storyId.lowercase())
            put("kind", ref.kind)
            put("preview", makePreview(ref.preview).take(MAX_STORY_REPLY_PREVIEW))
            put("reaction", ref.reaction)
        }
    }

    /** Throws [IllegalArgumentException] unless [emoji] is one emoji ([isReactionEmoji]). */
    fun serializeReaction(messageId: String, emoji: String): String {
        require(isReactionEmoji(emoji)) { "Unsupported reaction" }
        return REACTION_PREFIX + json.encodeToString(ReactionBody.serializer(), ReactionBody("reaction", 1, messageId, emoji))
    }

    // Reactions: exactly one emoji, by the same grammar as the web
    // (richMessage.ts) and desktop (rich_message.cpp) apps. Change all three together.
    private const val MAX_EMOJI_CODE_POINTS = 16
    private const val ZWJ = 0x200D
    private const val VS16 = 0xFE0F
    private const val KEYCAP = 0x20E3

    /** The emoji blocks, plus the older symbols that are drawn as emoji. */
    private val PICTOGRAPH_RANGES = arrayOf(
        0x00A9..0x00A9, 0x00AE..0x00AE, 0x203C..0x203C, 0x2049..0x2049, 0x2122..0x2122, 0x2139..0x2139,
        0x2194..0x2199, 0x21A9..0x21AA, 0x231A..0x231B, 0x2328..0x2328, 0x23CF..0x23CF, 0x23E9..0x23F3,
        0x23F8..0x23FA, 0x24C2..0x24C2, 0x25AA..0x25AB, 0x25B6..0x25B6, 0x25C0..0x25C0, 0x25FB..0x25FE,
        0x2600..0x27BF, 0x2934..0x2935, 0x2B05..0x2B07, 0x2B1B..0x2B1C, 0x2B50..0x2B50, 0x2B55..0x2B55,
        0x3030..0x3030, 0x303D..0x303D, 0x3297..0x3297, 0x3299..0x3299, 0x1F000..0x1FAFF,
    )

    private fun isRegional(c: Int) = c in 0x1F1E6..0x1F1FF
    private fun isSkinTone(c: Int) = c in 0x1F3FB..0x1F3FF
    private fun isTag(c: Int) = c in 0xE0020..0xE007F
    private fun isPictograph(c: Int) = !isRegional(c) && !isSkinTone(c) && PICTOGRAPH_RANGES.any { c in it }

    /**
     * Whether [emoji] is exactly one emoji: a pictograph (optionally with
     * VS16, a skin tone and tag characters), a flag or a keycap, or several
     * of those joined by ZWJ.
     */
    fun isReactionEmoji(emoji: String): Boolean {
        // A lone surrogate comes through as itself and fails below.
        val cps = emoji.codePoints().toArray()
        val n = cps.size
        if (n == 0 || n > MAX_EMOJI_CODE_POINTS) return false
        // A keycap: 0-9, # or *, an optional VS16, then U+20E3.
        if (cps[0] in 0x30..0x39 || cps[0] == 0x23 || cps[0] == 0x2A) {
            return (n == 2 && cps[1] == KEYCAP) || (n == 3 && cps[1] == VS16 && cps[2] == KEYCAP)
        }
        var i = 0
        fun at(k: Int) = if (k < n) cps[k] else 0
        // One element: a flag (two regional indicators), or a pictograph with an
        // optional VS16, then an optional skin tone, then any tag characters.
        fun element(): Boolean {
            if (isRegional(at(i))) {
                if (!isRegional(at(i + 1))) return false
                i += 2
                return true
            }
            if (!isPictograph(at(i))) return false
            i++
            if (at(i) == VS16) i++
            if (isSkinTone(at(i))) i++
            while (isTag(at(i))) i++
            return true
        }
        if (!element()) return false
        while (i < n) {
            if (cps[i] != ZWJ) return false
            i++
            if (!element()) return false
        }
        return true
    }

    fun parse(plaintext: String): ParsedBody {
        val isReaction = plaintext.startsWith(REACTION_PREFIX)
        val body = plaintext.removePrefix(if (isReaction) REACTION_PREFIX else TEXT_PREFIX)
        val d = runCatching { json.parseToJsonElement(body).jsonObject }.getOrNull() ?: return ParsedBody.Invalid
        if (int(d, "version") != 1) return ParsedBody.Invalid

        if (isReaction) {
            val messageId = str(d, "messageId")?.takeIf(UUID_RE::matches) ?: return ParsedBody.Invalid
            val emoji = str(d, "emoji")?.takeIf(::isReactionEmoji) ?: return ParsedBody.Invalid
            if (str(d, "type") != "reaction") return ParsedBody.Invalid
            return ParsedBody.Reaction(messageId.lowercase(), emoji)
        }

        if (str(d, "type") != "text") return ParsedBody.Invalid
        val text = str(d, "text")?.takeIf { it.length <= MAX_TEXT } ?: return ParsedBody.Invalid
        val withExtras = parseExtras(d, text) ?: return ParsedBody.Invalid
        if (!d.containsKey("replyTo")) return withExtras
        val r = runCatching { d["replyTo"]?.jsonObject }.getOrNull() ?: return ParsedBody.Invalid
        val messageId = str(r, "messageId")?.takeIf(UUID_RE::matches) ?: return ParsedBody.Invalid
        val senderId = str(r, "senderId")?.takeIf(UUID_RE::matches) ?: return ParsedBody.Invalid
        val preview = str(r, "preview") ?: return ParsedBody.Invalid
        return withExtras.copy(replyTo = ReplyRef(messageId.lowercase(), senderId.lowercase(), makePreview(preview)))
    }

    /** The optional fields above; null if any is present but malformed (the whole message is then invalid). */
    private fun parseExtras(d: JsonObject, text: String): ParsedBody.Text? {
        var out = ParsedBody.Text(text, null)
        if (d.containsKey("edits")) out = out.copy(edits = uuid(d, "edits") ?: return null)
        if (d.containsKey("forwarded")) {
            val forwarded = runCatching { d["forwarded"]?.jsonPrimitive?.takeIf { !it.isString }?.booleanOrNull }.getOrNull() ?: return null
            out = out.copy(forwarded = forwarded)
        }
        if (d.containsKey("pin")) {
            val p = obj(d, "pin") ?: return null
            val messageId = uuid(p, "messageId") ?: return null
            val preview = str(p, "preview") ?: return null
            out = out.copy(pin = PinRef(messageId, makePreview(preview)))
        }
        if (d.containsKey("unpin")) out = out.copy(unpin = uuid(d, "unpin") ?: return null)
        if (d.containsKey("poll")) {
            val p = obj(d, "poll") ?: return null
            val question = str(p, "question")?.takeIf { it.isNotEmpty() && it.length <= MAX_POLL_QUESTION } ?: return null
            val options = (p["options"] as? JsonArray)?.takeIf { it.size in 2..MAX_POLL_OPTIONS } ?: return null
            val labels = options.map { o ->
                (o as? JsonPrimitive)?.takeIf { it.isString }?.content?.takeIf { it.isNotEmpty() && it.length <= MAX_POLL_OPTION } ?: return null
            }
            val multiple = if (p.containsKey("multiple")) {
                (p["multiple"] as? JsonPrimitive)?.takeIf { !it.isString }?.booleanOrNull ?: return null
            } else {
                false
            }
            out = out.copy(poll = PollDef(question, labels, multiple))
        }
        if (d.containsKey("vote")) {
            val v = obj(d, "vote") ?: return null
            val pollId = uuid(v, "pollId") ?: return null
            val raw = (v["options"] as? JsonArray)?.takeIf { it.size <= MAX_POLL_OPTIONS } ?: return null
            val options = ArrayList<Int>()
            for (o in raw) {
                val index = (o as? JsonPrimitive)?.takeIf { !it.isString }?.intOrNull?.takeIf { it in 0 until MAX_POLL_OPTIONS } ?: return null
                if (index !in options) options += index
            }
            out = out.copy(vote = VoteRef(pollId, options))
        }
        if (d.containsKey("endPoll")) out = out.copy(endPoll = uuid(d, "endPoll") ?: return null)
        if (d.containsKey("channelPost")) {
            val c = obj(d, "channelPost") ?: return null
            val channelId = uuid(c, "channelId") ?: return null
            val postId = uuid(c, "postId") ?: return null
            val channelName = str(c, "channelName")?.let { makePreview(it).take(MAX_CHANNEL_NAME) }?.takeIf { it.isNotEmpty() } ?: return null
            out = out.copy(channelPost = ChannelPostRef(channelId, postId, channelName))
        }
        if (d.containsKey("storyReply")) {
            val r = obj(d, "storyReply") ?: return null
            val authorId = uuid(r, "authorId") ?: return null
            val storyId = uuid(r, "storyId") ?: return null
            val kind = str(r, "kind")?.takeIf { it == "image" || it == "video" || it == "text" } ?: return null
            val preview = str(r, "preview") ?: return null
            val reaction = (r["reaction"] as? JsonPrimitive)?.takeIf { !it.isString }?.booleanOrNull ?: return null
            out = out.copy(storyReply = StoryReplyRef(authorId, storyId, kind, makePreview(preview).take(MAX_STORY_REPLY_PREVIEW), reaction))
        }
        return out
    }

    private fun str(o: JsonObject, k: String) = runCatching { o[k]?.jsonPrimitive?.takeIf { it.isString }?.contentOrNull }.getOrNull()
    private fun int(o: JsonObject, k: String) = runCatching { o[k]?.jsonPrimitive?.takeIf { !it.isString }?.intOrNull }.getOrNull()
    private fun uuid(o: JsonObject, k: String) = str(o, k)?.takeIf(UUID_RE::matches)?.lowercase()
    private fun obj(o: JsonObject, k: String) = o[k] as? JsonObject
}
