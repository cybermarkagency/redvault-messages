// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject

// What a private channel's post (or suggestion) holds once it's opened (Part C1,
// docs/signal-integration/10-part-c1-private-channels.md):
//
//   {"v":1,"body":<text, at most 4000 UTF-16 units>,"content":<the post's content or null>}
//
// `content` follows the plain post request's own shapes, so converting between the
// two formats is a copy: {"poll":{question, options, multiple?, quiz?:{correct, explanation?}}},
// {"checklist":{title, items, collaborative?}}, or {"media":[item…]}, where each media item
// carries its upload's id and everything the server no longer sees: the real kind, type,
// name, size and dimensions, and the FileCrypto secretstream key, header, encrypted size
// and ciphertext SHA-256 (the same names as a chat file's descriptor). The full layout is in
// docs/signal-integration/10-part-c1-android-notes.md. Everything read from a post is
// untrusted: it's checked here before the screens see it.

@Serializable
data class SealedQuiz(val correct: Int, val explanation: String? = null)

@Serializable
data class SealedPoll(val question: String, val options: List<String>, val multiple: Boolean = false, val quiz: SealedQuiz? = null)

@Serializable
data class SealedChecklist(val title: String, val items: List<String>, val collaborative: Boolean = false)

/** One encrypted upload in a post: its id in the post's media list, and how to open and show it. */
@Serializable
data class SealedMediaItem(
    val id: String,
    /** "photo", "video", "file", "audio", "voice" or "round". */
    val kind: String,
    val mime: String,
    val filename: String? = null,
    /** The real (plaintext) size. */
    val size: Long,
    val durationMs: Long? = null,
    val width: Int? = null,
    val height: Int? = null,
    /** base64: the file's own secretstream key. */
    val fileKey: String,
    /** base64: the secretstream header. */
    val header: String,
    val encryptedSize: Long,
    /** Lowercase hex SHA-256 of the encrypted bytes. */
    val ciphertextSha256: String,
)

@Serializable
data class SealedContent(
    val poll: SealedPoll? = null,
    val checklist: SealedChecklist? = null,
    val media: List<SealedMediaItem>? = null,
)

@Serializable
data class SealedPostPlain(val v: Int, val body: String, val content: SealedContent?)

object ChannelSealedPosts {
    const val VERSION = 1
    const val MAX_BODY = 4000
    private const val MAX_OPTIONS = 10
    private const val MAX_ITEMS = 30
    private const val MAX_MEDIA = 20
    private val KINDS = setOf("photo", "video", "file", "audio", "voice", "round")
    private val UUID_RE = Regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", RegexOption.IGNORE_CASE)
    private val HEX64_RE = Regex("^[0-9a-f]{64}$")

    // Written with "content": null when there's none (as the spec spells it), and no other nulls or defaults.
    private val writer = Json { encodeDefaults = false; explicitNulls = true }
    private val writerInner = Json { encodeDefaults = false; explicitNulls = false }
    private val reader = Json { ignoreUnknownKeys = true; explicitNulls = false }

    fun encode(plain: SealedPostPlain): String {
        require(plain.body.length <= MAX_BODY) { "A post is at most $MAX_BODY characters" }
        // Nested nulls are left out; only the top-level content is written as null.
        val content = plain.content?.let { writerInner.encodeToJsonElement(SealedContent.serializer(), it) }
        val obj = buildJsonObject {
            put("v", JsonPrimitive(plain.v))
            put("body", JsonPrimitive(plain.body))
            put("content", content ?: JsonNull)
        }
        return writer.encodeToString(JsonObject.serializer(), obj)
    }

    /** The opened post, or null when it isn't one this app can show (another version, or malformed). */
    fun decode(text: String): SealedPostPlain? {
        val plain = runCatching { reader.decodeFromString(SealedPostPlain.serializer(), text) }.getOrNull() ?: return null
        if (plain.v != VERSION || plain.body.length > MAX_BODY) return null
        val content = plain.content ?: return plain
        val poll = content.poll
        if (poll != null) {
            if (poll.options.size !in 2..MAX_OPTIONS) return null
            if (poll.quiz != null && (poll.multiple || poll.quiz.correct !in poll.options.indices)) return null
        }
        if (content.checklist != null && content.checklist.items.size !in 1..MAX_ITEMS) return null
        val media = content.media
        if (media != null) {
            if (media.size > MAX_MEDIA) return null
            for (m in media) {
                if (!UUID_RE.matches(m.id) || m.kind !in KINDS || m.size < 0 || m.encryptedSize < 0) return null
                if (!HEX64_RE.matches(m.ciphertextSha256)) return null
                if ((m.filename?.length ?: 0) > 1024 || m.mime.length > 200) return null
            }
        }
        return plain
    }
}
