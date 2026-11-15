// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileDecryptException
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.crypto.StoryCryptoException
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.StoriesApi
import com.redvaultsystems.redvaultmessage.data.api.StoryAvailability
import com.redvaultsystems.redvaultmessage.data.api.StoryConfigParser
import com.redvaultsystems.redvaultmessage.data.api.StoryFeedParser
import com.redvaultsystems.redvaultmessage.data.api.StoryKeysRequest
import com.redvaultsystems.redvaultmessage.data.api.StoryWrappedKeyDto
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryLimits
import com.redvaultsystems.redvaultmessage.data.model.StoryMediaRef
import com.redvaultsystems.redvaultmessage.data.model.StoryPrefs
import com.redvaultsystems.redvaultmessage.data.model.StoryPrefsParser
import com.redvaultsystems.redvaultmessage.data.api.DELIVERY_DEVICE_KEYS
import com.redvaultsystems.redvaultmessage.data.api.StoryAccessRequest
import com.redvaultsystems.redvaultmessage.devicekeys.StoryDeviceKeys
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.put
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.asRequestBody
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException

/** The author's story settings as this phone could read them. */
class LoadedStoryPrefs(
    val prefs: StoryPrefs,
    /**
     * False when settings exist on the server but can't be opened here. Then nothing is posted (the hidden-from list
     * is unknown) and no view is announced (whether to share them is unknown).
     */
    val readable: Boolean,
    /** Whether stories that leave your story are kept for you (the archive). */
    val archiveEnabled: Boolean = false,
    /** The version the server holds; the next save must name it. */
    val revision: Int = 0,
)

/** Someone's highlight as this phone can read it: only the stories it holds a key for, and a title if it can see the cover. */
class HighlightView(val id: String, val ownerId: String, val title: String?, val coverStoryId: String, val stories: List<Story>)

/**
 * Stories on this phone: what the server says, opening the feed, fetching and opening media, posting. The crypto is
 * [StoryCrypto] and [FileCrypto] (libsodium only); the reading rules are [StoryDecoder] and `data/model/StoryModels.kt`;
 * the design is docs/stories.md. Only ciphertext and ids go to the server, and only to our own API (the HTTP client
 * refuses any other host). Decrypted media is kept in memory, or for a video in this app's private cache, which
 * [clearCache] empties when the session ends.
 */
class StoriesRepository(
    private val api: StoriesApi,
    private val appApi: ApiService,
    private val crypto: StoryCrypto,
    private val fileCrypto: FileCrypto,
    private val client: OkHttpClient,
    private val apiBase: String,
    cacheDir: File,
    private val seen: StorySeenStore,
) {
    private val decoder = StoryDecoder(crypto)
    private val builder = StoryBuilder(crypto, fileCrypto)
    private val mediaDir = File(cacheDir, "stories")

    /** Whether Stories are on, from the one route that answers while they're off. Nothing else is asked before this says on. */
    suspend fun availability(): StoryAvailability =
        try {
            StoryConfigParser.parse(apiCall(ApiClient.json) { api.config() })
        } catch (e: ApiException) {
            // A server with no Stories at all answers 404: nothing to show.
            if (e.status == 404) StoryAvailability.Off else throw e
        }

    suspend fun loadPrefs(identity: SelfIdentity): LoadedStoryPrefs {
        val dto = apiCall(ApiClient.json) { api.prefs() }
        val ciphertext = dto.prefsCiphertext
        val nonce = dto.prefsNonce
        if (ciphertext == null || nonce == null) return LoadedStoryPrefs(StoryPrefs.DEFAULT, readable = true, archiveEnabled = dto.archiveEnabled, revision = dto.revision)
        return try {
            val json = crypto.openStoryPrefs(Base64Codec.decode(ciphertext), Base64Codec.decode(nonce), identity.privateKey)
            LoadedStoryPrefs(StoryPrefsParser.parse(ApiClient.json.parseToJsonElement(json)), readable = true, archiveEnabled = dto.archiveEnabled, revision = dto.revision)
        } catch (e: Exception) {
            // The web app carries on with the defaults here. This app doesn't: the defaults would mean "hide from nobody"
            // and "tell people I looked".
            LoadedStoryPrefs(StoryPrefs(emptySet(), emptySet(), emptySet(), shareViews = false, allowReplies = true), readable = false, archiveEnabled = dto.archiveEnabled, revision = dto.revision)
        }
    }

    /** Saves the settings, sealed with a key only this account's devices hold. A 409 (another device saved first) is for the caller to reload and retry. */
    suspend fun savePrefs(identity: SelfIdentity, prefs: StoryPrefs, archiveEnabled: Boolean, revision: Int): LoadedStoryPrefs {
        val sealed = crypto.sealStoryPrefs(StoryPrefsParser.toJson(prefs), identity.privateKey)
        val body = kotlinx.serialization.json.buildJsonObject {
            put("prefsCiphertext", Base64Codec.encode(sealed.ciphertext))
            put("prefsNonce", Base64Codec.encode(sealed.nonce))
            put("archiveEnabled", archiveEnabled)
            put("revision", revision)
        }
        val saved = apiCall(ApiClient.json) { api.putPrefs(body) }
        return LoadedStoryPrefs(prefs, readable = true, archiveEnabled = saved.archiveEnabled, revision = saved.revision)
    }

    /** Takes [userId]'s keys to your stories away: all of them, or only [storyIds]. What they already opened stays opened. */
    suspend fun revokeAccess(userId: String, storyIds: List<String>? = null) {
        val body = kotlinx.serialization.json.buildJsonObject {
            put("userId", userId)
            if (storyIds != null) put("storyIds", kotlinx.serialization.json.JsonArray(storyIds.map { kotlinx.serialization.json.JsonPrimitive(it) }))
        }
        apiCall(ApiClient.json) { api.revoke(body) }
    }

    /** Seals one of your story's keys to a friend who didn't have it (they joined a list, or the story's audience grew). */
    suspend fun addKey(identity: SelfIdentity, story: Story, friend: StoryFriend) {
        val wrapped = StoryWrappedKeyDto(friend.userId, Base64Codec.encode(crypto.wrapStoryKey(story.id, story.key, friend.publicKey, identity.privateKey)))
        apiCall(ApiClient.json) { api.addKeys(story.id, StoryKeysRequest(listOf(wrapped))) }
    }

    /** Your stories that left your story (kept for you, or in a highlight), opened with your own key. */
    suspend fun archive(identity: SelfIdentity, deviceKeys: StoryDeviceKeys? = null): List<Story> {
        val dtos = StoryFeedParser.parseStories(apiCall(ApiClient.json) { api.archive() })
        return withContext(Dispatchers.Default) {
            dtos.mapNotNull { (decoder.decode(it, identity.publicKey, identity.privateKey, seen = true, deviceKeys = deviceKeys) as? StoryDecoding.Shown)?.story }.sortedByDescending { it.createdAtMs }
        }
    }

    /** Someone's highlights as this phone can see them ([ownerPublicKey] is theirs, or yours for your own). */
    suspend fun highlights(identity: SelfIdentity, ownerId: String, ownerPublicKey: ByteArray, deviceKeys: StoryDeviceKeys? = null): List<HighlightView> {
        val dtos = StoryFeedParser.parseHighlights(apiCall(ApiClient.json) { api.highlights(ownerId) })
        return withContext(Dispatchers.Default) {
            dtos.mapNotNull { h ->
                val stories = h.stories
                    .mapNotNull { (decoder.decode(it, ownerPublicKey, identity.privateKey, seen = true, deviceKeys = deviceKeys) as? StoryDecoding.Shown)?.story }
                    .filter { it.authorId == h.ownerId }
                if (stories.isEmpty()) return@mapNotNull null
                val cover = stories.firstOrNull { it.id == h.coverStoryId }
                val title = cover?.let { crypto.openHighlightTitle(Base64Codec.decode(h.titleCiphertext), Base64Codec.decode(h.titleNonce), it.id, it.key) }
                HighlightView(h.id, h.ownerId, title, cover?.id ?: stories[0].id, stories)
            }
        }
    }

    /** Creates a highlight (or with [highlightId] replaces one's title, cover and stories). The title is sealed with the cover's key. */
    suspend fun saveHighlight(highlightId: String?, title: String, stories: List<Story>, cover: Story): String {
        val sealed = crypto.sealHighlightTitle(title, cover.id, cover.key)
        val body = kotlinx.serialization.json.buildJsonObject {
            put("titleCiphertext", Base64Codec.encode(sealed.ciphertext))
            put("titleNonce", Base64Codec.encode(sealed.nonce))
            put("coverStoryId", cover.id)
            put("storyIds", kotlinx.serialization.json.JsonArray(stories.map { kotlinx.serialization.json.JsonPrimitive(it.id) }))
        }
        return if (highlightId != null) apiCall(ApiClient.json) { api.updateHighlight(highlightId, body) }.id else apiCall(ApiClient.json) { api.createHighlight(body) }.id
    }

    suspend fun deleteHighlight(highlightId: String) {
        apiCall(ApiClient.json) { api.deleteHighlight(highlightId) }
    }

    /** Reports a story you can see. [evidence] is only what the reporter chose to include. */
    suspend fun reportStory(storyId: String, reason: String, evidence: String?) {
        val body = kotlinx.serialization.json.buildJsonObject {
            put("reason", reason)
            if (!evidence.isNullOrBlank()) put("evidence", evidence)
        }
        apiCall(ApiClient.json) { api.reportStory(storyId, body) }
    }

    /** Friends with the key their stories are sealed with. A friend whose chat shows a different key is left out. */
    suspend fun loadFriends(chatKeys: Map<String, String>): Map<String, StoryFriend> {
        val out = LinkedHashMap<String, StoryFriend>()
        for (f in apiCall(ApiClient.json) { appApi.listFriends() }) {
            val publicKey = runCatching { Base64Codec.decode(f.publicKey) }.getOrNull() ?: continue
            val chatKey = chatKeys[f.userId]
            if (chatKey != null && chatKey != f.publicKey) continue
            out[f.userId] = StoryFriend(f.userId, f.username, f.avatarUrl, publicKey)
        }
        return out
    }

    /**
     * [deviceKeys]: Part D's store (null on a phone without device keys). Device-keys stories open with the key their
     * author sent; keys of stories the server lists nowhere any more are forgotten here ([forgetUnlisted]).
     */
    suspend fun loadFeed(identity: SelfIdentity, friends: Map<String, StoryFriend>, deviceKeys: StoryDeviceKeys? = null): DecodedFeed {
        val feed = StoryFeedParser.parse(apiCall(ApiClient.json) { api.feed() })
        if (deviceKeys != null) forgetUnlisted(identity, (feed.own + feed.authors.flatMap { it.stories }).mapTo(HashSet()) { it.id.lowercase() }, deviceKeys)
        return withContext(Dispatchers.Default) { decoder.decodeFeed(feed, identity.publicKey, identity.privateKey, friends, seen.ids(identity.userId), deviceKeys) }
    }

    @Volatile private var unlistedCheckedAt = 0L

    /**
     * Part D's retention of K ([forgetUnlistedStoryKeys]), at most every [UNLISTED_CHECK_MS]: the archive (your own
     * stories) and each author's highlights are asked about keys the feed ([listed]) no longer names. Returns the story
     * ids forgotten.
     */
    suspend fun forgetUnlisted(identity: SelfIdentity, listed: Set<String>, deviceKeys: StoryDeviceKeys, now: Long = System.currentTimeMillis()): Set<String> {
        if (now - unlistedCheckedAt < UNLISTED_CHECK_MS) return emptySet()
        val gone = forgetUnlistedStoryKeys(
            listed, deviceKeys,
            archive = { StoryFeedParser.parseStories(apiCall(ApiClient.json) { api.archive() }).map { it.id } },
            highlights = { owner -> StoryFeedParser.parseHighlights(apiCall(ApiClient.json) { api.highlights(owner) }).flatMap { h -> h.stories.map { it.id } } },
        ) ?: return emptySet()
        unlistedCheckedAt = now
        return gone
    }

    // --- Seeing ------------------------------------------------------------------------------------------------

    /** The ring goes grey on this phone; the author learns you looked only if [announce] (you share views). */
    suspend fun markSeen(identity: SelfIdentity, story: Story, announce: Boolean) {
        seen.remember(identity.userId, story.id)
        if (announce) runCatching { apiCall(ApiClient.json) { api.markViewed(story.id) } }
    }

    suspend fun delete(storyId: String, deviceKeys: StoryDeviceKeys? = null) {
        apiCall(ApiClient.json) { api.delete(storyId) }
        deviceKeys?.forget(storyId.lowercase())
    }

    suspend fun viewers(storyId: String): List<String> = apiCall(ApiClient.json) { api.viewers(storyId) }

    // --- Media -------------------------------------------------------------------------------------------------

    /** The decrypted photo, in memory. */
    suspend fun photo(story: Story): ByteArray {
        val media = checkMedia(story, StoryKind.Image, MAX_PHOTO_CIPHERTEXT_BYTES)
        photoCache[story.id]?.let { return it }
        val out = ByteArrayOutputStream(media.encryptedSize.toInt())
        withContext(Dispatchers.IO) { fetch(story, media, out) }
        return out.toByteArray().also { remember(story.id, it) }
    }

    /** The decrypted video, as a file in this app's private cache. */
    suspend fun video(story: Story): File = playable(story, StoryKind.Video, MAX_VIDEO_CIPHERTEXT_BYTES, "video")

    /** The decrypted voice recording, as a file in this app's private cache. */
    suspend fun audio(story: Story): File = playable(story, StoryKind.Audio, MAX_AUDIO_CIPHERTEXT_BYTES, "audio")

    private suspend fun playable(story: Story, kind: StoryKind, maxBytes: Long, extension: String): File {
        val media = checkMedia(story, kind, maxBytes)
        val file = File(mediaDir, "${story.id}.$extension")
        if (file.isFile) return file
        return withContext(Dispatchers.IO) {
            mediaDir.mkdirs()
            val partial = File(mediaDir, "${story.id}.partial")
            try {
                partial.outputStream().buffered().use { fetch(story, media, it) }
                if (!partial.renameTo(file)) throw UserFacingException("Couldn't open that story.")
                file
            } finally {
                partial.delete()
            }
        }
    }

    private fun checkMedia(story: Story, kind: StoryKind, maxBytes: Long): StoryMediaRef {
        val media = story.meta.media
        if (story.kind != kind || media == null) throw UserFacingException("This story has no ${kind.wire}.")
        // The web refuses any other chunk size too; and a friend's app picks the size it claims, so it's capped here.
        if (media.chunkSize != StoryLimits.FILE_CHUNK_BYTES || media.encryptedSize > maxBytes) throw UserFacingException("This story can't be opened on this version of the app.")
        return media
    }

    private fun fetch(story: Story, media: StoryMediaRef, output: java.io.OutputStream) {
        val request = Request.Builder().url("${apiBase}stories/${story.id}/media").get().build()
        try {
            client.newCall(request).execute().use { response ->
                if (response.code == 401) throw ApiException(401, "unauthorized", "Signed out.")
                if (!response.isSuccessful) throw UserFacingException("This story isn't available any more.")
                val body = response.body ?: throw UserFacingException("This story isn't available any more.")
                fileCrypto.decrypt(
                    body.byteStream(),
                    output,
                    Base64Codec.decode(media.key),
                    Base64Codec.decode(media.header),
                    media.encryptedSize,
                    media.sha256,
                )
            }
        } catch (e: FileDecryptException) {
            throw UserFacingException("This story can't be opened: it was damaged or changed on the way.")
        } catch (e: IOException) {
            throw ApiException(0, "network_error", "Can't reach the server. Check your connection.")
        } catch (e: IllegalArgumentException) {
            throw UserFacingException("This story can't be opened.")
        }
    }

    private val photoCache = object : LinkedHashMap<String, ByteArray>(8, 0.75f, true) {}
    private var photoCacheBytes = 0L

    @Synchronized
    private fun remember(id: String, bytes: ByteArray) {
        photoCache[id]?.let { photoCacheBytes -= it.size }
        photoCache[id] = bytes
        photoCacheBytes += bytes.size
        val it = photoCache.entries.iterator()
        while (photoCacheBytes > PHOTO_CACHE_BYTES && it.hasNext()) {
            val oldest = it.next()
            if (oldest.key == id) continue
            photoCacheBytes -= oldest.value.size
            it.remove()
        }
    }

    /** Decrypted media doesn't outlive the session. */
    @Synchronized
    fun clearCache() {
        photoCache.values.forEach { it.fill(0) }
        photoCache.clear()
        photoCacheBytes = 0
        mediaDir.deleteRecursively()
    }

    // --- Posting -----------------------------------------------------------------------------------------------

    /**
     * Encrypts and posts a story: a fresh id and key, the details sealed with the key, the key sealed to you and to each
     * recipient, then (for a photo or video) the encrypted file. If the file can't be uploaded the story is deleted again.
     * Returns the story's id.
     */
    suspend fun publish(
        identity: SelfIdentity,
        draft: StoryDraft,
        friends: Collection<StoryFriend>,
        prefs: StoryPrefs,
        onStage: (PostStage) -> Unit,
        deviceKeys: StoryDeviceKeys? = null,
    ): String = withContext(Dispatchers.IO) {
        val upload = File(mediaDir, "upload-${System.nanoTime()}")
        var built: BuiltStory? = null
        try {
            onStage(PostStage.Encrypting)
            // Part D: on device keys when this phone has them and every viewer has a device list; otherwise as before.
            val viewers = StoryPublishing.recipients(friends, prefs, draft.audience, draft.customIds).map { it.userId }
            val onDeviceKeys = deviceKeys != null && deviceKeys.ready() && deviceKeys.allHaveDevices(viewers)
            val story = builder.build(identity.userId, identity.publicKey, identity.privateKey, draft, friends, prefs, upload, onDeviceKeys)
            built = story
            onStage(PostStage.Uploading)
            val created = if (onDeviceKeys) {
                postOnDeviceKeys(story, draft, deviceKeys!!)
            } else {
                apiCall(ApiClient.json) { api.create(story.body) }
            }
            if (story.encryptedMedia != null) {
                try {
                    uploadMedia(story.id, story.encryptedMedia)
                } catch (e: Exception) {
                    runCatching { apiCall(ApiClient.json) { api.delete(story.id) } }
                    if (onDeviceKeys) deviceKeys?.forget(story.id)
                    throw e
                }
            }
            created.id
        } finally {
            built?.wipe()
            upload.delete()
        }
    }

    /**
     * Part D: keeps K in this phone's story-key store, creates the story with its viewers, then sends K to every viewer
     * device and this account's other devices (in batches, in order, through the Tor queue). Whoever it couldn't reach is
     * remembered and tried again with the late friends. The media goes up after (the key usually arrives first).
     */
    private suspend fun postOnDeviceKeys(story: BuiltStory, draft: StoryDraft, deviceKeys: StoryDeviceKeys): com.redvaultsystems.redvaultmessage.data.api.StoryCreatedDto {
        val key = story.key ?: throw IllegalStateException("a device-keys story has its key")
        val ttlMs = (draft.ttlSeconds ?: com.redvaultsystems.redvaultmessage.data.api.StoryConfigParser.DEFAULT_TTL_SECONDS) * 1000L
        deviceKeys.rememberOwnKey(story.id, key, System.currentTimeMillis() + ttlMs)
        val created = try {
            apiCall(ApiClient.json) { api.create(story.body) }
        } catch (e: Exception) {
            deviceKeys.forget(story.id)
            throw e
        }
        val expiresAt = created.expiresAt?.let { runCatching { java.time.Instant.parse(it).toEpochMilli() }.getOrNull() }
            ?: (System.currentTimeMillis() + ttlMs)
        deviceKeys.rememberOwnKey(story.id, key, expiresAt)
        try {
            deviceKeys.deliverKey(story.id, key, expiresAt, story.viewers)
        } catch (e: ApiException) {
            if (e.status == 401) throw e
            deviceKeys.owe(story.id, story.viewers, expiresAt)
        } catch (e: com.redvaultsystems.redvaultmessage.devicekeys.EngineException) {
            deviceKeys.owe(story.id, story.viewers, expiresAt)
        }
        return created
    }

    private fun uploadMedia(storyId: String, file: File) {
        val request = Request.Builder()
            .url("${apiBase}stories/$storyId/media")
            .put(file.asRequestBody("application/octet-stream".toMediaType()))
            .build()
        try {
            client.newCall(request).execute().use { response ->
                if (response.code == 401) throw ApiException(401, "unauthorized", "Signed out.")
                if (!response.isSuccessful) throw UserFacingException(if (response.code == 413) "That file is too large for a story." else "Couldn't upload that. Try again.")
            }
        } catch (e: IOException) {
            throw ApiException(0, "network_error", "Can't reach the server. Check your connection.")
        }
    }

    /**
     * Your "friends" stories and friends who have no key for them yet (added since): seal the key to them, except anyone
     * you hide stories from. The key used is the one this app knows for that friend; if the server offers a different
     * one, that friend is skipped. Returns how many keys were added.
     */
    suspend fun fillKeyGaps(
        identity: SelfIdentity,
        ownKeys: Map<String, ByteArray>,
        friends: Map<String, StoryFriend>,
        prefs: StoryPrefs,
        deviceKeys: StoryDeviceKeys? = null,
        /** Part D: your live stories, for their keys and end times. */
        own: List<Story> = emptyList(),
    ): Int {
        val gaps = apiCall(ApiClient.json) { api.keyGaps() }
        var added = 0
        val ownById = own.associateBy { it.id.lowercase() }
        // A story that expired into a highlight isn't in the feed any more: its K comes from your own highlights (read once, only if needed).
        var highlighted: Map<String, Story>? = null
        suspend fun ownStory(storyId: String, dk: StoryDeviceKeys): Story? {
            val id = storyId.lowercase()
            val found = ownById[id] ?: (highlighted ?: ownHighlightedStories(identity, dk).also { highlighted = it })[id]
            return found?.takeIf { it.deviceKeys }
        }
        // Part D: what an earlier pass couldn't deliver goes first (those viewers already have access).
        if (deviceKeys != null) {
            for ((storyId, users) in deviceKeys.owed()) {
                val story = ownStory(storyId, deviceKeys) ?: continue
                added += deviceKeys.deliverKey(story.id, story.key, story.expiresAtMs, users).delivered.size
            }
        }
        for (gap in gaps) {
            if (gap.delivery == DELIVERY_DEVICE_KEYS) {
                val dk = deviceKeys ?: continue
                val story = ownStory(gap.storyId, dk) ?: continue
                added += fillDeviceKeyGap(gap, story, friends, prefs, dk)
                continue
            }
            val key = ownKeys[gap.storyId] ?: continue
            val wrapped = gap.users
                .filter { it.userId !in prefs.hideFrom }
                .mapNotNull { u ->
                    val known = friends[u.userId] ?: return@mapNotNull null
                    if (Base64Codec.encode(known.publicKey) != u.publicKey) return@mapNotNull null
                    try {
                        StoryWrappedKeyDto(u.userId, Base64Codec.encode(crypto.wrapStoryKey(gap.storyId, key, known.publicKey, identity.privateKey)))
                    } catch (e: StoryCryptoException) {
                        null
                    }
                }
            if (wrapped.isEmpty()) continue
            apiCall(ApiClient.json) { api.addKeys(gap.storyId, StoryKeysRequest(wrapped)) }
            added += wrapped.size
        }
        return added
    }

    /**
     * Part D: late friends on a device-keys `friends` story. Access first (`POST /:id/access`; the mailbox refuses a key
     * for someone without it), then K to their devices. Friends you hide stories from, people this app doesn't know as
     * friends, and friends without device keys (they couldn't open it) are left out. Returns how many got it.
     */
    private suspend fun fillDeviceKeyGap(
        gap: com.redvaultsystems.redvaultmessage.data.api.StoryKeyGapDto,
        story: Story,
        friends: Map<String, StoryFriend>,
        prefs: StoryPrefs,
        deviceKeys: StoryDeviceKeys,
    ): Int {
        val users = gap.users.map { it.userId.lowercase() }.distinct()
            .filter { it !in prefs.hideFrom && friends.containsKey(it) }
        return addDeviceKeyViewers(story, users, deviceKeys)
    }

    /** Your own highlighted device-keys stories by id (their K from this phone's story-key store); empty if they can't be read. */
    private suspend fun ownHighlightedStories(identity: SelfIdentity, deviceKeys: StoryDeviceKeys): Map<String, Story> = try {
        highlights(identity, identity.userId, identity.publicKey, deviceKeys)
            .flatMap { it.stories }
            .filter { it.authorId == identity.userId && it.deviceKeys }
            .associateBy { it.id.lowercase() }
    } catch (e: ApiException) {
        if (e.status == 401) throw e
        emptyMap()
    }

    /**
     * Part D: new viewers of one of your device-keys stories (late friends of a `friends` story, people added to close friends
     * after a `close_friends` story). Access first (`POST /:id/access`; the mailbox refuses a key for someone without it), then K
     * to their devices; whoever can't be reached now is owed and retried by the next key-gap pass. People without device keys
     * are left out (they couldn't open it). Returns how many got the key now.
     */
    suspend fun addDeviceKeyViewers(story: Story, userIds: List<String>, deviceKeys: StoryDeviceKeys): Int {
        val users = userIds.map { it.lowercase() }.distinct().filter { deviceKeys.allHaveDevices(listOf(it)) }
        if (users.isEmpty()) return 0
        apiCall(ApiClient.json) { api.addAccess(story.id, StoryAccessRequest(users)) }
        deviceKeys.owe(story.id, users, story.expiresAtMs)
        return deviceKeys.deliverKey(story.id, story.key, story.expiresAtMs, users).delivered.size
    }

    companion object {
        /** The server's own limits are 12 MB and 60 MB; a bit of room for the framing. */
        const val MAX_PHOTO_CIPHERTEXT_BYTES = 13L * 1024 * 1024
        const val MAX_VIDEO_CIPHERTEXT_BYTES = 62L * 1024 * 1024

        /** The server keeps a recording up to 25 MiB; a friend's app picks the size it claims, so it's capped here. */
        const val MAX_AUDIO_CIPHERTEXT_BYTES = 26L * 1024 * 1024
        private const val PHOTO_CACHE_BYTES = 24L * 1024 * 1024
        /** How often the archive and highlights are asked about keys the feed no longer lists (as on the desktop). */
        const val UNLISTED_CHECK_MS = 5 * 60_000L
    }
}

/**
 * Part D's retention of K (docs/signal-integration/12-part-d-stories.md, "Retention of K"): a key goes when its story is
 * deleted, or once the feed ([listed]), the [archive] (this account's own stories) and the [highlights] of whoever sent a
 * key for it all stop listing it; revoked access and blocks end up there. Never at `expiresAt` alone: archived and
 * highlighted stories keep opening. A key under an hour old stays ([StoryDeviceKeys.unlisted]): it can come before its
 * story is published. A 404 from a route counts as "not listed there"; any other failure means nothing is forgotten on a
 * guess (null). Returns the story ids forgotten.
 */
suspend fun forgetUnlistedStoryKeys(
    listed: Set<String>,
    deviceKeys: StoryDeviceKeys,
    archive: suspend () -> List<String>,
    highlights: suspend (ownerId: String) -> List<String>,
): Set<String>? {
    val candidates = deviceKeys.unlisted(listed)
    if (candidates.isEmpty()) return emptySet()
    val stillListed = HashSet<String>()
    suspend fun add(call: suspend () -> List<String>) {
        try {
            call().mapTo(stillListed) { it.lowercase() }
        } catch (e: ApiException) {
            if (e.status != 404) throw e
        }
    }
    try {
        add(archive)
        for (owner in candidates.values.flatten().mapTo(LinkedHashSet()) { it.lowercase() }) add { highlights(owner) }
    } catch (e: ApiException) {
        if (e.status == 401) throw e
        return null
    }
    val gone = candidates.keys.filterTo(HashSet()) { it !in stillListed }
    deviceKeys.forgetAll(gone)
    return gone
}

/**
 * Stories seen on this phone, by id, so rings stay grey even when you don't share views (nothing then reaches the server).
 * Ids only, per account, dropped after two days like the web app's.
 */
class StorySeenStore(context: Context) {
    private val prefs = context.getSharedPreferences("storiesSeen", Context.MODE_PRIVATE)

    fun ids(selfUserId: String, now: Long = System.currentTimeMillis()): Set<String> =
        entries(selfUserId, now).keys

    fun remember(selfUserId: String, storyId: String, now: Long = System.currentTimeMillis()) {
        val kept = entries(selfUserId, now) + (storyId to now)
        prefs.edit().putStringSet(selfUserId, kept.map { "${it.key}|${it.value}" }.toSet()).apply()
    }

    private fun entries(selfUserId: String, now: Long): Map<String, Long> =
        prefs.getStringSet(selfUserId, emptySet()).orEmpty().mapNotNull { entry ->
            val at = entry.substringAfter('|', "").toLongOrNull() ?: return@mapNotNull null
            if (now - at < KEEP_MS) entry.substringBefore('|') to at else null
        }.toMap()

    private companion object {
        const val KEEP_MS = 48 * 3600_000L
    }
}
