// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.channels

import android.graphics.Bitmap
import android.media.MediaDataSource
import android.net.Uri
import com.redvaultsystems.redvaultmessage.crypto.ChannelCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.data.ConversionResult
import com.redvaultsystems.redvaultmessage.data.OutgoingMedia
import com.redvaultsystems.redvaultmessage.data.PrivateChannels
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaDto
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
    fileCrypto: FileCrypto,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /** Part C1: a private channel's keys, and sealing and opening what goes up and comes down. */
    private val sealed = PrivateChannels(
        repository, crypto, fileCrypto, media, media.workDir,
        ownerPublicKey = { identity.publicKey },
        ownerPrivateKey = { identity.privateKey },
    )

    /** Per channel, how converting its posts after a visibility switch is going (for the owner's settings), or nothing. */
    private val _conversion = MutableStateFlow<Map<String, String>>(emptyMap())
    val conversion: StateFlow<Map<String, String>> = _conversion.asStateFlow()
    private val maintaining = ConcurrentHashMap<String, Job>()
    private val _followed = MutableStateFlow(store.load(identity.userId))
    val followed: StateFlow<List<FollowedChannel>> = _followed.asStateFlow()

    /** Followed channels with posts you haven't seen, as of the last check (the Channels badge). */
    private val _unread = MutableStateFlow(0)
    val unread: StateFlow<Int> = _unread.asStateFlow()

    /** The channel open on screen right now: no notifications for it. */
    @Volatile
    var currentChannelId: String? = null

    private val tokens = ConcurrentHashMap<String, String>()
    /** Each access token's secret, so a call made with a token can open the channel's keys. */
    private val secrets = ConcurrentHashMap<String, String>()
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
        val token = tokens.getOrPut(secret) { runCatching { crypto.accessToken(secret) }.getOrNull() ?: return null }
        secrets[token] = secret
        return token
    }

    /** The private link's secret from your own channel's sealed copy, or null. */
    fun ownSecret(channel: ChannelDto): String? = crypto.openSecret(channel.secretSealed, identity.publicKey, identity.privateKey)

    /** A reader's call about [id] with [access]: the link it came from opens the channel's keys. */
    private fun reading(id: String, access: String?) {
        access?.let { secrets[it] }?.let { sealed.useLink(id, it) }
    }

    /**
     * Private: a fresh link (anyone with the old one loses access) and a new key epoch; every older key is
     * re-wrapped under the new link, so it reads the whole history. Public: no link, and the posts are turned
     * back into plain text on the server. Either way the posts in the old format are converted in the
     * background. Returns the channel and the new secret.
     */
    suspend fun setPrivate(id: String, makePrivate: Boolean): Pair<ChannelDto, String?> = signedIn {
        val result = if (makePrivate) sealed.makePrivate(id).let { (updated, fresh) -> updated to fresh } else sealed.makePublic(id) to null
        val (updated, fresh) = result
        if (fresh != null && followedEntry(id) != null) save(Channels.markSeen(_followed.value, updated, fresh))
        maintain(id, force = true)
        result
    }

    /**
     * The owner opened their channel: a private channel made before C1 gets its keys (same link), and posts
     * still in the old format are converted, oldest first. Runs in the background; a round stopped by rate
     * limits carries on next time.
     */
    fun maintain(id: String, force: Boolean = false) {
        val key = id.lowercase()
        if (!force && maintaining[key]?.isActive == true) return
        maintaining[key]?.cancel()
        maintaining[key] = scope.launch {
            try {
                val result = sealed.maintain(key) { n -> setConversion(key, "Converting older posts… $n done.") }
                setConversion(
                    key,
                    when (result) {
                        is ConversionResult.Done -> if (result.converted > 0) "All ${result.converted} older posts converted." else null
                        is ConversionResult.Paused -> "Converting older posts paused after ${result.converted}: ${result.reason} It carries on next time you open the channel."
                    },
                )
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired()
                setConversion(key, null)
            } catch (e: kotlinx.coroutines.CancellationException) {
                throw e
            } catch (e: Exception) {
                setConversion(key, null)
            }
        }
    }

    private fun setConversion(id: String, text: String?) {
        _conversion.value = if (text == null) _conversion.value - id else _conversion.value + (id to text)
    }

    // --- Reading (no account) ---

    suspend fun search(query: String, offset: Int = 0): List<ChannelDto> = repository.search(query, offset)

    suspend fun get(id: String, access: String? = null): ChannelDto {
        reading(id, access)
        return repository.get(id, access).also(sealed::remember)
    }

    /** A page of posts; a private channel's are opened on this phone (one its link can't open comes back locked). */
    suspend fun posts(id: String, before: String? = null, access: String? = null): List<ChannelPostDto> {
        reading(id, access)
        return opened(sealed.openPosts(id, repository.posts(id, before, access), access))
    }

    /** Opened posts' media keys go to the media client, so their photos and files open. */
    private fun opened(list: List<ChannelPostDto>): List<ChannelPostDto> {
        list.forEach { media.rememberSealed(it.media) }
        return list
    }

    private fun opened(post: ChannelPostDto): ChannelPostDto = opened(listOf(post)).single()

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

    suspend fun mine(): List<ChannelDto> = signedIn { repository.mine().onEach(sealed::rememberOwned) }

    suspend fun create(name: String, description: String): ChannelDto = signedIn { repository.create(name, description).also(sealed::rememberOwned) }

    suspend fun update(id: String, name: String, description: String): ChannelDto =
        signedIn { repository.update(id, name, description).also(sealed::rememberOwned) }

    suspend fun setAcceptSuggestions(id: String, accept: Boolean): ChannelDto =
        signedIn { repository.setAcceptSuggestions(id, accept).also(sealed::rememberOwned) }

    /** A downsized JPEG of the picked image (no EXIF), as the channel's photo. */
    suspend fun setPhoto(id: String, imageBase64: String): ChannelDto = signedIn { repository.setAvatar(id, imageBase64).also(sealed::rememberOwned) }

    suspend fun removePhoto(id: String) = signedIn { repository.removeAvatar(id) }

    suspend fun delete(id: String) = signedIn {
        repository.delete(id)
        unfollow(id)
    }

    // --- Posting ---

    suspend fun startMedia(id: String, meta: ChannelMediaStartRequest): ChannelUploadDto = signedIn { repository.startMedia(id, meta) }

    /**
     * One file for a post: in a private channel it's encrypted on this phone first (the server learns only its
     * encrypted size); in a public one it goes up as it is. Returns the upload's id.
     */
    suspend fun uploadForPost(id: String, out: OutgoingMedia): String = signedIn {
        if (sealed.isPrivate(id)) {
            sealed.sealUpload(id, { media.open(out.file, out.uri) }, out.meta, forSuggestion = false, access = null, onProgress = out.onProgress)
        } else {
            val started = repository.startMedia(id, out.meta)
            media.upload(started.uploadUrl, out.file, out.uri, out.size, out.onProgress)
            started.id
        }
    }

    /** A post: sealed on this phone in a private channel (text, poll, checklist, media keys), as it is in a public one. */
    suspend fun post(id: String, post: ChannelPostRequest): ChannelPostDto = signedIn { opened(sealed.post(id, post)) }

    /** The owner edits a private channel's post: its text, sealed again under the newest key. */
    suspend fun edit(id: String, postId: String, body: String): ChannelPostDto = signedIn { opened(sealed.edit(id, postId, body)) }

    suspend fun scheduled(id: String): List<ChannelPostDto> = signedIn { opened(sealed.openPosts(id, repository.scheduled(id), null)) }

    suspend fun publishNow(id: String, postId: String): ChannelPostDto = signedIn { opened(sealed.openPost(id, repository.publishNow(id, postId), null)) }

    suspend fun deletePost(id: String, postId: String) = signedIn { repository.deletePost(id, postId) }

    // --- Polls and checklists ---

    /** Your vote (empty takes a poll vote back); remembered on this phone so the results show. */
    suspend fun vote(id: String, postId: String, options: List<Int>, access: String?): ChannelVoteDto = signedIn {
        reading(id, access)
        val result = sealed.fillVote(postId, repository.vote(id, postId, options, access))
        val remembered = result.mine.takeIf { it.isNotEmpty() }?.let {
            RememberedVote(it, result.poll.correct.takeIf { result.poll.quiz }, result.poll.explanation.takeIf { result.poll.quiz })
        }
        device.rememberVote(identity.userId, postId, remembered)
        result
    }

    fun rememberedVote(postId: String): RememberedVote? = device.vote(identity.userId, postId)

    suspend fun closePoll(id: String, postId: String): ChannelPostDto = signedIn { opened(sealed.openPost(id, repository.closePoll(id, postId), null)) }

    suspend fun setChecklistItem(id: String, postId: String, index: Int, done: Boolean, access: String?): ChannelChecklistDto =
        signedIn { sealed.fillChecklist(postId, repository.setChecklistItem(id, postId, index, done, access)) }

    fun position(mediaId: String): Int = device.position(mediaId)

    fun savePosition(mediaId: String, seconds: Int) = device.savePosition(mediaId, seconds)

    // --- Suggested posts ---

    suspend fun startSuggestionMedia(id: String, meta: ChannelMediaStartRequest, access: String?): ChannelUploadDto =
        signedIn { repository.startSuggestionMedia(id, meta, access) }

    /** A photo for a suggestion: encrypted on this phone first in a private channel. Returns the upload's id. */
    suspend fun uploadForSuggestion(id: String, out: OutgoingMedia, access: String?): String = signedIn {
        reading(id, access)
        if (sealed.isPrivate(id)) {
            sealed.sealUpload(id, { media.open(out.file, out.uri) }, out.meta, forSuggestion = true, access = access, onProgress = out.onProgress)
        } else {
            val started = repository.startSuggestionMedia(id, out.meta, access)
            media.upload(started.uploadUrl, out.file, out.uri, out.size, out.onProgress)
            started.id
        }
    }

    suspend fun suggest(id: String, body: String, media: List<String>, access: String?): ChannelSuggestionDto = signedIn {
        reading(id, access)
        sealed.suggest(id, body, media, access).also { this.media.rememberSealed(it.media) }
    }

    /** Yours: the channel is opened with the link you hold (or your own keys). */
    suspend fun mySuggestions(id: String, access: String? = null): List<ChannelSuggestionDto> = signedIn {
        reading(id, access)
        sealed.openSuggestions(id, repository.mySuggestions(id), access).onEach { media.rememberSealed(it.media) }
    }

    suspend fun suggestions(id: String): List<ChannelSuggestionDto> =
        signedIn { sealed.openSuggestions(id, repository.suggestions(id), null).onEach { media.rememberSealed(it.media) } }

    /** In a private channel the owner's phone seals the suggestion again under the newest key. */
    suspend fun approveSuggestion(id: String, suggestionId: String, body: String?, publishAt: String?): ChannelPostDto =
        signedIn { opened(sealed.approve(id, suggestionId, body, publishAt)) }

    suspend fun rejectSuggestion(id: String, suggestionId: String) = signedIn { repository.rejectSuggestion(id, suggestionId) }

    suspend fun report(id: String, reason: String) = signedIn { repository.report(id, reason) }

    // --- Media ---

    suspend fun avatar(avatarUrl: String, access: String?): Bitmap? = media.avatar(avatarUrl, access)

    suspend fun photo(mediaId: String, url: String, maxSide: Int): Bitmap? = media.photo(mediaId, url, maxSide)

    suspend fun download(mediaId: String, url: String, onProgress: (Float) -> Unit = {}): File = media.download(mediaId, url, onProgress)

    /** What plays [item]: by byte ranges, or a private channel's from its opened copy. */
    fun sourceFor(item: ChannelMediaDto): MediaDataSource? = media.sourceFor(item)

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
                        val access = accessFor(f.secret)
                        sealed.useLink(f.id, f.secret)
                        sealed.remember(channel)
                        val latest = runCatching {
                            repository.posts(f.id, null, access, 1).firstOrNull()?.let { sealed.openPost(f.id, it, access) }
                        }.getOrNull()
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
        sealed.clear()
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
