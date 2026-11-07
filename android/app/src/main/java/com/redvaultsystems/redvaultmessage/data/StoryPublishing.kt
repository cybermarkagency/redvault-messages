package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.data.model.MalformedStoryException
import com.redvaultsystems.redvaultmessage.data.model.StickerLimits
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryCaption
import com.redvaultsystems.redvaultmessage.data.model.StoryMetaParser
import com.redvaultsystems.redvaultmessage.data.model.StoryOverlay
import com.redvaultsystems.redvaultmessage.data.model.StorySticker
import com.redvaultsystems.redvaultmessage.data.model.StoryStickerParser
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryLimits
import com.redvaultsystems.redvaultmessage.data.model.StoryMediaRef
import com.redvaultsystems.redvaultmessage.data.model.StoryPrefs
import com.redvaultsystems.redvaultmessage.data.model.jsTrim
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.File
import java.io.InputStream

/** Who a story is for. */
enum class StoryAudience(val wire: String) {
    /** Your friends, minus anyone you hide stories from. */
    Friends("friends"),

    /** Only the friends on your close friends list. */
    CloseFriends("close_friends"),

    /** Only the friends picked one by one (still never anyone you hide stories from). */
    Custom("custom"),
}

/** A photo or video ready to be encrypted: already cut down (a photo) or checked (a video), with how to read its bytes. */
class PreparedStoryMedia(
    val kind: StoryKind,
    val mime: String,
    val width: Int,
    val height: Int,
    /** Video only. */
    val durationMs: Long?,
    val sizeBytes: Long,
    /** A fresh stream of the file's bytes each time it's called. */
    val open: () -> InputStream,
)

/** What is being posted: a text story (words and a look) or a photo or video. */
class StoryDraft(
    val text: String?,
    val background: StoryBackground,
    val font: StoryFont,
    val media: PreparedStoryMedia?,
    val audience: StoryAudience,
    val allowReplies: Boolean,
    /** How long it stays up, in seconds (the server's default is 24 hours). */
    val ttlSeconds: Int?,
    /** Text and emoji placed on the story. */
    val overlays: List<StoryOverlay> = emptyList(),
    val stickers: List<StorySticker> = emptyList(),
    /** For [StoryAudience.Custom]: the friends picked. */
    val customIds: Set<String> = emptySet(),
    /** Subtitles of a video (made elsewhere; this app keeps them when it posts a draft that has them). */
    val captions: List<StoryCaption> = emptyList(),
)

enum class PostStage { Encrypting, Uploading }

/** Something the person can fix ("Write something first"). The message is safe to show. */
class StoryInputException(message: String) : Exception(message)

object StoryPublishing {

    /**
     * Who gets a story: your friends minus anyone you hide stories from; only close friends for a close-friends
     * story. Nobody who isn't a friend ever gets a key, whatever the ids say.
     */
    fun recipients(friends: Collection<StoryFriend>, prefs: StoryPrefs, audience: StoryAudience, customIds: Set<String> = emptySet()): List<StoryFriend> =
        friends.filter {
            it.userId !in prefs.hideFrom && when (audience) {
                StoryAudience.Friends -> true
                StoryAudience.CloseFriends -> it.userId in prefs.closeFriends
                StoryAudience.Custom -> it.userId in customIds
            }
        }

    /** The details of a text story, as the JSON that gets sealed. */
    fun textMeta(
        text: String,
        background: StoryBackground,
        font: StoryFont,
        closeFriends: Boolean,
        overlays: List<StoryOverlay> = emptyList(),
        stickers: List<StorySticker> = emptyList(),
    ): String {
        val trimmed = jsTrim(text)
        if (trimmed.isEmpty()) throw StoryInputException("Write something first.")
        if (trimmed.length > StoryLimits.MAX_STORY_TEXT) throw StoryInputException("That's too long for a story (at most ${StoryLimits.MAX_STORY_TEXT} characters).")
        return checked(
            StoryKind.Text,
            buildJsonObject {
                put("v", 1)
                put("kind", StoryKind.Text.wire)
                put("text", trimmed)
                put("background", background.wire)
                put("font", font.wire)
                put("overlays", overlaysJson(overlays))
                if (stickers.isNotEmpty()) put("stickers", stickersJson(stickers))
                put("closeFriends", closeFriends)
            }.toString(),
        )
    }

    /** The details are read back with the same rules every app reads with, so nothing is posted that another app would refuse. */
    private fun checked(kind: StoryKind, json: String): String {
        try {
            StoryMetaParser.parse(kotlinx.serialization.json.Json.parseToJsonElement(json), kind)
        } catch (e: MalformedStoryException) {
            throw StoryInputException("Something in this story can't be posted. Check the text and stickers.")
        }
        return json
    }

    internal fun overlaysJson(overlays: List<StoryOverlay>): JsonArray = JsonArray(
        overlays.take(StoryLimits.MAX_OVERLAYS).map { o ->
            buildJsonObject {
                put("type", if (o.isEmoji) "emoji" else "text")
                put("text", o.text)
                put("x", o.x)
                put("y", o.y)
                put("scale", o.scale)
                put("rotation", o.rotation)
                put("color", o.color.wire)
                put("boxed", o.boxed)
                put("font", o.font.wire)
                if (o.anim != null) put("anim", o.anim.wire)
            }
        },
    )

    private fun captionsJson(captions: List<StoryCaption>): JsonArray = JsonArray(
        captions.take(StoryLimits.MAX_CAPTIONS).map { c -> buildJsonObject { put("startMs", c.startMs); put("endMs", c.endMs); put("text", c.text) } },
    )

    private fun stickersJson(stickers: List<StorySticker>): JsonArray = JsonArray(stickers.take(StickerLimits.MAX_STICKERS).map(StoryStickerParser::toJson))

    /** The details of a photo or video story, with the file key and header the encrypted file was made with. */
    fun mediaMeta(
        kind: StoryKind,
        ref: StoryMediaRef,
        closeFriends: Boolean,
        overlays: List<StoryOverlay> = emptyList(),
        stickers: List<StorySticker> = emptyList(),
        captions: List<StoryCaption> = emptyList(),
    ): String {
        require(kind != StoryKind.Text)
        return checked(kind, buildJsonObject {
            put("v", 1)
            put("kind", kind.wire)
            put("overlays", overlaysJson(overlays))
            if (stickers.isNotEmpty()) put("stickers", stickersJson(stickers))
            if (captions.isNotEmpty() && kind == StoryKind.Video) put("captions", captionsJson(captions))
            put("closeFriends", closeFriends)
            put(
                "media",
                buildJsonObject {
                    put("key", ref.key)
                    put("header", ref.header)
                    put("sha256", ref.sha256)
                    put("encryptedSize", ref.encryptedSize)
                    put("chunkSize", ref.chunkSize)
                    put("mime", ref.mime)
                    put("width", ref.width)
                    put("height", ref.height)
                    if (ref.durationMs != null) put("durationMs", ref.durationMs)
                },
            )
        }.toString())
    }

    /** `POST /stories`: spells out `"mediaSize": null` for a text story, as the server's validation wants. */
    fun createBody(
        id: String,
        kind: StoryKind,
        metaCiphertext: String,
        metaNonce: String,
        mediaSize: Long?,
        audience: StoryAudience,
        allowReplies: Boolean,
        ttlSeconds: Int?,
        keys: List<Pair<String, String>>,
    ): JsonObject = buildJsonObject {
        put("id", id)
        put("kind", kind.wire)
        put("metaCiphertext", metaCiphertext)
        put("metaNonce", metaNonce)
        if (mediaSize != null) put("mediaSize", mediaSize) else put("mediaSize", JsonNull)
        put("audience", audience.wire)
        put("allowReplies", allowReplies)
        if (ttlSeconds != null) put("ttlSeconds", ttlSeconds)
        put(
            "keys",
            JsonArray(keys.map { (userId, wrapped) -> buildJsonObject { put("userId", userId); put("wrappedKey", wrapped) } }),
        )
    }
}

/** A story ready to send: the `POST /stories` body, and for a photo or video the encrypted file to upload afterwards. */
class BuiltStory(val id: String, val body: JsonObject, val encryptedMedia: File?)

/**
 * Everything about posting that needs no network: a fresh id and key, the details sealed with the key, the key sealed to you and
 * to each recipient, a photo or video encrypted into the upload file. Port of the first half of publishStory in
 * ui/src/lib/stories.ts. [StoriesRepository.publish] sends the result.
 */
class StoryBuilder(private val crypto: StoryCrypto, private val fileCrypto: FileCrypto) {
    fun build(
        selfUserId: String,
        selfPublicKey: ByteArray,
        selfPrivateKey: ByteArray,
        draft: StoryDraft,
        friends: Collection<StoryFriend>,
        prefs: StoryPrefs,
        uploadFile: File,
    ): BuiltStory {
        val id = crypto.newStoryId()
        val key = crypto.newStoryKey()
        try {
            val closeFriends = draft.audience == StoryAudience.CloseFriends
            val media = draft.media
            var mediaSize: Long? = null
            val metaJson = if (media == null) {
                StoryPublishing.textMeta(draft.text.orEmpty(), draft.background, draft.font, closeFriends, draft.overlays, draft.stickers)
            } else {
                uploadFile.parentFile?.mkdirs()
                val encrypted = media.open().use { fileCrypto.encryptFile(it, uploadFile) }
                mediaSize = encrypted.encryptedSize
                val ref = StoryMediaRef(
                    key = Base64Codec.encode(encrypted.key),
                    header = Base64Codec.encode(encrypted.header),
                    sha256 = encrypted.ciphertextSha256,
                    encryptedSize = encrypted.encryptedSize,
                    chunkSize = FileCrypto.CHUNK_BYTES.toLong(),
                    mime = media.mime,
                    width = media.width,
                    height = media.height,
                    durationMs = media.durationMs,
                )
                encrypted.key.fill(0)
                StoryPublishing.mediaMeta(media.kind, ref, closeFriends, draft.overlays, draft.stickers, draft.captions)
            }
            val sealed = crypto.sealStoryMeta(id, key, metaJson)
            val recipients = StoryPublishing.recipients(friends, prefs, draft.audience, draft.customIds)
            if (recipients.isEmpty() && draft.audience == StoryAudience.CloseFriends) throw StoryInputException("You have no close friends yet.")
            if (recipients.isEmpty() && draft.audience == StoryAudience.Custom) throw StoryInputException("Choose at least one friend.")
            val keys = (listOf(selfUserId to selfPublicKey) + recipients.map { it.userId to it.publicKey }).map { (userId, publicKey) ->
                userId to Base64Codec.encode(crypto.wrapStoryKey(id, key, publicKey, selfPrivateKey))
            }
            val body = StoryPublishing.createBody(
                id, media?.kind ?: StoryKind.Text, Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce),
                mediaSize, draft.audience, draft.allowReplies, draft.ttlSeconds, keys,
            )
            return BuiltStory(id, body, if (media != null) uploadFile else null)
        } catch (e: Exception) {
            uploadFile.delete()
            throw e
        } finally {
            key.fill(0)
        }
    }
}
