// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ChannelApi
import com.redvaultsystems.redvaultmessage.data.api.ChannelApproveRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelAvatarRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelCheckRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistItemDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelConversionDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelDetailsRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelEncryptedApproveRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelEncryptedDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelEncryptedPostRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelEncryptedSuggestionRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelKeysDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaStartRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelPollDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelReportRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelSealedChecklistDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelSealedDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelSealedPollDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelSettingsRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelSuggestionDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelSuggestionRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelUploadDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelViewsRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelVisibilityRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelVoteDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelVoteRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelWrappedKeyDto
import com.redvaultsystems.redvaultmessage.data.api.PublicChannelApi
import java.io.File
import java.time.Instant
import java.util.UUID

/**
 * An in-memory stand-in for server/src/services/channelService.ts's Part C1 rules
 * (docs/signal-integration/10-part-c1-private-channels.md, "API (as built)"), enough to drive PrivateChannels
 * end to end off a phone: access tokens, wrapped keys and epochs, encrypted and plain posts and the rules
 * between them, edits and conversions, votes and ticks by index, suggestions, and opaque media.
 */
class FakeChannelServer {
    class Channel(
        val id: String,
        val ownerId: String,
        var name: String,
        var isPrivate: Boolean = false,
        var access: String? = null,
        var secretSealed: String? = null,
        var formerSecretSealed: String? = null,
        var keyEpoch: Int? = null,
        var keys: Map<Int, String> = emptyMap(),
        var acceptSuggestions: Boolean = true,
    )

    class Post(
        val id: String,
        val channelId: String,
        val createdAt: String,
        var published: Boolean,
        var body: String,
        var poll: ChannelPollDto? = null,
        var quizCorrect: Int? = null,
        var quizExplanation: String? = null,
        var checklist: ChannelChecklistDto? = null,
        var media: List<String> = emptyList(),
        var encrypted: ChannelEncryptedDto? = null,
        val suggested: Boolean = false,
        val votes: MutableMap<String, List<Int>> = HashMap(),
    )

    class Media(
        val id: String,
        val channelId: String,
        val ownerId: String,
        val encrypted: Boolean,
        val kind: String,
        val mime: String,
        val size: Long,
        val filename: String?,
        var bytes: ByteArray? = null,
        var used: Boolean = false,
    )

    class Suggestion(val id: String, val channelId: String, val authorId: String, val body: String, val encrypted: ChannelSealedDto?, val media: List<String>, var status: String = "pending")

    val channels = LinkedHashMap<String, Channel>()
    val posts = ArrayList<Post>()
    val media = LinkedHashMap<String, Media>()
    val suggestionsList = ArrayList<Suggestion>()
    /** The next call to the named operation fails with this (rate limits and the like). */
    val failNext = HashMap<String, ApiException>()
    private var clock = Instant.parse("2026-11-08T08:00:00Z")

    private fun now(): String {
        clock = clock.plusSeconds(1)
        return clock.toString()
    }

    /** The named operation answers 429 this many more times. */
    val refusals = HashMap<String, Int>()

    private fun fail(op: String) {
        failNext.remove(op)?.let { throw it }
        val left = refusals[op] ?: 0
        if (left > 0) {
            refusals[op] = left - 1
            throw ApiException(429, "rate_limited", "Too many requests")
        }
    }
    private fun notFound(): Nothing = throw ApiException(404, "not_found", "This channel doesn't exist, was deleted, or is private.")
    private fun conflict(code: String): Nothing = throw ApiException(409, code, code)

    private fun channel(id: String) = channels[id.lowercase()] ?: notFound()

    private fun canRead(c: Channel, access: String?) = !c.isPrivate || (access != null && access == c.access)

    private fun readerDto(c: Channel) = ChannelDto(c.id, c.name, isPrivate = c.isPrivate, keyEpoch = if (c.isPrivate) c.keyEpoch else null, acceptSuggestions = c.acceptSuggestions)

    private fun ownerDto(c: Channel) = readerDto(c).copy(secretSealed = c.secretSealed, formerSecretSealed = if (c.isPrivate) null else c.formerSecretSealed, keyEpoch = c.keyEpoch)

    private fun mediaView(id: String) = media[id]!!.let { m ->
        if (m.encrypted) ChannelMediaDto(m.id, "file", "application/octet-stream", m.size, null, url = "/api/channel-media/${m.id}?exp=1&sig=x")
        else ChannelMediaDto(m.id, m.kind, m.mime, m.size, m.filename, url = "/api/channel-media/${m.id}?exp=1&sig=x")
    }

    /** [revealQuiz]: the owner's conversion route shows an open quiz's answer too (other routes only once closed). */
    fun dto(p: Post, revealQuiz: Boolean = false): ChannelPostDto {
        val enc = p.encrypted?.let { e ->
            val poll = e.poll?.let { s ->
                val counts = MutableList(s.optionCount) { 0 }
                p.votes.values.flatten().forEach { counts[it]++ }
                s.copy(counts = counts, voters = p.votes.size)
            }
            e.copy(poll = poll)
        }
        val poll = p.poll?.let { s ->
            val counts = MutableList(s.options.size) { 0 }
            p.votes.values.flatten().forEach { counts[it]++ }
            s.copy(counts = counts, voters = p.votes.size, correct = if (s.quiz && (s.closed || revealQuiz)) p.quizCorrect else null,
                explanation = if (s.quiz && (s.closed || revealQuiz)) p.quizExplanation else null)
        }
        return ChannelPostDto(p.id, p.body, p.createdAt, suggested = p.suggested, poll = poll, checklist = p.checklist, media = p.media.map(::mediaView), encrypted = enc)
    }

    /** A channel made private before Part C1: a link and sealed secret, but no keys. */
    fun oldPrivate(ownerId: String, name: String, access: String, secretSealed: String): Channel {
        val c = Channel(UUID.randomUUID().toString(), ownerId, name, isPrivate = true, access = access, secretSealed = secretSealed)
        channels[c.id] = c
        return c
    }

    /** A plain post stored straight on the server (as one made before C1). */
    fun plainPost(channelId: String, body: String): Post = Post(UUID.randomUUID().toString(), channelId, now(), true, body).also(posts::add)

    // --- Media bytes (what ChannelMediaClient does over HTTP) ---

    val transfer = object : ChannelMediaTransfer {
        override suspend fun fetch(url: String, into: File) {
            val id = url.removePrefix("/api/channel-media/").substringBefore('?')
            into.writeBytes(media[id]?.bytes ?: throw ApiException(404, "not_found", "gone"))
        }

        override suspend fun send(uploadUrl: String, file: File, onProgress: (Float) -> Unit) {
            fail("send")
            val m = media[uploadUrl.removePrefix("/api/channel-uploads/")] ?: throw ApiException(404, "not_found", "gone")
            val bytes = file.readBytes()
            if (bytes.size.toLong() != m.size) throw ApiException(400, "validation_error", "size")
            m.bytes = bytes
            onProgress(1f)
        }
    }

    private fun startUpload(c: Channel, userId: String, body: ChannelMediaStartRequest): ChannelUploadDto {
        val m = if (c.isPrivate) {
            if (body.encrypted != true) conflict("encryption_required")
            if (body.kind != null || body.mime != null || body.filename != null) throw ApiException(400, "validation_error", "strict")
            Media(UUID.randomUUID().toString(), c.id, userId, true, "file", "application/octet-stream", body.size, null)
        } else {
            if (body.encrypted == true) conflict("plain_text_required")
            Media(UUID.randomUUID().toString(), c.id, userId, false, body.kind!!, body.mime!!, body.size, body.filename)
        }
        media[m.id] = m
        return ChannelUploadDto(m.id, "file", m.size, "/api/channel-uploads/${m.id}")
    }

    private fun takeMedia(c: Channel, userId: String, ids: List<String>, keep: List<String> = emptyList()) {
        for (id in ids) {
            if (id in keep) continue
            val m = media[id] ?: throw ApiException(400, "validation_error", "unknown media")
            if (m.used || m.ownerId != userId || m.channelId != c.id || m.bytes == null) throw ApiException(400, "validation_error", "media not usable")
            if (m.encrypted != c.isPrivate) throw ApiException(400, "validation_error", "media in the other format")
        }
        ids.forEach { media[it]!!.used = true }
    }

    private fun checkSealed(c: Channel, keyEpoch: Int, nonce: String, ciphertext: String) {
        if (!c.isPrivate) conflict("plain_text_required")
        val newest = c.keyEpoch ?: conflict("channel_keys_missing")
        if (keyEpoch != newest) throw ApiException(409, "stale_key_epoch", "stale")
        if (Base64Codec.decode(nonce).size != 24 || Base64Codec.decode(ciphertext).size < 16) throw ApiException(400, "validation_error", "sizes")
    }

    private fun sealedMeta(body: ChannelEncryptedPostRequest): ChannelEncryptedDto {
        val kind = when {
            body.poll != null -> "poll"
            body.checklist != null -> "checklist"
            !body.media.isNullOrEmpty() -> "media"
            else -> "text"
        }
        body.poll?.let { if (it.optionCount !in 2..10 || (it.multiple == true && it.quiz == true)) throw ApiException(400, "validation_error", "poll") }
        body.checklist?.let { if (it.itemCount !in 1..30) throw ApiException(400, "validation_error", "checklist") }
        return ChannelEncryptedDto(
            body.keyEpoch, body.nonce, body.ciphertext, kind,
            poll = body.poll?.let { ChannelSealedPollDto(it.optionCount, it.multiple == true, it.quiz == true) },
            checklist = body.checklist?.let { ChannelSealedChecklistDto(it.itemCount, it.collaborative == true, List(it.itemCount) { false }) },
        )
    }

    /** A post's shape for post_kind_changed: (kind, count, multiple, quiz). */
    private fun shape(p: Post): List<Any?> = p.encrypted?.let { e ->
        listOf(e.kind.takeIf { it == "poll" || it == "checklist" }, e.poll?.optionCount ?: e.checklist?.itemCount, e.poll?.multiple, e.poll?.quiz)
    } ?: listOf(if (p.poll != null) "poll" else if (p.checklist != null) "checklist" else null, p.poll?.options?.size ?: p.checklist?.items?.size, p.poll?.multiple, p.poll?.quiz)

    // --- Reading (no cookie) ---

    val public = object : PublicChannelApi {
        override suspend fun search(query: String?, limit: Int, offset: Int) = channels.values.filter { !it.isPrivate }.map(::readerDto)

        override suspend fun get(id: String, access: String?): ChannelDto = channel(id).let { if (canRead(it, access)) readerDto(it) else notFound() }

        override suspend fun posts(id: String, before: String?, limit: Int, access: String?): List<ChannelPostDto> {
            val c = channel(id)
            if (!canRead(c, access)) notFound()
            return posts.filter { it.channelId == c.id && it.published && (c.isPrivate || it.encrypted == null) && (before == null || it.createdAt < before) }
                .sortedByDescending { it.createdAt }.take(limit).map(::dto)
        }

        override suspend fun keys(id: String, access: String?): ChannelKeysDto {
            val c = channel(id)
            if (!c.isPrivate || access == null || access != c.access) notFound()
            return ChannelKeysDto(c.keyEpoch, c.keys.toSortedMap().map { ChannelWrappedKeyDto(it.key, it.value) })
        }

        override suspend fun views(body: ChannelViewsRequest) {}
    }

    // --- Signed in, as [userId] ---

    fun signedIn(userId: String): ChannelApi = object : ChannelApi {
        private fun own(id: String) = channel(id).also { if (it.ownerId != userId) throw ApiException(403, "forbidden", "Only the channel's owner can do that.") }
        private fun post(c: Channel, postId: String) = posts.firstOrNull { it.id == postId && it.channelId == c.id } ?: notFound()

        override suspend fun mine() = channels.values.filter { it.ownerId == userId }.map(::ownerDto)

        override suspend fun create(body: ChannelDetailsRequest): ChannelDto {
            val c = Channel(UUID.randomUUID().toString(), userId, body.name)
            channels[c.id] = c
            return ownerDto(c)
        }

        override suspend fun update(id: String, body: ChannelDetailsRequest) = own(id).also { it.name = body.name }.let(::ownerDto)

        override suspend fun setVisibility(id: String, body: ChannelVisibilityRequest): ChannelDto {
            val c = own(id)
            fail("visibility")
            if (!body.isPrivate) {
                if (c.isPrivate) c.formerSecretSealed = c.secretSealed
                c.isPrivate = false
                c.access = null
                c.secretSealed = null
                return ownerDto(c)
            }
            val keys = body.keys ?: throw ApiException(400, "keys_required", "keys")
            val n = c.keyEpoch ?: 0
            if (keys.map { it.epoch }.sorted() != (1..n + 1).toList()) throw ApiException(409, "wrong_key_epochs", "epochs")
            if (keys.any { Base64Codec.decode(it.wrapped).size != 72 }) throw ApiException(400, "validation_error", "wrapped")
            if (Base64Codec.decode(body.access!!).size != 32) throw ApiException(400, "validation_error", "access")
            c.isPrivate = true
            c.access = body.access
            c.secretSealed = body.secretSealed
            c.formerSecretSealed = null
            c.keys = keys.associate { it.epoch to it.wrapped }
            c.keyEpoch = n + 1
            return ownerDto(c)
        }

        override suspend fun keys(id: String, access: String?): ChannelKeysDto {
            val c = channel(id)
            if (c.ownerId != userId && !(c.isPrivate && access == c.access)) notFound()
            return ChannelKeysDto(c.keyEpoch, c.keys.toSortedMap().map { ChannelWrappedKeyDto(it.key, it.value) })
        }

        override suspend fun conversion(id: String, limit: Int): ChannelConversionDto {
            val c = own(id)
            fail("conversion")
            val left = posts.filter { it.channelId == c.id && (if (c.isPrivate) it.encrypted == null else it.encrypted != null) }.sortedBy { it.createdAt }
            return ChannelConversionDto(c.isPrivate, c.keyEpoch, left.take(limit).map { dto(it, revealQuiz = true) })
        }

        override suspend fun setSettings(id: String, body: ChannelSettingsRequest) = own(id).also { it.acceptSuggestions = body.acceptSuggestions }.let(::ownerDto)
        override suspend fun setAvatar(id: String, body: ChannelAvatarRequest): ChannelDto = TODO()
        override suspend fun removeAvatar(id: String) = TODO()
        override suspend fun delete(id: String) = TODO()

        override suspend fun startMedia(id: String, body: ChannelMediaStartRequest): ChannelUploadDto {
            fail("startMedia")
            return startUpload(own(id), userId, body)
        }

        override suspend fun post(id: String, body: ChannelPostRequest): ChannelPostDto {
            val c = own(id)
            if (c.isPrivate) conflict(if (c.keyEpoch == null) "encryption_required" else "encryption_required")
            takeMedia(c, userId, body.media.orEmpty())
            val p = Post(UUID.randomUUID().toString(), c.id, body.publishAt ?: now(), body.publishAt == null, body.body, media = body.media.orEmpty())
            body.poll?.let { p.poll = ChannelPollDto(it.question, it.options, it.multiple, it.quiz != null); p.quizCorrect = it.quiz?.correct; p.quizExplanation = it.quiz?.explanation }
            body.checklist?.let { p.checklist = ChannelChecklistDto(it.title, it.items.map { t -> ChannelChecklistItemDto(t) }, it.collaborative) }
            posts.add(p)
            return dto(p)
        }

        override suspend fun postEncrypted(id: String, body: ChannelEncryptedPostRequest): ChannelPostDto {
            val c = own(id)
            fail("postEncrypted")
            checkSealed(c, body.keyEpoch, body.nonce, body.ciphertext)
            takeMedia(c, userId, body.media.orEmpty())
            val p = Post(UUID.randomUUID().toString(), c.id, body.publishAt ?: now(), body.publishAt == null, "", media = body.media.orEmpty(), encrypted = sealedMeta(body))
            posts.add(p)
            return dto(p)
        }

        override suspend fun replacePost(id: String, postId: String, body: ChannelPostRequest): ChannelPostDto {
            val c = own(id)
            fail("replace")
            val p = post(c, postId)
            if (c.isPrivate) conflict("encryption_required")
            if (p.encrypted == null) conflict("post_not_encrypted")
            val before = shape(p)
            takeMedia(c, userId, body.media.orEmpty(), keep = p.media)
            val ticks = p.encrypted?.checklist?.done
            val closed = p.encrypted?.poll?.closed ?: false
            p.encrypted = null
            p.body = body.body
            p.poll = body.poll?.let { ChannelPollDto(it.question, it.options, it.multiple, it.quiz != null, closed) }
            p.quizCorrect = body.poll?.quiz?.correct
            p.quizExplanation = body.poll?.quiz?.explanation
            p.checklist = body.checklist?.let { l -> ChannelChecklistDto(l.title, l.items.mapIndexed { i, t -> ChannelChecklistItemDto(t, ticks?.getOrNull(i) ?: false) }, l.collaborative) }
            if (shape(p) != before) conflict("post_kind_changed")
            (p.media - body.media.orEmpty().toSet()).forEach { media.remove(it) }
            p.media = body.media.orEmpty()
            return dto(p)
        }

        override suspend fun replacePostEncrypted(id: String, postId: String, body: ChannelEncryptedPostRequest): ChannelPostDto {
            val c = own(id)
            fail("replace")
            val p = post(c, postId)
            checkSealed(c, body.keyEpoch, body.nonce, body.ciphertext)
            val before = shape(p)
            takeMedia(c, userId, body.media.orEmpty(), keep = p.media)
            val meta = sealedMeta(body)
            // State carried over: a poll's closed flag, a checklist's ticks.
            val closed = p.encrypted?.poll?.closed ?: p.poll?.closed ?: false
            val ticks = p.encrypted?.checklist?.done ?: p.checklist?.items?.map { it.done }
            p.encrypted = meta.copy(poll = meta.poll?.copy(closed = closed), checklist = meta.checklist?.copy(done = ticks ?: meta.checklist.done))
            p.body = ""
            p.poll = null
            p.checklist = null
            if (shape(p) != before) conflict("post_kind_changed")
            (p.media - body.media.orEmpty().toSet()).forEach { media.remove(it) }
            p.media = body.media.orEmpty()
            return dto(p)
        }

        override suspend fun scheduled(id: String) = own(id).let { c -> posts.filter { it.channelId == c.id && !it.published }.map(::dto) }

        override suspend fun publishNow(id: String, postId: String) = post(own(id), postId).also { it.published = true }.let(::dto)

        override suspend fun deletePost(id: String, postId: String) {
            posts.remove(post(own(id), postId))
        }

        override suspend fun vote(id: String, postId: String, body: ChannelVoteRequest, access: String?): ChannelVoteDto {
            val c = channel(id)
            if (!canRead(c, access)) notFound()
            val p = post(c, postId)
            if (!c.isPrivate && p.encrypted != null) notFound()
            val count = p.encrypted?.poll?.optionCount ?: p.poll?.options?.size ?: throw ApiException(400, "validation_error", "not a poll")
            if (body.options.any { it !in 0 until count }) throw ApiException(400, "validation_error", "index")
            p.votes[userId] = body.options
            val shown = dto(p)
            val poll = shown.encrypted?.poll?.let { s -> ChannelPollDto(multiple = s.multiple, quiz = s.quiz, closed = s.closed, counts = s.counts, voters = s.voters, optionCount = s.optionCount) }
                ?: shown.poll!!.copy(correct = p.quizCorrect.takeIf { shown.poll!!.quiz })
            return ChannelVoteDto(poll, body.options)
        }

        override suspend fun closePoll(id: String, postId: String): ChannelPostDto {
            val p = post(own(id), postId)
            p.encrypted?.let { e -> p.encrypted = e.copy(poll = e.poll?.copy(closed = true)) }
            p.poll?.let { p.poll = it.copy(closed = true) }
            return dto(p)
        }

        override suspend fun setChecklistItem(id: String, postId: String, index: Int, body: ChannelCheckRequest, access: String?): ChannelChecklistDto {
            val c = channel(id)
            if (!canRead(c, access)) notFound()
            val p = post(c, postId)
            val e = p.encrypted?.checklist ?: throw ApiException(400, "validation_error", "plain checklists aren't in this fake")
            if (index !in 0 until e.itemCount) throw ApiException(400, "validation_error", "index")
            if (c.ownerId != userId && !e.collaborative) throw ApiException(403, "forbidden", "not collaborative")
            val done = e.done.toMutableList().also { it[index] = body.done }
            p.encrypted = p.encrypted!!.copy(checklist = e.copy(done = done))
            return ChannelChecklistDto(collaborative = e.collaborative, itemCount = e.itemCount, done = done)
        }

        override suspend fun startSuggestionMedia(id: String, body: ChannelMediaStartRequest, access: String?): ChannelUploadDto {
            val c = channel(id)
            if (!canRead(c, access)) notFound()
            return startUpload(c, userId, body)
        }

        override suspend fun suggest(id: String, body: ChannelSuggestionRequest, access: String?): ChannelSuggestionDto {
            val c = channel(id)
            if (!canRead(c, access)) notFound()
            if (c.isPrivate) conflict("encryption_required")
            takeMedia(c, userId, body.media)
            return Suggestion(UUID.randomUUID().toString(), c.id, userId, body.body, null, body.media).also(suggestionsList::add).let(::suggestionDto)
        }

        override suspend fun suggestEncrypted(id: String, body: ChannelEncryptedSuggestionRequest, access: String?): ChannelSuggestionDto {
            val c = channel(id)
            if (!canRead(c, access)) notFound()
            checkSealed(c, body.keyEpoch, body.nonce, body.ciphertext)
            takeMedia(c, userId, body.media.orEmpty())
            val s = Suggestion(UUID.randomUUID().toString(), c.id, userId, "", ChannelSealedDto(body.keyEpoch, body.nonce, body.ciphertext), body.media.orEmpty())
            suggestionsList.add(s)
            return suggestionDto(s)
        }

        override suspend fun mySuggestions(id: String) = suggestionsList.filter { it.channelId == id && it.authorId == userId }.map(::suggestionDto)

        override suspend fun suggestions(id: String) = own(id).let { c -> suggestionsList.filter { it.channelId == c.id && it.status == "pending" }.map(::suggestionDto) }

        override suspend fun approveSuggestion(id: String, suggestionId: String, body: ChannelApproveRequest): ChannelPostDto {
            val c = own(id)
            if (c.isPrivate) conflict("encryption_required")
            val s = suggestionsList.first { it.id == suggestionId }
            s.status = "approved"
            return Post(UUID.randomUUID().toString(), c.id, now(), true, body.body ?: s.body, media = s.media, suggested = true).also(posts::add).let(::dto)
        }

        override suspend fun approveSuggestionEncrypted(id: String, suggestionId: String, body: ChannelEncryptedApproveRequest): ChannelPostDto {
            val c = own(id)
            checkSealed(c, body.keyEpoch, body.nonce, body.ciphertext)
            val s = suggestionsList.first { it.id == suggestionId }
            if (s.encrypted == null && s.media.isNotEmpty()) conflict("suggestion_format_mismatch")
            s.status = "approved"
            val kind = if (s.media.isNotEmpty()) "media" else "text"
            val p = Post(
                UUID.randomUUID().toString(), c.id, body.publishAt ?: now(), body.publishAt == null, "", media = s.media, suggested = true,
                encrypted = ChannelEncryptedDto(body.keyEpoch, body.nonce, body.ciphertext, kind),
            )
            posts.add(p)
            return dto(p)
        }

        override suspend fun rejectSuggestion(id: String, suggestionId: String) {
            suggestionsList.first { it.id == suggestionId }.status = "rejected"
        }

        override suspend fun report(id: String, body: ChannelReportRequest) = TODO()
    }

    private fun suggestionDto(s: Suggestion) = ChannelSuggestionDto(s.id, s.body, s.status, "2026-11-08T08:00:00Z", s.media.map(::mediaView), s.encrypted)
}
