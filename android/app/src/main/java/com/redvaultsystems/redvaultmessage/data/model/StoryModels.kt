// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.put
import kotlin.math.abs
import kotlin.math.floor

// What a story shows, read from its decrypted details. Port of parseStoryMeta and friends in
// ui/src/lib/storyCrypto.ts: the same rules, so a story reads the same on every app. The rule of
// thumb for a newer app's story is the web's: a field or value this app doesn't know is left out and
// the rest of the story shows; a story whose own structure is wrong is refused whole.

/** The kinds this app can show. The web also makes `audio` stories; they're skipped here, as is any kind not yet invented. */
enum class StoryKind(val wire: String) {
    Image("image"),
    Video("video"),
    Text("text"),
    ;

    companion object {
        fun fromWire(value: String): StoryKind? = entries.firstOrNull { it.wire == value }
    }
}

/** Named colors only, so every app draws a story the same way (the web's tokens.css --story-*). */
enum class StoryColor(val wire: String) {
    White("white"), Black("black"), Red("red"), Orange("orange"), Yellow("yellow"),
    Green("green"), Teal("teal"), Blue("blue"), Purple("purple"), Pink("pink"),
    ;

    companion object {
        fun fromWire(value: String?): StoryColor? = entries.firstOrNull { it.wire == value }
    }
}

enum class StoryBackground(val wire: String) {
    Sunset("sunset"), Ocean("ocean"), Forest("forest"), Berry("berry"),
    Night("night"), Ember("ember"), Mint("mint"), Slate("slate"),
    ;

    companion object {
        fun fromWire(value: String?): StoryBackground? = entries.firstOrNull { it.wire == value }
    }
}

enum class StoryFont(val wire: String) {
    Classic("classic"), Modern("modern"), Typewriter("typewriter"), Strong("strong"),
    ;

    companion object {
        fun fromWire(value: String?): StoryFont? = entries.firstOrNull { it.wire == value }
    }
}

/** How an overlay moves on the web. Read so it can be passed on, but not drawn here: this app shows the overlay still, like apps from before it existed. */
enum class StoryAnimation(val wire: String) {
    Pulse("pulse"), Bounce("bounce"), Fade("fade"),
    ;

    companion object {
        fun fromWire(value: String?): StoryAnimation? = entries.firstOrNull { it.wire == value }
    }
}

/** Text or an emoji placed on a story; [x] and [y] are its centre as fractions of the frame. */
data class StoryOverlay(
    val isEmoji: Boolean,
    val text: String,
    val x: Double,
    val y: Double,
    /** 0.5 to 4, relative to the default size. */
    val scale: Double,
    /** Degrees, 0 until 360. */
    val rotation: Double,
    val color: StoryColor,
    /** Text only: a solid box behind it. */
    val boxed: Boolean,
    val font: StoryFont,
    val anim: StoryAnimation?,
)

/** A line of subtitles shown over a video between two times. */
class StoryCaption(val startMs: Long, val endMs: Long, val text: String)

/** Where a photo or video's ciphertext is and how to open it (the file key and header are base64). */
class StoryMediaRef(
    val key: String,
    val header: String,
    /** Lowercase hex SHA-256 of the ciphertext. */
    val sha256: String,
    val encryptedSize: Long,
    val chunkSize: Long,
    val mime: String,
    val width: Int,
    val height: Int,
    /** Video only. */
    val durationMs: Long?,
)

class StoryMeta(
    val kind: StoryKind,
    /** A text story's words. */
    val text: String?,
    val background: StoryBackground?,
    val font: StoryFont?,
    val overlays: List<StoryOverlay>,
    val media: StoryMediaRef?,
    /** Video only; empty for the others. */
    val captions: List<StoryCaption>,
    /** Shared with close friends only (shown with a green ring). */
    val closeFriends: Boolean,
    /** Interactive and informational stickers; empty when there are none (or none this app knows). */
    val stickers: List<StorySticker> = emptyList(),
)

/** The details of a story are wrong in a way that isn't just "something newer": the story is refused rather than half shown. */
class MalformedStoryException(message: String = "malformed story") : Exception(message)

object StoryLimits {
    const val MAX_OVERLAYS = 30
    const val MAX_OVERLAY_TEXT = 300
    const val MAX_STORY_TEXT = 700
    const val MAX_CAPTIONS = 60
    const val MAX_CAPTION_TEXT = 120
    const val MAX_CAPTION_END_MS = 10 * 60_000L

    /** Photos are cut down to this on their longest side before they're posted. */
    const val MAX_IMAGE_SIDE = 1920
    const val MAX_VIDEO_MS = 60_000L
    const val MAX_VIDEO_BYTES = 60L * 1024 * 1024

    /** The only chunk size a story's media is cut into (and the web refuses any other). */
    const val FILE_CHUNK_BYTES = 64L * 1024
}

/** The reading rules, kept apart from the crypto so they're checked without a libsodium. */
object StoryMetaParser {

    /** Validates decrypted details of a story the server says is [kind]; anything malformed is refused. */
    fun parse(raw: JsonElement, kind: StoryKind): StoryMeta {
        val r = raw as? JsonObject ?: throw MalformedStoryException()
        if (r["v"]?.number() != 1.0 || r["kind"]?.string() != kind.wire) throw MalformedStoryException()
        val overlayItems = (r["overlays"] as? JsonArray)?.take(StoryLimits.MAX_OVERLAYS) ?: emptyList()
        val overlays = overlayItems.map { parseOverlay(it) ?: throw MalformedStoryException() }
        val closeFriends = (r["closeFriends"] as? JsonPrimitive)?.let { !it.isString && it.content == "true" } == true
        val stickers = StoryStickerParser.parse(r["stickers"])
        return if (kind == StoryKind.Text) {
            val text = r["text"]?.string()
            if (text == null || jsTrim(text).isEmpty() || text.length > StoryLimits.MAX_STORY_TEXT) throw MalformedStoryException()
            StoryMeta(
                kind, text,
                StoryBackground.fromWire(r["background"]?.string()) ?: StoryBackground.Sunset,
                StoryFont.fromWire(r["font"]?.string()) ?: StoryFont.Classic,
                overlays, null, emptyList(), closeFriends, stickers,
            )
        } else {
            val media = parseMedia(r["media"] as? JsonObject ?: throw MalformedStoryException(), kind)
            val captions = if (kind == StoryKind.Image) emptyList() else parseCaptions(r["captions"])
            StoryMeta(kind, null, null, null, overlays, media, captions, closeFriends, stickers)
        }
    }

    private fun parseMedia(m: JsonObject, kind: StoryKind): StoryMediaRef {
        val key = m["key"]?.string()
        val header = m["header"]?.string()
        val sha256 = m["sha256"]?.string()
        if (!isBase64Ish(key, 64) || !isBase64Ish(header, 64) || sha256 == null || !SHA256_HEX.matches(sha256)) throw MalformedStoryException()
        val encryptedSize = m["encryptedSize"]?.positiveSafeInteger() ?: throw MalformedStoryException()
        val chunkSize = m["chunkSize"]?.positiveSafeInteger() ?: throw MalformedStoryException()
        val width = m["width"]?.positiveSafeInteger() ?: throw MalformedStoryException()
        val height = m["height"]?.positiveSafeInteger() ?: throw MalformedStoryException()
        val mime = m["mime"]?.string() ?: ""
        val allowed = if (kind == StoryKind.Image) IMAGE_MIME else VIDEO_MIME
        if (!allowed.matches(mime)) throw MalformedStoryException()
        val durationMs = if (kind == StoryKind.Video) m["durationMs"]?.positiveSafeInteger() ?: throw MalformedStoryException() else null
        return StoryMediaRef(key!!, header!!, sha256, encryptedSize, chunkSize, mime, width.toIntChecked(), height.toIntChecked(), durationMs)
    }

    private fun Long.toIntChecked(): Int = if (this <= Int.MAX_VALUE) toInt() else throw MalformedStoryException()

    private fun parseOverlay(o: JsonElement): StoryOverlay? {
        val r = o as? JsonObject ?: return null
        val type = r["type"]?.string()
        if (type != "text" && type != "emoji") return null
        val text = r["text"]?.string()
        if (text == null || text.isEmpty() || text.length > StoryLimits.MAX_OVERLAY_TEXT) return null
        val x = r["x"]?.fraction() ?: return null
        val y = r["y"]?.fraction() ?: return null
        val scale = r["scale"]?.number()?.takeIf { it.isFinite() }?.coerceIn(0.5, 4.0) ?: 1.0
        val rotation = r["rotation"]?.number()?.takeIf { it.isFinite() }?.let { ((it % 360) + 360) % 360 } ?: 0.0
        return StoryOverlay(
            isEmoji = type == "emoji",
            text = text,
            x = x,
            y = y,
            scale = scale,
            rotation = rotation,
            color = StoryColor.fromWire(r["color"]?.string()) ?: StoryColor.White,
            boxed = (r["boxed"] as? JsonPrimitive)?.let { !it.isString && it.content == "true" } == true,
            font = StoryFont.fromWire(r["font"]?.string()) ?: StoryFont.Classic,
            anim = StoryAnimation.fromWire(r["anim"]?.string()),
        )
    }

    /** Overlays of a draft: the ones that are well formed, the rest dropped (a draft is yours, so it is read kindly). */
    fun parseOverlays(raw: JsonElement?): List<StoryOverlay> =
        (raw as? JsonArray)?.take(StoryLimits.MAX_OVERLAYS)?.mapNotNull { parseOverlay(it) }.orEmpty()

    /** Keeps the lines that make sense (a real span, readable text), in time order, and drops the rest. */
    fun parseCaptions(raw: JsonElement?): List<StoryCaption> {
        val items = (raw as? JsonArray)?.take(StoryLimits.MAX_CAPTIONS) ?: return emptyList()
        val out = ArrayList<StoryCaption>()
        for (item in items) {
            val c = item as? JsonObject ?: continue
            val text = cleanStickerText(c["text"], StoryLimits.MAX_CAPTION_TEXT) ?: continue
            val start = c["startMs"]?.number()?.takeIf { it.isFinite() } ?: continue
            val end = c["endMs"]?.number()?.takeIf { it.isFinite() } ?: continue
            val startMs = jsRound(start)
            val endMs = jsRound(end)
            if (startMs < 0 || endMs <= startMs || endMs > StoryLimits.MAX_CAPTION_END_MS) continue
            out.add(StoryCaption(startMs, endMs, text))
        }
        return out.sortedBy { it.startMs }
    }

    /** The line to show at [timeMs], if any. */
    fun captionAt(captions: List<StoryCaption>, timeMs: Long): String? =
        captions.firstOrNull { timeMs >= it.startMs && timeMs < it.endMs }?.text

    private val SHA256_HEX = Regex("^[0-9a-f]{64}$")
    private val IMAGE_MIME = Regex("^image/(jpeg|png|webp)$")
    private val VIDEO_MIME = Regex("^video/(mp4|webm|quicktime)$")
    private val BASE64_ISH = Regex("^[A-Za-z0-9+/_=-]+$")

    private fun isBase64Ish(v: String?, max: Int) = v != null && v.isNotEmpty() && v.length <= max && BASE64_ISH.matches(v)
}

/** The author's story settings (synced between their devices, sealed so the server can't read them). */
data class StoryPrefs(
    /** User ids who see "Close friends" stories. */
    val closeFriends: Set<String>,
    /** User ids who never get your stories. */
    val hideFrom: Set<String>,
    /** User ids whose stories are muted (they stay out of the tray). */
    val muted: Set<String>,
    /** Let people see that you viewed their story. Off: this app never tells the server. */
    val shareViews: Boolean,
    /** New stories allow replies by default. */
    val allowReplies: Boolean,
) {
    companion object {
        val DEFAULT = StoryPrefs(emptySet(), emptySet(), emptySet(), shareViews = true, allowReplies = true)
    }
}

object StoryPrefsParser {
    private const val MAX_IDS = 5000
    private val UUID_LIKE = Regex("^[0-9a-fA-F-]{36}$")

    /** The JSON that gets sealed, with the web's key order. */
    fun toJson(p: StoryPrefs): String = kotlinx.serialization.json.buildJsonObject {
        put("closeFriends", JsonArray(p.closeFriends.map { JsonPrimitive(it) }))
        put("hideFrom", JsonArray(p.hideFrom.map { JsonPrimitive(it) }))
        put("muted", JsonArray(p.muted.map { JsonPrimitive(it) }))
        put("shareViews", p.shareViews)
        put("allowReplies", p.allowReplies)
    }.toString()

    /** Port of openStoryPrefs's reading: ids that aren't ids are dropped, everything but an explicit `false` is on. */
    fun parse(raw: JsonElement): StoryPrefs {
        val r = raw as? JsonObject ?: throw MalformedStoryException("your story settings can't be read")
        fun ids(key: String): Set<String> =
            (r[key] as? JsonArray).orEmpty().mapNotNull { it.string() }.filter { UUID_LIKE.matches(it) }.map { it.lowercase() }.distinct().take(MAX_IDS).toSet()
        fun flag(key: String): Boolean = !((r[key] as? JsonPrimitive)?.let { !it.isString && it.content == "false" } == true)
        return StoryPrefs(ids("closeFriends"), ids("hideFrom"), ids("muted"), flag("shareViews"), flag("allowReplies"))
    }
}

// --- JavaScript's rules, which the formats are defined by ---------------------------------------------------

/** A JSON number (not a number written as a string); null for anything else. */
internal fun JsonElement.number(): Double? = (this as? JsonPrimitive)?.takeIf { !it.isString }?.doubleOrNull

internal fun JsonElement.string(): String? = (this as? JsonPrimitive)?.takeIf { it.isString }?.content

/** A number from 0 to 1, as a position in the frame. */
private fun JsonElement.fraction(): Double? = number()?.takeIf { it.isFinite() && it >= 0.0 && it <= 1.0 }

/** Number.isSafeInteger(v) && v > 0 (so 65553.0 counts, as it does in JavaScript). */
private fun JsonElement.positiveSafeInteger(): Long? =
    number()?.takeIf { it.isFinite() && it == floor(it) && it > 0 && abs(it) <= 9007199254740991.0 }?.toLong()

/** Math.round: halves go up. */
internal fun jsRound(v: Double): Long = floor(v + 0.5).toLong()

/** What JavaScript's `\s` and trim() count as white space (not Kotlin's isWhitespace: it misses the no-break space and the byte order mark). */
internal fun isJsSpace(c: Char): Boolean = when (c) {
    '\t', '\n', '\u000B', '\u000C', '\r', ' ', '\u00A0', '\u1680', '\u2028', '\u2029', '\u202F', '\u205F', '\u3000', '\uFEFF' -> true
    else -> c in '\u2000'..'\u200A'
}

internal fun jsTrim(s: String): String = s.trim { isJsSpace(it) }

/**
 * Control characters and bidi overrides out, white space tidied: text that comes from a story is shown as plain
 * text only. Null when it's not text, empty after tidying, or longer than [max] (in UTF-16 units, like JavaScript).
 */
fun cleanStickerText(v: JsonElement?, max: Int): String? {
    val s = v?.string() ?: return null
    val sb = StringBuilder(s.length)
    for (c in s) {
        sb.append(if (c <= '\u001F' || c in '\u007F'..'\u009F' || c in '\u202A'..'\u202E' || c in '\u2066'..'\u2069') ' ' else c)
    }
    val collapsed = StringBuilder(sb.length)
    var inSpace = false
    for (c in sb) {
        if (isJsSpace(c)) {
            if (!inSpace) collapsed.append(' ')
            inSpace = true
        } else {
            collapsed.append(c)
            inSpace = false
        }
    }
    val t = jsTrim(collapsed.toString())
    return if (t.isNotEmpty() && t.length <= max) t else null
}
