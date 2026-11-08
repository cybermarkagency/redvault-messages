// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryCaption
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryMetaParser
import com.redvaultsystems.redvaultmessage.data.model.StoryOverlay
import com.redvaultsystems.redvaultmessage.data.model.StorySticker
import com.redvaultsystems.redvaultmessage.data.model.StoryStickerParser
import com.redvaultsystems.redvaultmessage.data.model.jsTrim
import com.redvaultsystems.redvaultmessage.data.model.number
import com.redvaultsystems.redvaultmessage.data.model.string
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.File
import java.io.FileInputStream
import java.security.SecureRandom

// Unfinished stories, kept on this phone only (port of ui/src/lib/storyDrafts.ts, format version 2, so the web, desktop and this app mean the
// same thing by a draft; they are never shared between devices). Nothing readable is stored: the details are sealed with a key derived one-way
// from the account's private key, bound to the account and the draft's id, and each slide's photo or video is encrypted like any story media
// with its own key, which lives inside the sealed details. A draft never reaches the server. Ten per account, thirty days.

enum class SlideMode(val wire: String) { Media("media"), Text("text") }

/** One slide: a photo or video, or words on a background, and what's on it. */
class DraftSlide(
    val mode: SlideMode,
    val text: String,
    val background: StoryBackground,
    val font: StoryFont,
    val overlays: List<StoryOverlay>,
    val stickers: List<StorySticker>,
    /** Subtitles belong to a video; this app doesn't edit them but keeps what a draft from the web holds. */
    val captions: List<StoryCaption>,
    val media: PreparedStoryMedia?,
)

/** A whole unfinished story: its slides and the choices that apply to all of them. */
class DraftContent(
    val slides: List<DraftSlide>,
    val audience: StoryAudience,
    val customIds: List<String>,
    val allowReplies: Boolean,
    val ttlSeconds: Int,
)

class DraftSummary(val id: String, val updatedAt: Long, val slideCount: Int, val preview: String, val stickerCount: Int)

class StoryDraftStore(
    private val root: File,
    /** Where decrypted media of a draft being edited is put: the app's cache, emptied when the session ends. */
    private val workDir: File,
    private val crypto: StoryCrypto,
    private val fileCrypto: FileCrypto,
    private val now: () -> Long = System::currentTimeMillis,
) {
    private fun dirOf(identity: SelfIdentity) = File(root, identity.userId.lowercase())

    /** Your drafts, newest first. Ones that are too old, or can't be opened any more, are deleted. */
    fun list(identity: SelfIdentity): List<DraftSummary> {
        val dir = dirOf(identity)
        val out = ArrayList<DraftSummary>()
        for (file in dir.listFiles { f -> f.name.endsWith(DRAFT_EXT) }.orEmpty()) {
            val id = file.name.removeSuffix(DRAFT_EXT)
            val updatedAt = file.lastModified()
            val sealed = if (now() - updatedAt > DRAFT_KEEP_MS) null else unseal(identity, id, file.readBytes())
            val summary = (sealed?.get("summary") as? JsonObject)
            if (sealed == null || summary == null) {
                delete(identity, id)
                continue
            }
            out.add(
                DraftSummary(
                    id, updatedAt,
                    summary["slideCount"]?.number()?.toInt() ?: 1,
                    summary["preview"]?.string().orEmpty(),
                    summary["stickerCount"]?.number()?.toInt() ?: 0,
                ),
            )
        }
        return out.sortedByDescending { it.updatedAt }
    }

    /** Saves a draft (a new one, or over [id]). The oldest goes when there are already ten. Returns its id. */
    fun save(identity: SelfIdentity, content: DraftContent, id: String? = null): String {
        val draftId = id ?: newId()
        val slides = content.slides.take(MAX_SLIDES)
        require(slides.isNotEmpty()) { "There is nothing to save." }
        val dir = dirOf(identity).also { it.mkdirs() }
        // Replace the media of an old save of this draft.
        dir.listFiles { f -> f.name.startsWith("$draftId.m") }.orEmpty().forEach { it.delete() }
        val sealedSlides = slides.mapIndexed { i, slide ->
            val ref: JsonElement = slide.media?.let { media ->
                val target = File(dir, "$draftId.m$i")
                val encrypted = media.open().use { fileCrypto.encryptFile(it, target) }
                try {
                    buildJsonObject {
                        put("kind", media.kind.wire)
                        put("key", Base64Codec.encode(encrypted.key))
                        put("header", Base64Codec.encode(encrypted.header))
                        put("sha256", encrypted.ciphertextSha256)
                        put("encryptedSize", encrypted.encryptedSize)
                        put("chunkSize", FileCrypto.CHUNK_BYTES.toLong())
                        put("mime", media.mime)
                        put("width", media.width)
                        put("height", media.height)
                        if (media.durationMs != null) put("durationMs", media.durationMs)
                    }
                } finally {
                    encrypted.key.fill(0)
                }
            } ?: kotlinx.serialization.json.JsonNull
            buildJsonObject {
                put(
                    "content",
                    buildJsonObject {
                        put("mode", slide.mode.wire)
                        put("text", slide.text)
                        put("background", slide.background.wire)
                        put("font", slide.font.wire)
                        put("overlays", StoryPublishing.overlaysJson(slide.overlays))
                        put("stickers", JsonArray(slide.stickers.map(StoryStickerParser::toJson)))
                        put("captions", JsonArray(slide.captions.map { c -> buildJsonObject { put("startMs", c.startMs); put("endMs", c.endMs); put("text", c.text) } }))
                    },
                )
                put("media", ref)
            }
        }
        val first = slides[0]
        val preview = when {
            first.media != null -> if (first.media.kind == StoryKind.Video) "Video" else "Photo"
            else -> jsTrim(first.text).take(60)
        }
        val sealedJson = buildJsonObject {
            put("v", 2)
            put(
                "story",
                buildJsonObject {
                    put("audience", content.audience.wire)
                    put("customIds", JsonArray(content.customIds.map { JsonPrimitive(it) }))
                    put("allowReplies", content.allowReplies)
                    put("ttlSeconds", content.ttlSeconds)
                },
            )
            put("slides", JsonArray(sealedSlides))
            put(
                "summary",
                buildJsonObject {
                    put("slideCount", slides.size)
                    put("preview", preview)
                    put("stickerCount", slides.sumOf { it.stickers.size })
                },
            )
        }.toString()
        val sealed = crypto.sealDraft(sealedJson, identity.userId, draftId, identity.privateKey)
        val existing = dir.listFiles { f -> f.name.endsWith(DRAFT_EXT) }.orEmpty().filter { it.name != draftId + DRAFT_EXT }
        if (existing.size >= MAX_DRAFTS) existing.minByOrNull { it.lastModified() }?.let { delete(identity, it.name.removeSuffix(DRAFT_EXT)) }
        val file = File(dir, draftId + DRAFT_EXT)
        file.writeBytes(sealed.nonce + sealed.ciphertext)
        file.setLastModified(now())
        return draftId
    }

    /** Opens a draft for editing: its media is decrypted into the work folder. Null if it is gone or can't be opened. */
    fun load(identity: SelfIdentity, id: String): DraftContent? {
        val dir = dirOf(identity)
        val file = File(dir, id + DRAFT_EXT)
        if (!file.isFile) return null
        val sealed = unseal(identity, id, file.readBytes()) ?: return null
        val story = sealed["story"] as? JsonObject ?: return null
        val items = (sealed["slides"] as? JsonArray)?.take(MAX_SLIDES) ?: return null
        val slides = ArrayList<DraftSlide>()
        workDir.mkdirs()
        for ((i, item) in items.withIndex()) {
            val slide = item as? JsonObject ?: continue
            val c = slide["content"] as? JsonObject ?: continue
            val ref = slide["media"] as? JsonObject
            var media: PreparedStoryMedia? = null
            if (ref != null) {
                media = openMedia(dir, id, i, ref) ?: continue // a slide whose media can't be opened (or is a kind this app can't show) is left out
            }
            val wantsMedia = c["mode"]?.string() == "media"
            if (wantsMedia && media == null) continue
            slides.add(
                DraftSlide(
                    mode = if (wantsMedia) SlideMode.Media else SlideMode.Text,
                    text = c["text"]?.string().orEmpty(),
                    background = StoryBackground.fromWire(c["background"]?.string()) ?: StoryBackground.Sunset,
                    font = StoryFont.fromWire(c["font"]?.string()) ?: StoryFont.Classic,
                    overlays = StoryMetaParser.parseOverlays(c["overlays"]),
                    stickers = StoryStickerParser.parse(c["stickers"]),
                    captions = StoryMetaParser.parseCaptions(c["captions"]),
                    media = if (wantsMedia) media else null,
                ),
            )
        }
        if (slides.isEmpty()) return null
        val audience = StoryAudience.entries.firstOrNull { it.wire == story["audience"]?.string() } ?: StoryAudience.Friends
        return DraftContent(
            slides,
            audience,
            (story["customIds"] as? JsonArray).orEmpty().mapNotNull { it.string() },
            story["allowReplies"].let { !(it is JsonPrimitive && !it.isString && it.content == "false") },
            story["ttlSeconds"]?.number()?.toInt() ?: 86_400,
        )
    }

    fun delete(identity: SelfIdentity, id: String) {
        val dir = dirOf(identity)
        File(dir, id + DRAFT_EXT).delete()
        dir.listFiles { f -> f.name.startsWith("$id.m") }.orEmpty().forEach { it.delete() }
    }

    private fun openMedia(dir: File, id: String, index: Int, ref: JsonObject): PreparedStoryMedia? {
        val source = File(dir, "$id.m$index")
        if (!source.isFile) return null
        val kind = StoryKind.fromWire(ref["kind"]?.string().orEmpty())?.takeIf { it != StoryKind.Text } ?: return null
        return try {
            val target = File(workDir, "draft-$id-$index.bin")
            target.outputStream().buffered().use { out ->
                source.inputStream().buffered().use { input ->
                    fileCrypto.decrypt(
                        input, out,
                        Base64Codec.decode(ref["key"]?.string().orEmpty()),
                        Base64Codec.decode(ref["header"]?.string().orEmpty()),
                        ref["encryptedSize"]?.number()?.toLong() ?: return null,
                        ref["sha256"]?.string().orEmpty(),
                    )
                }
            }
            PreparedStoryMedia(
                kind,
                ref["mime"]?.string().orEmpty(),
                ref["width"]?.number()?.toInt() ?: 1,
                ref["height"]?.number()?.toInt() ?: 1,
                ref["durationMs"]?.number()?.toLong(),
                target.length(),
            ) { FileInputStream(target) }
        } catch (e: Exception) {
            null
        }
    }

    private fun unseal(identity: SelfIdentity, id: String, bytes: ByteArray): JsonObject? {
        if (bytes.size <= NONCE_BYTES) return null
        val json = crypto.openDraft(bytes.copyOfRange(NONCE_BYTES, bytes.size), bytes.copyOfRange(0, NONCE_BYTES), identity.userId, id, identity.privateKey) ?: return null
        val parsed = runCatching { Json.parseToJsonElement(json) as? JsonObject }.getOrNull() ?: return null
        if (parsed["v"]?.number() != 2.0 || (parsed["slides"] as? JsonArray)?.isNotEmpty() != true || parsed["story"] !is JsonObject) return null
        return parsed
    }

    private fun newId(): String = ByteArray(12).also { SecureRandom().nextBytes(it) }.joinToString("") { "%02x".format(it.toInt() and 0xff) }

    companion object {
        const val MAX_DRAFTS = 10
        const val MAX_SLIDES = 10
        const val DRAFT_KEEP_MS = 30L * 24 * 3600_000
        private const val DRAFT_EXT = ".draft"
        private const val NONCE_BYTES = 24
    }
}
