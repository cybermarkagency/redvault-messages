// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put
import java.nio.ByteBuffer
import java.util.Base64
import java.util.UUID

/**
 * Part D (docs/signal-integration/12-part-d-stories.md): the two story envelopes that travel pairwise over device keys,
 * and the RVSD container of anonymous sticker answers. Shared vectors: desktop/tests/vectors/v3_story_vectors.json.
 * No Android classes here; nothing in this file logs.
 */
const val STORY_KEY_TYPE = "storykey"
const val STORY_ANSWER_TYPE = "storyanswer"

object StoryEnvelopeLimits {
    const val KEY_BYTES = 32
    /** A sticker id: what the story's details call it (the apps make 6 to 16 letters and digits; others are read too). */
    const val MAX_STICKER_ID_CHARS = 64

    /** The sticker ids a `storyanswer` may carry (both apps): 1 to 64 of A-Z, a-z, 0-9, `_` and `-`. */
    val STICKER_ID = Regex("^[A-Za-z0-9_-]{1,64}$")
    /** The answer JSON inside a `storyanswer`. */
    const val MAX_PAYLOAD_BYTES = 4096
}

/** A story's key K for [storyId], to forget after [expiresAt] (ms). Never printed. */
class StoryKeyMessage(val storyId: String, val key: ByteArray, val expiresAt: Long) {
    override fun equals(other: Any?): Boolean =
        other is StoryKeyMessage && storyId == other.storyId && key.contentEquals(other.key) && expiresAt == other.expiresAt

    override fun hashCode(): Int = storyId.hashCode() * 31 + expiresAt.hashCode()

    override fun toString(): String = "StoryKeyMessage(storyId=$storyId, expiresAt=$expiresAt)"
}

/** A named answer to a sticker: revision [rev] (≥ 1) of this person's answer, [payload] the answer JSON as today. Never printed. */
class StoryAnswerMessage(val storyId: String, val stickerId: String, val rev: Long, val final: Boolean, val payload: JsonObject) {
    override fun equals(other: Any?): Boolean =
        other is StoryAnswerMessage && storyId == other.storyId && stickerId == other.stickerId && rev == other.rev && final == other.final && payload == other.payload

    override fun hashCode(): Int = (storyId.hashCode() * 31 + stickerId.hashCode()) * 31 + rev.hashCode()

    override fun toString(): String = "StoryAnswerMessage(storyId=$storyId, stickerId=$stickerId, rev=$rev, final=$final)"
}

private val STORY_UUID = Regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
private const val STORY_PAD_BUCKET = 256

private fun storyEnvelope(t: String, storyId: String, id: String, ts: Long, fields: kotlinx.serialization.json.JsonObjectBuilder.() -> Unit): String {
    val json = buildJsonObject {
        put("v", 1)
        put("t", t)
        put("id", id)
        // A story has no chat: its conversation is the story itself.
        put("c", storyId)
        put("ts", ts)
        fields()
    }.toString()
    val bytes = json.toByteArray(Charsets.UTF_8).size
    return json + " ".repeat((bytes / STORY_PAD_BUCKET + 1) * STORY_PAD_BUCKET - bytes)
}

fun encodeStoryKey(message: StoryKeyMessage, id: String, ts: Long): String {
    require(message.key.size == StoryEnvelopeLimits.KEY_BYTES) { "A story key is 32 bytes." }
    val storyId = message.storyId.lowercase()
    return storyEnvelope(STORY_KEY_TYPE, storyId, id.lowercase(), ts) {
        put("storyId", storyId)
        put("key", Base64.getEncoder().encodeToString(message.key))
        put("expiresAt", message.expiresAt)
    }
}

fun encodeStoryAnswer(message: StoryAnswerMessage, id: String, ts: Long): String {
    require(message.rev >= 1) { "A revision is at least 1." }
    require(StoryEnvelopeLimits.STICKER_ID.matches(message.stickerId)) { "Not a sticker id." }
    val storyId = message.storyId.lowercase()
    return storyEnvelope(STORY_ANSWER_TYPE, storyId, id.lowercase(), ts) {
        put("storyId", storyId)
        put("stickerId", message.stickerId)
        put("rev", message.rev)
        put("final", message.final)
        put("payload", message.payload)
    }
}

private fun JsonElement?.storyString(): String? = (this as? JsonPrimitive)?.takeIf { it.isString }?.content

/** A JSON integer written as one: "9.5", "1e3", strings and null are not. */
private fun JsonElement?.storyWhole(): Long? =
    (this as? JsonPrimitive)?.takeIf { !it.isString && it.doubleOrNull?.isFinite() == true }?.longOrNull

/**
 * The fields of a `storykey` or `storyanswer` (the common ones, [id] and [ts], are already checked by decodeEnvelope).
 * [conversationId] is the mailbox item's: it must be the story id itself. Anything malformed is ignored, as in part A.
 */
internal fun decodeStoryEnvelope(e: JsonObject, t: String, id: String, ts: Long, conversationId: String): DecodedEnvelope {
    val storyId = e["storyId"].storyString()?.takeIf(STORY_UUID::matches) ?: return DecodedEnvelope.Ignore("bad_story_id")
    if (storyId != conversationId) return DecodedEnvelope.Ignore("story_not_conversation")
    if (t == STORY_KEY_TYPE) {
        val text = e["key"].storyString() ?: return DecodedEnvelope.Ignore("bad_key")
        val key = runCatching { Base64.getDecoder().decode(text) }.getOrNull()
            ?.takeIf { it.size == StoryEnvelopeLimits.KEY_BYTES } ?: return DecodedEnvelope.Ignore("bad_key")
        val expiresAt = e["expiresAt"].storyWhole() ?: return DecodedEnvelope.Ignore("bad_expires_at")
        return DecodedEnvelope.StoryKey(id, ts, StoryKeyMessage(storyId, key, expiresAt))
    }
    val stickerId = e["stickerId"].storyString()?.takeIf(StoryEnvelopeLimits.STICKER_ID::matches)
        ?: return DecodedEnvelope.Ignore("bad_sticker_id")
    val rev = e["rev"].storyWhole()?.takeIf { it >= 1 } ?: return DecodedEnvelope.Ignore("bad_rev")
    val final = (e["final"] as? JsonPrimitive)?.takeIf { !it.isString }?.booleanOrNull ?: return DecodedEnvelope.Ignore("bad_final")
    val payload = e["payload"] as? JsonObject ?: return DecodedEnvelope.Ignore("bad_payload")
    if (payload.toString().toByteArray(Charsets.UTF_8).size > StoryEnvelopeLimits.MAX_PAYLOAD_BYTES) return DecodedEnvelope.Ignore("bad_payload")
    return DecodedEnvelope.StoryAnswer(id, ts, StoryAnswerMessage(storyId, stickerId, rev, final, payload))
}

/** The answer JSON a `storyanswer` carries, back as text for the sticker's parser. */
fun StoryAnswerMessage.payloadJson(): String = Json.encodeToString(JsonObject.serializer(), payload)

// --- RVSD: an anonymous answer, sealed once per author device -------------------------------------------------------

/** One copy of an anonymous answer: [sealed] is crypto_box_seal of the payload to device [deviceId]'s Curve25519 identity. */
class RvsdCopy(val deviceId: String, val sealed: ByteArray)

/**
 * `"RVSD" | 1 | count (1 byte) | count × (deviceId (16 bytes) ‖ len (u16, big-endian) ‖ sealed)`: 1 to 10 copies, each
 * sealed copy at least 48 bytes, no device twice, nothing left over, 8 KiB in all at most. The same rules as the
 * server's parser (server/src/crypto/storyAnswerEnvelope.ts) and the shared vectors.
 */
object Rvsd {
    val MAGIC = byteArrayOf(0x52, 0x56, 0x53, 0x44) // "RVSD"
    const val VERSION: Byte = 1
    const val MAX_COPIES = 10
    const val MAX_BYTES = 8192
    const val MIN_SEALED = 48

    fun isRvsd(bytes: ByteArray): Boolean = bytes.size >= 4 && bytes.copyOfRange(0, 4).contentEquals(MAGIC)

    fun encode(copies: List<RvsdCopy>): ByteArray {
        require(copies.size in 1..MAX_COPIES) { "1 to 10 copies" }
        require(copies.map { it.deviceId.lowercase() }.toSet().size == copies.size) { "a device once" }
        val size = 6 + copies.sumOf { 18 + it.sealed.size }
        require(size <= MAX_BYTES) { "an anonymous answer is at most 8 KiB" }
        val out = ByteBuffer.allocate(size)
        out.put(MAGIC).put(VERSION).put(copies.size.toByte())
        for (copy in copies) {
            require(copy.sealed.size in MIN_SEALED..0xffff) { "a sealed copy is at least 48 bytes" }
            val uuid = UUID.fromString(copy.deviceId)
            out.putLong(uuid.mostSignificantBits).putLong(uuid.leastSignificantBits)
            out.putShort(copy.sealed.size.toShort())
            out.put(copy.sealed)
        }
        return out.array()
    }

    /** The copies, or null when the container breaks any rule. */
    fun parse(bytes: ByteArray): List<RvsdCopy>? {
        if (bytes.size > MAX_BYTES || bytes.size < 6 || !isRvsd(bytes) || bytes[4] != VERSION) return null
        val count = bytes[5].toInt() and 0xff
        if (count < 1 || count > MAX_COPIES) return null
        val buffer = ByteBuffer.wrap(bytes, 6, bytes.size - 6)
        val out = ArrayList<RvsdCopy>(count)
        val seen = HashSet<String>()
        repeat(count) {
            if (buffer.remaining() < 18) return null
            val deviceId = UUID(buffer.long, buffer.long).toString()
            val len = buffer.short.toInt() and 0xffff
            if (len < MIN_SEALED || len > buffer.remaining()) return null
            if (!seen.add(deviceId)) return null
            val sealed = ByteArray(len).also { buffer.get(it) }
            out += RvsdCopy(deviceId, sealed)
        }
        if (buffer.hasRemaining()) return null
        return out
    }
}
