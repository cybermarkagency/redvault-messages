package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.crypto.StoryCryptoException
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.StoryDto
import com.redvaultsystems.redvaultmessage.data.api.StoryFeed
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryMeta
import com.redvaultsystems.redvaultmessage.data.model.StoryMetaParser
import java.time.Instant

/** A story opened on this phone, with the key that opens its media. The key lives in memory only, and [wipe]s with the session. */
class Story(
    val id: String,
    val authorId: String,
    val kind: StoryKind,
    val meta: StoryMeta,
    val key: ByteArray,
    val createdAtMs: Long,
    val expiresAtMs: Long,
    val allowReplies: Boolean,
    val hasMedia: Boolean,
    /** Feed only: already seen (a shared view, or seen on this phone). */
    val seen: Boolean,
    /** Your own stories: how many friends shared that they saw it. */
    val viewCount: Int,
) {
    fun wipe() = key.fill(0)

    /** What a reply quotes: "Photo", "Video", or the start of a text story (the web's storyPreview). */
    fun preview(): String = when (kind) {
        StoryKind.Image -> "Photo"
        StoryKind.Video -> "Video"
        StoryKind.Text -> (meta.text ?: "").take(60)
    }
}

/** A friend as stories need them: who they are and the key their stories are sealed with. */
class StoryFriend(val userId: String, val handle: String, val avatarUrl: String?, val publicKey: ByteArray)

class AuthorStories(val author: StoryFriend, val stories: List<Story>) {
    val allSeen: Boolean get() = stories.all { it.seen }
}

class DecodedFeed(val own: List<Story>, val authors: List<AuthorStories>)

enum class SkipReason {
    /** A kind this app doesn't show (the web's `audio`, or one that doesn't exist yet). */
    UnsupportedKind,

    /** Not made by its author for you, tampered with, or bound to another story. */
    CannotOpen,

    /** Opens, but what's inside isn't a story this app can read. */
    Malformed,
}

sealed interface StoryDecoding {
    class Shown(val story: Story) : StoryDecoding
    class Skipped(val id: String, val reason: SkipReason) : StoryDecoding
}

/**
 * Opens what the feed holds. Port of decodeStory and decodeFeed in ui/src/lib/stories.ts. One story that can't
 * be shown (an unknown kind, a bad key, damaged details) is left out on its own: it never hides the author's
 * other stories, and nothing is half shown.
 */
class StoryDecoder(private val crypto: StoryCrypto) {

    fun decode(dto: StoryDto, authorPublicKey: ByteArray, viewerPrivateKey: ByteArray, seen: Boolean = false): StoryDecoding {
        val kind = StoryKind.fromWire(dto.kind) ?: return StoryDecoding.Skipped(dto.id, SkipReason.UnsupportedKind)
        var key: ByteArray? = null
        try {
            val createdAt = Instant.parse(dto.createdAt).toEpochMilli()
            val expiresAt = Instant.parse(dto.expiresAt).toEpochMilli()
            key = crypto.unwrapStoryKey(dto.id, Base64Codec.decode(dto.wrappedKey), authorPublicKey, viewerPrivateKey)
            val json = crypto.openStoryMeta(dto.id, key, Base64Codec.decode(dto.metaCiphertext), Base64Codec.decode(dto.metaNonce))
            val meta = StoryMetaParser.parse(ApiClient.json.parseToJsonElement(json), kind)
            val story = Story(dto.id, dto.authorId, kind, meta, key, createdAt, expiresAt, dto.allowReplies, dto.hasMedia, seen, dto.viewCount)
            key = null
            return StoryDecoding.Shown(story)
        } catch (e: StoryCryptoException) {
            return StoryDecoding.Skipped(dto.id, SkipReason.CannotOpen)
        } catch (e: Exception) {
            // Bad base64, a date that isn't one, details that aren't JSON, details that aren't a story.
            return StoryDecoding.Skipped(dto.id, SkipReason.Malformed)
        } finally {
            key?.fill(0)
        }
    }

    /**
     * Opens the whole feed. Stories that can't be shown are left out, and so are authors who aren't friends any
     * more (the key their stories must be sealed with is the one in [friends]). People with something new come first.
     */
    fun decodeFeed(feed: StoryFeed, selfPublicKey: ByteArray, selfPrivateKey: ByteArray, friends: Map<String, StoryFriend>, seenHere: Set<String>): DecodedFeed {
        val own = feed.own
            .mapNotNull { (decode(it, selfPublicKey, selfPrivateKey, seen = true) as? StoryDecoding.Shown)?.story }
            .sortedBy { it.createdAtMs }
        val authors = ArrayList<AuthorStories>()
        for (group in feed.authors) {
            val author = friends[group.authorId] ?: continue
            val stories = group.stories
                .mapNotNull { dto -> (decode(dto, author.publicKey, selfPrivateKey, seen = dto.seen || dto.id in seenHere) as? StoryDecoding.Shown)?.story }
                .filter { it.authorId == group.authorId }
                .sortedBy { it.createdAtMs }
            if (stories.isNotEmpty()) authors.add(AuthorStories(author, stories))
        }
        // The server's order otherwise: most recent first.
        return DecodedFeed(own, authors.sortedBy { it.allSeen })
    }
}
