// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.channels

import android.graphics.Bitmap
import android.media.MediaDataSource
import android.net.Uri
import com.redvaultsystems.redvaultmessage.crypto.ChannelCrypto
import com.redvaultsystems.redvaultmessage.data.ChannelDeviceStore
import com.redvaultsystems.redvaultmessage.data.ChannelMediaClient
import com.redvaultsystems.redvaultmessage.data.ChannelRepository
import com.redvaultsystems.redvaultmessage.data.FollowedChannelsStore
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaStartRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelSuggestionDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelUploadDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelVisibilityRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelVoteDto
import com.redvaultsystems.redvaultmessage.data.model.ChannelPosts
import com.redvaultsystems.redvaultmessage.data.model.Channels
import com.redvaultsystems.redvaultmessage.data.model.FollowedChannel
import com.redvaultsystems.redvaultmessage.data.model.RememberedVote
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.cancel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.File
import java.util.concurrent.ConcurrentHashMap

/** A new post in a followed channel, to notify about (see ChannelNotifications). */
class ChannelNotice(val channelId: String, val channelName: String, val preview: String)

/**
 * Channels while signed in: the follow list (on this phone only, see
 * FollowedChannelsStore, with private channels' link secrets), the server
 * calls, and what this phone keeps for itself (ChannelDeviceStore: views it
 * counted, your votes, podcast positions). It also checks followed channels
 * for new posts every couple of minutes while the app runs, for the
 * Channels badge and notifications: the server keeps no follower list, so
 * nothing but this phone knows what it follows. Each call throws
 * ApiException; a 401 on a signed-in call ends the session first.
 */
class ChannelSession(
    private val repository: ChannelRepository,
    private val store: FollowedChannelsStore,
    private val device: ChannelDeviceStore,
    private val crypto: ChannelCrypto,
    val media: ChannelMediaClient,
    private val identity: SelfIdentity,
    private val notify: (ChannelNotice) -> Unit,
    private val onSessionExpired: () -> Unit,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _followed = MutableStateFlow(store.load(identity.userId))
    val followed: StateFlow<List<FollowedChannel>> = _followed.asStateFlow()

    /** Followed channels with posts you haven't seen, as of the last check (the Channels badge). */
    private val _unread = MutableStateFlow(0)
    val unread: StateFlow<Int> = _unread.asStateFlow()

    /** The channel open on screen right now: no notifications for it. */
    @Volatile
    var currentChannelId: String? = null

    private val tokens = ConcurrentHashMap<String, String>()
    private val checking = Mutex()
    private var watcher: Job? = null

    /** Super admins may take any channel or post down. */
    val isSuperAdmin: Boolean get() = identity.role == "super_admin"

    val userId: String get() = identity.userId

    /** Whether the Channels tab already offered to turn notifications on. */
    var askedForNotifications: Boolean
        get() = device.askedForNotifications
        set(value) {
            device.askedForNotifications = value
        }

    // --- Following (this phone only) ---

    fun follow(channel: ChannelDto, secret: String? = null) = save(Channels.follow(_followed.value, channel, secret))

    fun unfollow(id: String) = save(Channels.unfollow(_followed.value, id))

    fun setMuted(id: String, muted: Boolean) = save(Channels.setMuted(_followed.value, id, muted))

    fun followedEntry(id: String): FollowedChannel? = _followed.value.firstOrNull { it.id == id }

    /** Opening a followed channel counts its posts as seen, and keeps the private link it was opened with. */
    fun markSeen(channel: ChannelDto, secret: String? = null) {
        val seen = followedEntry(channel.id) ?: return
        val newSecret = secret?.takeIf { channel.isPrivate && it != seen.secret }
        if (seen.seenAt != channel.lastPostAt || seen.name != channel.name || newSecret != null) {
            save(Channels.markSeen(_followed.value, channel, newSecret))
        }
    }

    // --- Private links ---

    /** The access token a private channel's link secret shows the server, or null. */
    fun accessFor(secret: String?): String? {
        if (secret.isNullOrEmpty()) return null
        return tokens.getOrPut(secret) { runCatching { crypto.accessToken(secret) }.getOrNull() ?: return null }
    }

    /** The private link's secret from your own channel's sealed copy, or null. */
    fun ownSecret(channel: ChannelDto): String? = crypto.openSecret(channel.secretSealed, identity.publicKey, identity.privateKey)

    /** Private: a fresh link (anyone with the old one loses access). Public: no link needed. Returns the channel and the new secret. */
    suspend fun setPrivate(id: String, makePrivate: Boolean): Pair<ChannelDto, String?> = signedIn {
        if (!makePrivate) return@signedIn repository.setVisibility(id, ChannelVisibilityRequest(isPrivate = false)) to null
        val fresh = crypto.newSecret()
        val updated = repository.setVisibility(
            id,
            ChannelVisibilityRequest(isPrivate = true, access = crypto.accessToken(fresh), secretSealed = crypto.sealSecret(fresh, identity.publicKey)),
        )
        if (followedEntry(id) != null) save(Channels.markSeen(_followed.value, updated, fresh))
        updated to fresh
    }

    // --- Reading (no account) ---

    suspend fun search(query: String, offset: Int = 0): List<ChannelDto> = repository.search(query, offset)

    suspend fun get(id: String, access: String? = null): ChannelDto = repository.get(id, access)

    suspend fun posts(id: String, before: String? = null, access: String? = null): List<ChannelPostDto> = repository.posts(id, before, access)

    /**
     * Counts a view of each of these posts this phone hasn't counted yet
     * (in the channel or as a forwarded copy in a chat). The report carries
     * post ids only, no account and no cookie. Best effort.
     */
    fun countViews(postIds: List<String>) {
        if (postIds.isEmpty()) return
        scope.launch {
            val fresh = kotlinx.coroutines.withContext(Dispatchers.IO) { device.takeUncountedViews(postIds) }
            for (batch in fresh.chunked(ChannelPosts.VIEWS_BATCH)) runCatching { repository.recordViews(batch) }
        }
    }

    // --- Your own channels ---

    suspend fun mine(): List<ChannelDto> = signedIn { repository.mine() }

    suspend fun create(name: String, description: String): ChannelDto = signedIn { repository.create(name, description) }

    suspend fun update(id: String, name: String, description: String): ChannelDto = signedIn { repository.update(id, name, description) }

    suspend fun setAcceptSuggestions(id: String, accept: Boolean): ChannelDto = signedIn { repository.setAcceptSuggestions(id, accept) }

    /** A downsized JPEG of the picked image (no EXIF), as the channel's photo. */
    suspend fun setPhoto(id: String, imageBase64: String): ChannelDto = signedIn { repository.setAvatar(id, imageBase64) }

    suspend fun removePhoto(id: String) = signedIn { repository.removeAvatar(id) }

    suspend fun delete(id: String) = signedIn {
        repository.delete(id)
        unfollow(id)
    }

    // --- Posting ---

    suspend fun startMedia(id: String, meta: ChannelMediaStartRequest): ChannelUploadDto = signedIn { repository.startMedia(id, meta) }

    suspend fun post(id: String, post: ChannelPostRequest): ChannelPostDto = signedIn { repository.post(id, post) }

    suspend fun scheduled(id: String): List<ChannelPostDto> = signedIn { repository.scheduled(id) }

    suspend fun publishNow(id: String, postId: String): ChannelPostDto = signedIn { repository.publishNow(id, postId) }

    suspend fun deletePost(id: String, postId: String) = signedIn { repository.deletePost(id, postId) }

    // --- Polls and checklists ---

    /** Your vote (empty takes a poll vote back); remembered on this phone so the results show. */
    suspend fun vote(id: String, postId: String, options: List<Int>, access: String?): ChannelVoteDto = signedIn {
        val result = repository.vote(id, postId, options, access)
        val remembered = result.mine.takeIf { it.isNotEmpty() }?.let {
            RememberedVote(it, result.poll.correct.takeIf { result.poll.quiz }, result.poll.explanation.takeIf { result.poll.quiz })
        }
        device.rememberVote(identity.userId, postId, remembered)
        result
    }

    fun rememberedVote(postId: String): RememberedVote? = device.vote(identity.userId, postId)

    suspend fun closePoll(id: String, postId: String): ChannelPostDto = signedIn { repository.closePoll(id, postId) }

    suspend fun setChecklistItem(id: String, postId: String, index: Int, done: Boolean, access: String?): ChannelChecklistDto =
        signedIn { repository.setChecklistItem(id, postId, index, done, access) }

    fun position(mediaId: String): Int = device.position(mediaId)

    fun savePosition(mediaId: String, seconds: Int) = device.savePosition(mediaId, seconds)

    // --- Suggested posts ---

    suspend fun startSuggestionMedia(id: String, meta: ChannelMediaStartRequest, access: String?): ChannelUploadDto =
        signedIn { repository.startSuggestionMedia(id, meta, access) }

    suspend fun suggest(id: String, body: String, media: List<String>, access: String?): ChannelSuggestionDto =
        signedIn { repository.suggest(id, body, media, access) }

    suspend fun mySuggestions(id: String): List<ChannelSuggestionDto> = signedIn { repository.mySuggestions(id) }

    suspend fun suggestions(id: String): List<ChannelSuggestionDto> = signedIn { repository.suggestions(id) }

    suspend fun approveSuggestion(id: String, suggestionId: String, body: String?, publishAt: String?): ChannelPostDto =
        signedIn { repository.approveSuggestion(id, suggestionId, body, publishAt) }

    suspend fun rejectSuggestion(id: String, suggestionId: String) = signedIn { repository.rejectSuggestion(id, suggestionId) }

    suspend fun report(id: String, reason: String) = signedIn { repository.report(id, reason) }

    // --- Media ---

    suspend fun avatar(avatarUrl: String, access: String?): Bitmap? = media.avatar(avatarUrl, access)

    suspend fun photo(mediaId: String, url: String, maxSide: Int): Bitmap? = media.photo(mediaId, url, maxSide)

    suspend fun download(mediaId: String, url: String, onProgress: (Float) -> Unit = {}): File = media.download(mediaId, url, onProgress)

    fun rangeSource(url: String, size: Long): MediaDataSource? = media.rangeSource(url, size)

    fun describe(uri: Uri) = media.describe(uri)

    // --- New posts in followed channels ---

    /** Starts checking followed channels every couple of minutes, until the session closes. */
    fun startWatching() {
        if (watcher?.isActive == true) return
        watcher = scope.launch {
            while (isActive) {
                checkNow()
                delay(ChannelPosts.CHECK_EVERY_MS)
            }
        }
    }

    /**
     * One check: reads each followed channel (four at a time, no cookie),
     * counts those with unseen posts for the badge, and notifies of each one
     * newly posted in, unless it's muted or open right now.
     */
    fun checkNow() {
        scope.launch {
            if (!checking.tryLock()) return@launch
            try {
                check()
            } finally {
                checking.unlock()
            }
        }
    }

    private suspend fun check() {
        val followed = _followed.value
        val notified = device.notified(identity.userId).toMutableMap()
        val lock = Mutex()
        var count = 0
        val queue = ArrayDeque(followed)
        coroutineScope {
            (1..4).map {
                async {
                    while (true) {
                        val f = lock.withLock { queue.removeFirstOrNull() } ?: break
                        val channel = runCatching { repository.get(f.id, accessFor(f.secret)) }.getOrNull() ?: continue
                        val outcome = lock.withLock { ChannelPosts.watch(f, channel, notified, currentChannelId) }
                        if (outcome.unread) lock.withLock { count += 1 }
                        if (!outcome.notify) continue
                        val latest = runCatching { repository.posts(f.id, null, accessFor(f.secret), 1).firstOrNull() }.getOrNull()
                        notify(ChannelNotice(f.id, channel.name, ChannelPosts.postPreview(latest)))
                    }
                }
            }.awaitAll()
        }
        device.saveNotified(identity.userId, notified)
        _unread.value = count
    }

    fun close() {
        scope.cancel()
        media.clear()
    }

    private fun save(list: List<FollowedChannel>) {
        if (list == _followed.value) return
        _followed.value = list
        store.save(identity.userId, list)
        // The badge follows what's followed and seen.
        checkNow()
    }

    private suspend fun <T> signedIn(block: suspend () -> T): T {
        try {
            return block()
        } catch (e: ApiException) {
            if (e.status == 401) onSessionExpired()
            throw e
        }
    }

    companion object {
        const val PAGE = ChannelRepository.PAGE
    }
}
