// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.ChannelCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistItemDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistShape
import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelEncryptedApproveRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelEncryptedPostRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelEncryptedSuggestionRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaStartRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelPollDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPollRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelPollShape
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelQuizRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelSuggestionDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelVisibilityRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelVoteDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelWrappedKeyDto
import com.redvaultsystems.redvaultmessage.data.api.SealedMedia
import com.redvaultsystems.redvaultmessage.data.model.ChannelSealedPosts
import com.redvaultsystems.redvaultmessage.data.model.SealedChecklist
import com.redvaultsystems.redvaultmessage.data.model.SealedContent
import com.redvaultsystems.redvaultmessage.data.model.SealedMediaItem
import com.redvaultsystems.redvaultmessage.data.model.SealedPoll
import com.redvaultsystems.redvaultmessage.data.model.SealedPostPlain
import com.redvaultsystems.redvaultmessage.data.model.SealedQuiz
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.io.InputStream
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap

/** Moves a channel's media bytes: the app's own client in the app (ChannelMediaClient), a fake in tests. */
interface ChannelMediaTransfer {
    /** Downloads a same-server signed link's bytes into [into]. */
    suspend fun fetch(url: String, into: File)

    /** Sends [file]'s bytes to an upload address the server gave. */
    suspend fun send(uploadUrl: String, file: File, onProgress: (Float) -> Unit = {})
}

/** How a round of converting posts after a visibility switch ended. */
sealed class ConversionResult {
    abstract val converted: Int

    /** Nothing is left in the old format. */
    data class Done(override val converted: Int) : ConversionResult()

    /** Stopped for now (rate limits, the network, or a post that can't be opened); the next round carries on. */
    data class Paused(override val converted: Int, val reason: String) : ConversionResult()
}

/**
 * Private channels encrypted with the link (Part C1,
 * docs/signal-integration/10-part-c1-private-channels.md), apart from the screens so it can be tested off a
 * phone. It keeps each channel's content keys (unwrapped from the server's copies with the link's secret: the
 * owner's own sealed copy, or the link a reader opened), seals what goes up in a private channel (posts, edits,
 * scheduled posts, polls, checklists, suggestions and media) and opens what comes down into the plain shapes
 * the screens already show. Public channels pass straight through, unchanged.
 *
 * The owner's side: going private (epoch 1), resetting the link (a new epoch, every older key re-wrapped under
 * the new link, so the new link reads the whole history and the old one nothing new), going public, setting up
 * keys for a channel made private before C1, and converting the posts still in the old format, oldest first.
 *
 * Nothing here logs: no keys, secrets or content leave this class except to the server, sealed.
 */
class PrivateChannels(
    private val repository: ChannelRepository,
    private val crypto: ChannelCrypto,
    private val fileCrypto: FileCrypto,
    private val transfer: ChannelMediaTransfer,
    /** App-private scratch space for media being sealed or opened during a conversion. */
    private val workDir: File,
    private val ownerPublicKey: () -> ByteArray,
    private val ownerPrivateKey: () -> ByteArray,
    /** Waits out a rate limit (tests pass a no-op). */
    private val pause: suspend (Long) -> Unit = { delay(it) },
) {
    /** One channel's opened keys, from one link's secret. */
    private class Ring(val secret: String, val keyEpoch: Int?, val keys: Map<Int, ByteArray>)

    /** An opened post or suggestion, kept for votes, ticks, edits, approvals and conversions. */
    private class Opened(val channelId: String, val plain: SealedPostPlain, val mediaIds: List<String>, val sealed: Boolean)

    private val rings = ConcurrentHashMap<String, Ring>()
    private val links = ConcurrentHashMap<String, String>()
    private val owned = ConcurrentHashMap<String, ChannelDto>()
    private val known = ConcurrentHashMap<String, ChannelDto>()
    private val posts = ConcurrentHashMap<String, Opened>()
    private val suggestions = ConcurrentHashMap<String, Opened>()
    private val uploads = ConcurrentHashMap<String, SealedMediaItem>()
    private val locks = ConcurrentHashMap<String, Mutex>()

    // --- What this phone knows ---

    /** The link a reader opened (or follows) for [channelId]. */
    fun useLink(channelId: String, secret: String?) {
        val id = channelId.lowercase()
        if (secret.isNullOrEmpty() || links[id] == secret) return
        links[id] = secret
        if (!owned.containsKey(id)) rings.remove(id)
    }

    /** A channel as the server just showed it: a newer epoch means the keys are read again. */
    fun remember(channel: ChannelDto) {
        val id = channel.id.lowercase()
        known[id] = channel
        val ring = rings[id]
        if (ring != null && (channel.keyEpoch ?: 0) > (ring.keyEpoch ?: 0)) rings.remove(id)
    }

    /** The owner's own copies (channels/mine and every owner route's answer). */
    fun rememberOwned(channel: ChannelDto) {
        val id = channel.id.lowercase()
        val before = owned.put(id, channel)
        if (before != null && (before.secretSealed != channel.secretSealed || before.formerSecretSealed != channel.formerSecretSealed)) {
            // Only drop the keys if they came from a secret that's gone.
            val ring = rings[id]
            if (ring != null && ring.secret != ownerSecret(channel)) rings.remove(id)
        }
        remember(channel)
    }

    fun isPrivate(channelId: String): Boolean = known[channelId.lowercase()]?.isPrivate == true

    fun isOwned(channelId: String): Boolean = owned.containsKey(channelId.lowercase())

    /** The current link's secret (private) or else the last one (public), from the owner's sealed copies. */
    private fun ownerSecret(channel: ChannelDto): String? =
        crypto.openSecret(channel.secretSealed, ownerPublicKey(), ownerPrivateKey())
            ?: crypto.openSecret(channel.formerSecretSealed, ownerPublicKey(), ownerPrivateKey())

    private fun secretFor(channelId: String): String? = owned[channelId]?.let(::ownerSecret) ?: links[channelId]

    private fun lock(channelId: String) = locks.getOrPut(channelId.lowercase()) { Mutex() }

    // --- Keys ---

    /** The channel's keys, read once and kept; null when this phone can't have them (no link, or a refused one). */
    private suspend fun ring(channelId: String, access: String?, refresh: Boolean = false): Ring? {
        val id = channelId.lowercase()
        if (!refresh) rings[id]?.let { return it }
        val secret = secretFor(id) ?: return null
        val dto = try {
            if (owned.containsKey(id)) repository.ownerKeys(id) else repository.publicKeys(id, access ?: crypto.accessToken(secret))
        } catch (e: ApiException) {
            if (e.status == 404) return null
            throw e
        }
        val wrapKey = crypto.wrapKey(secret)
        val keys = HashMap<Int, ByteArray>()
        try {
            for (k in dto.keys) crypto.unwrapContentKey(k.wrapped, id, k.epoch, wrapKey)?.let { keys[k.epoch] = it }
        } finally {
            wrapKey.fill(0)
        }
        return Ring(secret, dto.keyEpoch, keys).also { rings[id] = it }
    }

    /** The newest epoch and its key, to seal with: reads the keys again if this phone's copy is behind. */
    private suspend fun newest(channelId: String, access: String?, refresh: Boolean = false): Pair<Int, ByteArray> {
        var ring = ring(channelId, access, refresh)
        val wanted = known[channelId.lowercase()]?.keyEpoch
        if (ring != null && wanted != null && (ring.keyEpoch ?: 0) < wanted && !refresh) ring = ring(channelId, access, true)
        val epoch = ring?.keyEpoch ?: throw ApiException(409, "channel_keys_missing", "This channel's keys aren't set up yet. Ask its owner to open it.")
        val key = ring.keys[epoch] ?: throw ApiException(409, "channel_key_unreadable", LOCKED_MESSAGE)
        return epoch to key
    }

    // --- Reading ---

    /** Opens each encrypted post (plain ones pass through); one this link's keys can't open comes back [ChannelPostDto.locked]. */
    suspend fun openPosts(channelId: String, list: List<ChannelPostDto>, access: String?): List<ChannelPostDto> {
        if (list.none { it.encrypted != null }) return list
        val id = channelId.lowercase()
        var ring = ring(id, access)
        if (list.any { p -> p.encrypted?.let { ring?.keys?.containsKey(it.keyEpoch) != true } == true }) ring = ring(id, access, true) ?: ring
        return list.map { open(id, it, ring) }
    }

    suspend fun openPost(channelId: String, post: ChannelPostDto, access: String?): ChannelPostDto = openPosts(channelId, listOf(post), access).single()

    private fun open(channelId: String, post: ChannelPostDto, ring: Ring?): ChannelPostDto {
        val enc = post.encrypted ?: return post
        val key = ring?.keys?.get(enc.keyEpoch)
        val plain = key?.let { crypto.openPost(enc.nonce, enc.ciphertext, it, channelId, enc.keyEpoch) }?.let(ChannelSealedPosts::decode)
            ?: return post.copy(locked = true)
        posts[post.id.lowercase()] = Opened(channelId, plain, post.media.map { it.id }, sealed = true)
        return view(post, plain)
    }

    /** The plain shapes the screens show, from the opened post and the counts the server keeps. */
    private fun view(post: ChannelPostDto, plain: SealedPostPlain): ChannelPostDto {
        val enc = post.encrypted
        val content = plain.content
        val poll = content?.poll?.let { p ->
            val s = enc?.poll
            val closed = s?.closed ?: false
            val quiz = s?.quiz ?: (p.quiz != null)
            ChannelPollDto(
                question = p.question,
                options = p.options,
                multiple = s?.multiple ?: p.multiple,
                quiz = quiz,
                closed = closed,
                counts = s?.counts?.takeIf { it.size == p.options.size } ?: List(p.options.size) { 0 },
                voters = s?.voters ?: 0,
                // As for plain quizzes: the right answer shows once it's closed (or in the answer to your own vote).
                correct = if (quiz && closed) p.quiz?.correct else null,
                explanation = if (quiz && closed) p.quiz?.explanation?.ifEmpty { null } else null,
                optionCount = s?.optionCount,
            )
        }
        val checklist = content?.checklist?.let { c ->
            val done = enc?.checklist?.done.orEmpty()
            ChannelChecklistDto(
                title = c.title,
                items = c.items.mapIndexed { i, text -> ChannelChecklistItemDto(text, done.getOrElse(i) { false }) },
                collaborative = enc?.checklist?.collaborative ?: c.collaborative,
            )
        }
        val byId = post.media.associateBy { it.id.lowercase() }
        // Only what the post names: thumbnails another app uploaded sit in the list too, but aren't shown.
        val media = content?.media.orEmpty().mapNotNull { item -> byId[item.id.lowercase()]?.let { mediaView(it, item) } }
        return post.copy(body = plain.body, poll = poll, checklist = checklist, media = media)
    }

    private fun mediaView(server: ChannelMediaDto, item: SealedMediaItem): ChannelMediaDto? {
        val key = runCatching { Base64Codec.decode(item.fileKey) }.getOrNull() ?: return null
        val header = runCatching { Base64Codec.decode(item.header) }.getOrNull() ?: return null
        return server.copy(
            kind = item.kind,
            mime = item.mime,
            size = item.size,
            filename = item.filename,
            durationMs = item.durationMs,
            width = item.width,
            height = item.height,
            sealed = SealedMedia(key, header, item.encryptedSize, item.ciphertextSha256, item.size),
        )
    }

    /** An encrypted poll's answer to a vote, with its question, options and (once answered) a quiz's answer filled in. */
    fun fillVote(postId: String, vote: ChannelVoteDto): ChannelVoteDto {
        if (vote.poll.optionCount == null) return vote
        val p = posts[postId.lowercase()]?.plain?.content?.poll ?: return vote
        val reveal = vote.poll.quiz && (vote.poll.closed || vote.mine.isNotEmpty())
        return vote.copy(
            poll = vote.poll.copy(
                question = p.question,
                options = p.options,
                correct = if (reveal) p.quiz?.correct else null,
                explanation = if (reveal) p.quiz?.explanation?.ifEmpty { null } else null,
            ),
        )
    }

    /** An encrypted checklist's answer to a tick, with its title and items filled in. */
    fun fillChecklist(postId: String, list: ChannelChecklistDto): ChannelChecklistDto {
        val done = list.done ?: return list
        val c = posts[postId.lowercase()]?.plain?.content?.checklist ?: return list
        return list.copy(title = c.title, items = c.items.mapIndexed { i, text -> ChannelChecklistItemDto(text, done.getOrElse(i) { false }) })
    }

    // --- Posting ---

    /** A post: sealed under the newest epoch in a private channel, as it is in a public one. */
    suspend fun post(channelId: String, request: ChannelPostRequest, access: String? = null): ChannelPostDto {
        val id = channelId.lowercase()
        if (!isPrivate(id)) return repository.post(id, request)
        val created = withFreshKeys(id, access) { epoch, key -> repository.postEncrypted(id, sealRequest(id, request, epoch, key)) }
        request.media.orEmpty().forEach { uploads.remove(it.lowercase()) }
        return openPost(id, created, access)
    }

    /** The owner edits a private channel's post: its text, re-sealed under the newest epoch; the rest stays. */
    suspend fun edit(channelId: String, postId: String, body: String): ChannelPostDto {
        val id = channelId.lowercase()
        val opened = posts[postId.lowercase()] ?: throw ApiException(409, "post_not_open", "This post can't be edited from this phone.")
        val plain = opened.plain.copy(body = body)
        val replaced = withFreshKeys(id, null) { epoch, key ->
            repository.replacePostEncrypted(id, postId, sealed(id, plain, epoch, key, opened.mediaIds, null))
        }
        return openPost(id, replaced, null)
    }

    /** Seals the composer's plain request: the server gets only the counts it needs for votes and ticks. */
    private fun sealRequest(channelId: String, request: ChannelPostRequest, epoch: Int, key: ByteArray): ChannelEncryptedPostRequest {
        val mediaIds = request.media.orEmpty()
        val items = mediaIds.map { mid ->
            uploads[mid.lowercase()] ?: throw ApiException(409, "media_not_sealed", "An attachment wasn't encrypted on this phone. Remove it and add it again.")
        }
        val content = SealedContent(
            poll = request.poll?.let { SealedPoll(it.question, it.options, it.multiple, it.quiz?.let { q -> SealedQuiz(q.correct, q.explanation.ifEmpty { null }) }) },
            checklist = request.checklist?.let { SealedChecklist(it.title, it.items, it.collaborative) },
            media = items.ifEmpty { null },
        ).takeIf { it.poll != null || it.checklist != null || it.media != null }
        return sealed(channelId, SealedPostPlain(ChannelSealedPosts.VERSION, request.body, content), epoch, key, mediaIds, request.publishAt)
    }

    private fun sealed(channelId: String, plain: SealedPostPlain, epoch: Int, key: ByteArray, mediaIds: List<String>, publishAt: String?): ChannelEncryptedPostRequest {
        val s = crypto.sealPost(ChannelSealedPosts.encode(plain), key, channelId, epoch)
        val poll = plain.content?.poll
        val list = plain.content?.checklist
        return ChannelEncryptedPostRequest(
            keyEpoch = s.keyEpoch,
            nonce = s.nonce,
            ciphertext = s.ciphertext,
            poll = poll?.let { ChannelPollShape(it.options.size, it.multiple.takeIf { m -> m }, (it.quiz != null).takeIf { q -> q }) },
            checklist = list?.let { ChannelChecklistShape(it.items.size, it.collaborative.takeIf { c -> c }) },
            media = mediaIds.ifEmpty { null },
            publishAt = publishAt,
        )
    }

    /**
     * Runs [send] with the newest key; if the server says another epoch is newest (a reset on another device),
     * reads the keys again and tries once more. A private channel made before C1 has its keys set up first,
     * when this is its owner.
     */
    private suspend fun <T> withFreshKeys(channelId: String, access: String?, send: suspend (Int, ByteArray) -> T): T {
        val first = try {
            newest(channelId, access)
        } catch (e: ApiException) {
            if (e.code != "channel_keys_missing" || !owned.containsKey(channelId)) throw e
            setUpKeys(channelId)
            newest(channelId, access)
        }
        return try {
            send(first.first, first.second)
        } catch (e: ApiException) {
            when (e.code) {
                "stale_key_epoch" -> {
                    if (owned.containsKey(channelId)) refreshOwned(channelId)
                    val again = newest(channelId, access, refresh = true)
                    send(again.first, again.second)
                }
                "channel_keys_missing" -> {
                    if (!owned.containsKey(channelId)) throw e
                    setUpKeys(channelId)
                    val again = newest(channelId, access, refresh = true)
                    send(again.first, again.second)
                }
                else -> throw e
            }
        }
    }

    // --- Media ---

    /**
     * A private channel's upload: the bytes are sealed on this phone with their own key (FileCrypto, 64 KiB
     * chunks) before anything is sent; the server is told only the encrypted size. The key and the file's real
     * details wait here until the post (or suggestion) that uses it seals them in.
     */
    suspend fun sealUpload(
        channelId: String,
        open: () -> InputStream,
        meta: ChannelMediaStartRequest,
        forSuggestion: Boolean,
        access: String?,
        onProgress: (Float) -> Unit = {},
    ): String {
        val id = channelId.lowercase()
        workDir.mkdirs()
        val temp = File(workDir, "seal-" + UUID.randomUUID())
        try {
            val enc = withContext(Dispatchers.IO) { open().use { fileCrypto.encryptFile(it, temp) } }
            val start = ChannelMediaStartRequest(size = enc.encryptedSize, encrypted = true)
            val started = if (forSuggestion) repository.startSuggestionMedia(id, start, access) else repository.startMedia(id, start)
            transfer.send(started.uploadUrl, temp, onProgress)
            uploads[started.id.lowercase()] = SealedMediaItem(
                id = started.id.lowercase(),
                kind = meta.kind ?: "file",
                mime = meta.mime ?: "application/octet-stream",
                filename = meta.filename,
                size = enc.plaintextSize,
                durationMs = meta.durationMs,
                width = meta.width,
                height = meta.height,
                fileKey = Base64Codec.encode(enc.key),
                header = Base64Codec.encode(enc.header),
                encryptedSize = enc.encryptedSize,
                ciphertextSha256 = enc.ciphertextSha256,
            )
            enc.key.fill(0)
            return started.id
        } finally {
            temp.delete()
        }
    }

    // --- Suggestions ---

    suspend fun suggest(channelId: String, body: String, mediaIds: List<String>, access: String?): ChannelSuggestionDto {
        val id = channelId.lowercase()
        if (!isPrivate(id)) return repository.suggest(id, body, mediaIds, access)
        val items = mediaIds.map { uploads[it.lowercase()] ?: throw ApiException(409, "media_not_sealed", "A photo wasn't encrypted on this phone. Remove it and add it again.") }
        val plain = SealedPostPlain(ChannelSealedPosts.VERSION, body, items.takeIf { it.isNotEmpty() }?.let { SealedContent(media = it) })
        val sent = withFreshKeys(id, access) { epoch, key ->
            val s = crypto.sealPost(ChannelSealedPosts.encode(plain), key, id, epoch)
            repository.suggestEncrypted(id, ChannelEncryptedSuggestionRequest(s.keyEpoch, s.nonce, s.ciphertext, mediaIds.ifEmpty { null }), access)
        }
        mediaIds.forEach { uploads.remove(it.lowercase()) }
        return openSuggestions(id, listOf(sent), access).single()
    }

    /** Opens encrypted suggestions (the owner's list, or your own); plain ones pass through. */
    suspend fun openSuggestions(channelId: String, list: List<ChannelSuggestionDto>, access: String?): List<ChannelSuggestionDto> {
        val id = channelId.lowercase()
        for (s in list) if (s.encrypted == null) suggestions[s.id.lowercase()] = Opened(id, SealedPostPlain(ChannelSealedPosts.VERSION, s.body, null), s.media.map { it.id }, sealed = false)
        if (list.none { it.encrypted != null }) return list
        var ring = ring(id, access)
        if (list.any { s -> s.encrypted?.let { ring?.keys?.containsKey(it.keyEpoch) != true } == true }) ring = ring(id, access, true) ?: ring
        return list.map { s ->
            val enc = s.encrypted ?: return@map s
            val plain = ring?.keys?.get(enc.keyEpoch)
                ?.let { crypto.openPost(enc.nonce, enc.ciphertext, it, id, enc.keyEpoch) }
                ?.let(ChannelSealedPosts::decode)
                ?: return@map s.copy(locked = true)
            suggestions[s.id.lowercase()] = Opened(id, plain, s.media.map { it.id }, sealed = true)
            val byId = s.media.associateBy { it.id.lowercase() }
            s.copy(body = plain.body, media = plain.content?.media.orEmpty().mapNotNull { item -> byId[item.id.lowercase()]?.let { mediaView(it, item) } })
        }
    }

    /**
     * The owner publishes a suggestion, with their edit of its text ([body], or null to keep it). In a private
     * channel the owner's phone re-seals it under the newest epoch (its photos' keys carried over); in a public
     * one, a suggestion left encrypted from before goes out as its opened text.
     */
    suspend fun approve(channelId: String, suggestionId: String, body: String?, publishAt: String?): ChannelPostDto {
        val id = channelId.lowercase()
        val opened = suggestions[suggestionId.lowercase()]
        if (!isPrivate(id)) {
            val text = body ?: opened?.takeIf { it.sealed }?.plain?.body
            return repository.approveSuggestion(id, suggestionId, text, publishAt)
        }
        val plain = opened?.plain ?: throw ApiException(409, "suggestion_not_open", "This suggestion can't be opened on this phone.")
        val edited = if (body != null) plain.copy(body = body) else plain
        val post = withFreshKeys(id, null) { epoch, key ->
            val s = crypto.sealPost(ChannelSealedPosts.encode(edited), key, id, epoch)
            repository.approveSuggestionEncrypted(id, suggestionId, ChannelEncryptedApproveRequest(s.keyEpoch, s.nonce, s.ciphertext, publishAt))
        }
        return openPost(id, post, null)
    }

    // --- The owner: visibility, link resets and keys ---

    private suspend fun refreshOwned(channelId: String): ChannelDto? {
        val list = repository.mine()
        list.forEach(::rememberOwned)
        return list.firstOrNull { it.id.equals(channelId, ignoreCase = true) }
    }

    private suspend fun ownedChannel(channelId: String): ChannelDto =
        owned[channelId.lowercase()] ?: refreshOwned(channelId) ?: throw ApiException(403, "forbidden", "Only the channel's owner can do that.")

    /**
     * Going private, or resetting the link of a private channel: a new link, and a new epoch n+1 whose key
     * seals posts from now on. Every older epoch's key is re-wrapped under the new link too, so the new link
     * reads the whole history while the old link reads nothing new (and the server refuses it anyway).
     * Returns the owner's channel and the new link's secret.
     */
    suspend fun makePrivate(channelId: String): Pair<ChannelDto, String> = lock(channelId).withLock {
        val id = channelId.lowercase()
        var attempt = 0
        while (true) {
            val channel = if (attempt == 0) ownedChannel(id) else refreshOwned(id) ?: ownedChannel(id)
            val n = channel.keyEpoch ?: 0
            // The keys so far, opened with the current link (or, while public, the last one).
            val old = if (n > 0) ring(id, null, refresh = true)?.keys.orEmpty() else emptyMap()
            val keys = HashMap<Int, ByteArray>()
            // An epoch whose key can't be opened any more gets a fresh one: nothing under it could be read anyway.
            for (e in 1..n) keys[e] = old[e] ?: crypto.newContentKey()
            keys[n + 1] = crypto.newContentKey()
            val fresh = crypto.newSecret()
            val wrapKey = crypto.wrapKey(fresh)
            val wrapped = try {
                keys.toSortedMap().map { (e, k) -> ChannelWrappedKeyDto(e, crypto.wrapContentKey(k, id, e, wrapKey)) }
            } finally {
                wrapKey.fill(0)
            }
            try {
                val updated = repository.setVisibility(
                    id,
                    ChannelVisibilityRequest(true, crypto.accessToken(fresh), crypto.sealSecret(fresh, ownerPublicKey()), wrapped),
                )
                rings[id] = Ring(fresh, n + 1, keys)
                rememberOwned(updated)
                return@withLock updated to fresh
            } catch (e: ApiException) {
                // Another device reset the link first: start again from what's stored now.
                if (e.code != "wrong_key_epochs" || attempt++ > 0) throw e
            }
        }
        @Suppress("UNREACHABLE_CODE")
        error("unreachable")
    }

    /** Going public: the link stops working; the keys stay with the owner so the old posts can be converted. */
    suspend fun makePublic(channelId: String): ChannelDto = lock(channelId).withLock {
        val updated = repository.setVisibility(channelId.lowercase(), ChannelVisibilityRequest(isPrivate = false))
        rememberOwned(updated)
        updated
    }

    /**
     * A private channel made before C1 (no keys yet): epoch 1, sent with the same link (its access token and
     * sealed secret), so followers' links keep working.
     */
    private suspend fun setUpKeys(channelId: String) {
        val id = channelId.lowercase()
        val channel = refreshOwned(id) ?: ownedChannel(id)
        if (!channel.isPrivate || channel.keyEpoch != null) return
        val sealed = channel.secretSealed ?: throw ApiException(409, "channel_keys_missing", "This channel's link can't be opened on this phone.")
        val secret = crypto.openSecret(sealed, ownerPublicKey(), ownerPrivateKey())
            ?: throw ApiException(409, "channel_keys_missing", "This channel's link can't be opened on this phone.")
        val key = crypto.newContentKey()
        val wrapKey = crypto.wrapKey(secret)
        val wrapped = try {
            crypto.wrapContentKey(key, id, 1, wrapKey)
        } finally {
            wrapKey.fill(0)
        }
        val updated = repository.setVisibility(
            id,
            ChannelVisibilityRequest(true, crypto.accessToken(secret), sealed, listOf(ChannelWrappedKeyDto(1, wrapped))),
        )
        rings[id] = Ring(secret, 1, mapOf(1 to key))
        rememberOwned(updated)
    }

    /**
     * What the owner's app does on opening their channel: sets up keys for a private channel made before C1,
     * then converts what's left in the old format. Safe to call again; each round carries on where the last
     * stopped.
     */
    suspend fun maintain(channelId: String, onProgress: (Int) -> Unit = {}): ConversionResult {
        val id = channelId.lowercase()
        lock(id).withLock {
            val channel = refreshOwned(id) ?: return ConversionResult.Done(0)
            if (channel.isPrivate && channel.keyEpoch == null) setUpKeys(id)
        }
        return convert(id, onProgress)
    }

    /**
     * Converts the posts still in the other format after a visibility switch, oldest first, page by page:
     * plain posts in a private channel are sealed under the newest epoch (their media downloaded and uploaded
     * again encrypted); encrypted posts in a public channel are opened and stored as plain text (their media
     * uploaded again as they are). Rate limits are waited out a few times, then the round stops for later.
     */
    suspend fun convert(channelId: String, onProgress: (Int) -> Unit = {}): ConversionResult = lock(channelId).withLock {
        val id = channelId.lowercase()
        var converted = 0
        var waits = 0
        while (true) {
            val page = try {
                repository.conversion(id)
            } catch (e: ApiException) {
                if (e.status == 429 && waits++ < MAX_WAITS) {
                    pause(RATE_LIMIT_WAIT_MS)
                    continue
                }
                return@withLock ConversionResult.Paused(converted, e.message ?: "Couldn't read the posts to convert.")
            }
            owned[id]?.let { remember(it.copy(isPrivate = page.isPrivate, keyEpoch = if (page.isPrivate) page.keyEpoch else it.keyEpoch)) }
            if (page.posts.isEmpty()) return@withLock ConversionResult.Done(converted)
            for (post in page.posts) {
                while (true) {
                    try {
                        if (page.isPrivate) sealOld(id, post) else openOld(id, post)
                        converted++
                        onProgress(converted)
                        break
                    } catch (e: ApiException) {
                        if (e.status == 429 && waits++ < MAX_WAITS) {
                            pause(RATE_LIMIT_WAIT_MS)
                            continue
                        }
                        return@withLock ConversionResult.Paused(converted, e.message ?: "Converting stopped. It carries on next time.")
                    } catch (e: Exception) {
                        if (e is kotlinx.coroutines.CancellationException) throw e
                        return@withLock ConversionResult.Paused(converted, "Converting stopped. It carries on next time.")
                    }
                }
            }
        }
        @Suppress("UNREACHABLE_CODE")
        error("unreachable")
    }

    /** A plain post in a now-private channel: sealed under the newest epoch, its media re-uploaded encrypted. */
    private suspend fun sealOld(channelId: String, post: ChannelPostDto) {
        if (post.encrypted != null) return
        val items = post.media.map { m ->
            val plainFile = File(workDir.apply { mkdirs() }, "plain-" + UUID.randomUUID())
            try {
                transfer.fetch(m.url, plainFile)
                val newId = sealUpload(
                    channelId,
                    { plainFile.inputStream() },
                    ChannelMediaStartRequest(m.kind, m.mime, plainFile.length(), m.filename, m.durationMs, m.width, m.height),
                    forSuggestion = false,
                    access = null,
                )
                uploads.remove(newId.lowercase())!!
            } finally {
                plainFile.delete()
            }
        }
        val poll = post.poll?.let { p ->
            // The conversion route gives the owner an open quiz's answer too, so it goes into the sealed copy.
            // (A server from before that fix leaves it out: the quiz then keeps its shape without an answer.)
            SealedPoll(p.question, p.options, p.multiple, if (p.quiz) p.correct?.let { SealedQuiz(it, p.explanation) } else null)
        }
        val content = SealedContent(
            poll = poll,
            checklist = post.checklist?.let { c -> SealedChecklist(c.title, c.items.map { it.text }, c.collaborative) },
            media = items.ifEmpty { null },
        ).takeIf { it.poll != null || it.checklist != null || it.media != null }
        val plain = SealedPostPlain(ChannelSealedPosts.VERSION, post.body, content)
        val mediaIds = items.map { it.id }
        withFreshKeys(channelId, null) { epoch, key ->
            val request = sealed(channelId, plain, epoch, key, mediaIds, null)
            // A quiz without its answer still has to keep the quiz flag.
            val shaped = if (post.poll?.quiz == true && poll?.quiz == null) request.copy(poll = request.poll?.copy(quiz = true)) else request
            repository.replacePostEncrypted(channelId, post.id, shaped)
        }
    }

    /** An encrypted post in a now-public channel: opened and stored as plain text, its media re-uploaded as they are. */
    private suspend fun openOld(channelId: String, post: ChannelPostDto) {
        val enc = post.encrypted ?: return
        val ring = ring(channelId, null)?.takeIf { it.keys.containsKey(enc.keyEpoch) } ?: ring(channelId, null, refresh = true)
        val key = ring?.keys?.get(enc.keyEpoch) ?: throw IllegalStateException("locked")
        val plain = crypto.openPost(enc.nonce, enc.ciphertext, key, channelId, enc.keyEpoch)?.let(ChannelSealedPosts::decode)
            ?: throw IllegalStateException("locked")
        val content = plain.content
        val byId = post.media.associateBy { it.id.lowercase() }
        val newIds = content?.media.orEmpty().mapNotNull { item ->
            val server = byId[item.id.lowercase()] ?: return@mapNotNull null
            val sealedFile = File(workDir.apply { mkdirs() }, "sealed-" + UUID.randomUUID())
            val plainFile = File(workDir, "open-" + UUID.randomUUID())
            try {
                transfer.fetch(server.url, sealedFile)
                withContext(Dispatchers.IO) {
                    sealedFile.inputStream().buffered().use { input ->
                        plainFile.outputStream().buffered().use { out ->
                            fileCrypto.decrypt(input, out, Base64Codec.decode(item.fileKey), Base64Codec.decode(item.header), item.encryptedSize, item.ciphertextSha256)
                        }
                    }
                }
                val started = repository.startMedia(
                    channelId,
                    ChannelMediaStartRequest(item.kind, item.mime, plainFile.length(), item.filename, item.durationMs, item.width, item.height),
                )
                transfer.send(started.uploadUrl, plainFile)
                started.id
            } finally {
                sealedFile.delete()
                plainFile.delete()
            }
        }
        val request = ChannelPostRequest(
            body = plain.body,
            poll = content?.poll?.let { p ->
                // A quiz has to name its answer to go plain; only one sealed by an older server's conversion lacks it (then the first).
                val quiz = if (enc.poll?.quiz == true || p.quiz != null) ChannelQuizRequest(p.quiz?.correct ?: 0, p.quiz?.explanation.orEmpty()) else null
                ChannelPollRequest(p.question, p.options, p.multiple, quiz)
            },
            checklist = content?.checklist?.let { ChannelChecklistRequest(it.title, it.items, it.collaborative) },
            media = newIds.ifEmpty { null },
        )
        repository.replacePost(channelId, post.id, request)
    }

    /** Forgets every key and opened post (the session ending). */
    fun clear() {
        rings.values.forEach { r -> r.keys.values.forEach { it.fill(0) } }
        rings.clear()
        links.clear()
        owned.clear()
        known.clear()
        posts.clear()
        suggestions.clear()
        uploads.clear()
    }

    companion object {
        /** What a reader sees for a post their link's keys can't open. */
        const val LOCKED_MESSAGE = "This channel's posts can't be opened with this link."
        private const val RATE_LIMIT_WAIT_MS = 60_000L
        private const val MAX_WAITS = 5
    }
}
