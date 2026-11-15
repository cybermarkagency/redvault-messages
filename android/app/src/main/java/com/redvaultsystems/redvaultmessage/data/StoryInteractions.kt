// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.StoryAnswerCrypto
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.StoryAnswerRequest
import com.redvaultsystems.redvaultmessage.data.api.StoryInteractionsApi
import com.redvaultsystems.redvaultmessage.data.api.StoryTotalsRequest
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.model.AnswerableSticker
import com.redvaultsystems.redvaultmessage.data.model.InboxEntry
import com.redvaultsystems.redvaultmessage.data.model.PollSticker
import com.redvaultsystems.redvaultmessage.data.model.QuestionSticker
import com.redvaultsystems.redvaultmessage.data.model.QuizSticker
import com.redvaultsystems.redvaultmessage.data.model.ResultsVisibility
import com.redvaultsystems.redvaultmessage.data.model.SliderSticker
import com.redvaultsystems.redvaultmessage.data.model.StickerTotals
import com.redvaultsystems.redvaultmessage.data.model.StoryAnswer
import com.redvaultsystems.redvaultmessage.data.model.StoryTally
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import com.redvaultsystems.redvaultmessage.devicekeys.StoryAnswerMessage
import com.redvaultsystems.redvaultmessage.devicekeys.StoryDeviceKeys
import com.redvaultsystems.redvaultmessage.devicekeys.storyAnswerPayload
import java.time.Instant
import kotlinx.serialization.json.put

/** What this account already answered: null when it can't be read back (a question sent without a name can't be, even by its sender). */
class MyAnswer(val answer: StoryAnswer?, val anonymous: Boolean)

/** The answers sealed to you on one of your stories, opened. */
class Inbox(
    val entries: List<InboxEntry>,
    /** Answers that could not be opened or didn't fit their sticker (never counted). */
    val unreadable: Int,
)

/** The stickers of a story that somebody can answer. */
fun Story.answerableStickers(): List<AnswerableSticker> = meta.stickers.filterIsInstance<AnswerableSticker>()

/** A question marked "without my name" is sent with no sender inside the envelope. */
fun isAnonymousSticker(s: AnswerableSticker): Boolean = s is QuestionSticker && s.anonymous

/** Quiz and question answers are final: a second one is refused by the server. Polls and sliders can be changed. */
fun isFinalSticker(s: AnswerableSticker): Boolean = s is QuizSticker || s is QuestionSticker

/**
 * Answering stickers, reading what the author published, and for the author opening the answers and publishing totals (port of
 * ui/src/lib/storyInteractions.ts). The crypto is [StoryAnswerCrypto]; what the server learns is in docs/stories-advanced/.
 */
class StoryInteractions(private val api: StoryInteractionsApi, private val crypto: StoryAnswerCrypto) {

    suspend fun submit(identity: SelfIdentity, story: Story, authorPublicKey: ByteArray, sticker: AnswerableSticker, answer: StoryAnswer, deviceKeys: StoryDeviceKeys? = null) {
        if (story.deviceKeys) return submitOnDeviceKeys(identity, story, sticker, answer, deviceKeys ?: throw StoryInputException(NO_DEVICE_KEYS))
        val anonymous = isAnonymousSticker(sticker)
        val envelope = if (anonymous) {
            crypto.sealAnonymousAnswer(story.id, sticker, answer, authorPublicKey)
        } else {
            crypto.sealAnswer(story.id, sticker, answer, authorPublicKey, identity.privateKey)
        }
        apiCall(ApiClient.json) { api.putAnswer(story.id, sticker.id, StoryAnswerRequest(Base64Codec.encode(envelope), anonymous, isFinalSticker(sticker))) }
    }

    /**
     * Part D: a named answer goes as `storyanswer` to the author's devices (and this account's others), never to the
     * server; an anonymous one is sealed by the engine to each of the author's pinned devices (RVSD; each copy holds the
     * RVSA payload) and stored on the server as today.
     */
    private suspend fun submitOnDeviceKeys(identity: SelfIdentity, story: Story, sticker: AnswerableSticker, answer: StoryAnswer, deviceKeys: StoryDeviceKeys) {
        try {
            sendOnDeviceKeys(identity, story, sticker, answer, deviceKeys)
        } catch (e: com.redvaultsystems.redvaultmessage.devicekeys.EngineException) {
            // A code only, never the answer.
            throw StoryInputException(
                when (e.code) {
                    "answer_too_large" -> "That answer is too long."
                    "story_seal_unavailable" -> "Their devices can't take an answer without your name yet."
                    else -> "Couldn't send that over device keys. Try again."
                },
            )
        }
    }

    private suspend fun sendOnDeviceKeys(identity: SelfIdentity, story: Story, sticker: AnswerableSticker, answer: StoryAnswer, deviceKeys: StoryDeviceKeys) {
        val final = isFinalSticker(sticker)
        if (isAnonymousSticker(sticker)) {
            val sealed = deviceKeys.sealAnonymous(story.id, story.authorId, crypto.anonymousPayload(story.id, sticker, answer))
            apiCall(ApiClient.json) { api.putAnswer(story.id, sticker.id, StoryAnswerRequest(Base64Codec.encode(sealed), true, final)) }
            return
        }
        val prior = deviceKeys.answers(story.id).firstOrNull { it.stickerId == sticker.id && it.userId == identity.userId.lowercase() }
        if (prior != null && prior.final) throw StoryInputException("You already answered that.")
        val payload = storyAnswerPayload(crypto.answerJson(sticker, answer))
        deviceKeys.sendAnswer(story.authorId, StoryAnswerMessage(story.id.lowercase(), sticker.id, (prior?.rev ?: 0) + 1, final, payload))
    }

    /** What this account already answered on a story (so a reload or another phone shows it). */
    suspend fun mine(identity: SelfIdentity, story: Story, authorPublicKey: ByteArray, deviceKeys: StoryDeviceKeys? = null): Map<String, MyAnswer> {
        val stickers = story.answerableStickers().associateBy { it.id }
        val out = HashMap<String, MyAnswer>()
        if (story.deviceKeys) {
            // Named answers: as sent from this phone or this account's other devices. Anonymous ones: the server knows there is one.
            for (a in deviceKeys?.answers(story.id).orEmpty()) {
                if (a.userId != identity.userId.lowercase()) continue
                val sticker = stickers[a.stickerId] ?: continue
                out[a.stickerId] = MyAnswer(runCatching { crypto.parseAnswerJson(sticker, a.payload.toString()) }.getOrNull(), anonymous = false)
            }
            for (row in apiCall(ApiClient.json) { api.myAnswers(story.id) }) {
                if (row.stickerId in stickers && row.anonymous) out[row.stickerId] = MyAnswer(null, anonymous = true)
            }
            return out
        }
        for (row in apiCall(ApiClient.json) { api.myAnswers(story.id) }) {
            val sticker = stickers[row.stickerId] ?: continue
            if (row.anonymous) {
                out[row.stickerId] = MyAnswer(null, anonymous = true)
                continue
            }
            out[row.stickerId] = try {
                MyAnswer(crypto.openAnswer(story.id, sticker, Base64Codec.decode(row.envelope), authorPublicKey, identity.privateKey), anonymous = false)
            } catch (e: Exception) {
                MyAnswer(null, anonymous = false)
            }
        }
        return out
    }

    /** The totals the author published, for stickers that show results. Anything that doesn't verify is left out. */
    suspend fun totals(story: Story): Map<String, StickerTotals> {
        val stickers = story.answerableStickers().associateBy { it.id }
        val out = HashMap<String, StickerTotals>()
        for (row in apiCall(ApiClient.json) { api.totals(story.id) }) {
            val sticker = stickers[row.stickerId] ?: continue
            if (sticker is QuestionSticker) continue
            try {
                out[row.stickerId] = crypto.openTotals(story.id, sticker, row.revision, Base64Codec.decode(row.ciphertext), Base64Codec.decode(row.nonce), story.key)
            } catch (e: Exception) {
                // Forged, for another sticker, or malformed: not shown.
            }
        }
        return out
    }

    // --- The author's side -----------------------------------------------------------------------------

    /** Opens every answer sealed to you on one of your stories. Answers from people who aren't friends any more can't be opened (their key isn't known). */
    suspend fun inbox(identity: SelfIdentity, story: Story, friends: Map<String, StoryFriend>, deviceKeys: StoryDeviceKeys? = null): Inbox {
        if (story.deviceKeys) return inboxOnDeviceKeys(identity, story, deviceKeys)
        val stickers = story.answerableStickers().associateBy { it.id }
        val entries = ArrayList<InboxEntry>()
        var unreadable = 0
        for (row in apiCall(ApiClient.json) { api.answers(story.id) }) {
            val sticker = stickers[row.stickerId]
            if (sticker == null) {
                unreadable++
                continue
            }
            try {
                val envelope = Base64Codec.decode(row.envelope)
                val answer = if (row.anonymous || row.responderId == null) {
                    crypto.openAnonymousAnswer(story.id, sticker, envelope, identity.publicKey, identity.privateKey)
                } else {
                    val responder = friends[row.responderId] ?: throw IllegalStateException("not a friend")
                    crypto.openAnswer(story.id, sticker, envelope, responder.publicKey, identity.privateKey)
                }
                entries.add(
                    InboxEntry(sticker, if (row.anonymous) null else row.responderId, answer, Instant.parse(row.respondedAt).toEpochMilli(), row.revision, row.envelope),
                )
            } catch (e: Exception) {
                unreadable++
            }
        }
        return Inbox(entries, unreadable)
    }

    /**
     * Part D: the named answers this device was sent (the latest revision per person and sticker, kept on this phone),
     * and the anonymous ones on the server, each opened from the copy sealed to this device. An anonymous answer names
     * nobody: the server doesn't say who sent it, and nothing inside does either.
     */
    private suspend fun inboxOnDeviceKeys(identity: SelfIdentity, story: Story, deviceKeys: StoryDeviceKeys?): Inbox {
        val stickers = story.answerableStickers().associateBy { it.id }
        val entries = ArrayList<InboxEntry>()
        var unreadable = 0
        for (a in deviceKeys?.answers(story.id).orEmpty()) {
            if (a.userId == identity.userId.lowercase()) continue
            val sticker = stickers[a.stickerId]
            if (sticker == null || isAnonymousSticker(sticker)) {
                // A named answer to a question that must come without a name isn't counted.
                unreadable++
                continue
            }
            try {
                entries.add(InboxEntry(sticker, a.userId, crypto.parseAnswerJson(sticker, a.payload.toString()), a.ts / 60_000 * 60_000, a.rev.toInt(), ""))
            } catch (e: Exception) {
                unreadable++
            }
        }
        for (row in apiCall(ApiClient.json) { api.answers(story.id) }) {
            val sticker = stickers[row.stickerId]
            if (sticker == null || !row.anonymous) {
                unreadable++
                continue
            }
            try {
                val payload = deviceKeys?.openAnonymous(Base64Codec.decode(row.envelope)) ?: throw IllegalStateException("not for this device")
                entries.add(InboxEntry(sticker, null, crypto.openAnonymousPayload(story.id, sticker, payload), Instant.parse(row.respondedAt).toEpochMilli(), row.revision, row.envelope))
            } catch (e: Exception) {
                unreadable++
            }
        }
        return Inbox(entries, unreadable)
    }

    /** The totals of one sticker from the answers in [inbox]. */
    fun tally(sticker: AnswerableSticker, inbox: Inbox): StickerTotals = StoryTally.tally(sticker, inbox.entries)

    /**
     * Publishes the totals of your stickers that show results, sealed with the story's key. Only what changed is sent; a second
     * device publishing at the same moment just loses the race (409) and the next round catches up. Returns how many stickers were updated.
     */
    suspend fun publishTotals(story: Story, inbox: Inbox): Int {
        val shown = story.answerableStickers().filter { it !is QuestionSticker && resultsOf(it) == ResultsVisibility.Everyone }
        if (shown.isEmpty()) return 0
        val current = apiCall(ApiClient.json) { api.totals(story.id) }.associate { it.stickerId to it.revision }
        var updated = 0
        for (sticker in shown) {
            val totals = tally(sticker, inbox)
            val fingerprint = fingerprintOf(totals)
            val cacheKey = story.id + ":" + sticker.id
            if (published[cacheKey] == fingerprint && sticker.id in current) continue
            val revision = (current[sticker.id] ?: 0) + 1
            val sealed = crypto.sealTotals(story.id, sticker.id, revision, totals, story.key)
            try {
                apiCall(ApiClient.json) { api.putTotals(story.id, sticker.id, StoryTotalsRequest(revision, Base64Codec.encode(sealed.nonce), Base64Codec.encode(sealed.ciphertext))) }
                published[cacheKey] = fingerprint
                updated++
            } catch (e: ApiException) {
                if (e.status != 409) throw e
            }
        }
        return updated
    }

    /**
     * Reports an answer on your story. [evidence] is only what the reporter chose to include. The answer is named by its
     * stored envelope; a named answer to a device-keys story came over device keys and has none on the server, so it's
     * named by [responderId] instead (Part D).
     */
    suspend fun reportAnswer(storyId: String, stickerId: String, envelope: String, reason: String, evidence: String?, responderId: String? = null) {
        val body = answerReportBody(stickerId, envelope, reason, evidence, responderId)
        apiCall(ApiClient.json) { api.reportAnswer(storyId, body) }
    }

    fun forgetPublished() = published.clear()

    companion object {
        /** The body of `POST /stories/:id/responses/report`: the envelope when there is one, else who sent the answer. */
        fun answerReportBody(stickerId: String, envelope: String, reason: String, evidence: String?, responderId: String?): kotlinx.serialization.json.JsonObject =
            kotlinx.serialization.json.buildJsonObject {
                put("stickerId", stickerId)
                when {
                    envelope.isNotEmpty() -> put("envelope", envelope)
                    responderId != null -> put("responderId", responderId)
                    else -> throw IllegalArgumentException("An answer with no envelope and no sender can't be reported")
                }
                put("reason", reason)
                if (!evidence.isNullOrBlank()) put("evidence", evidence)
            }

        private const val NO_DEVICE_KEYS = "This story uses device keys, which aren't available on this phone right now."
    }

    private val published = HashMap<String, String>()

    private fun fingerprintOf(t: StickerTotals) = "${t.total}|${t.counts}|${t.correct}|${t.average}"

    private fun resultsOf(s: AnswerableSticker): ResultsVisibility = when (s) {
        is PollSticker -> s.results
        is QuizSticker -> s.results
        is SliderSticker -> s.results
        is QuestionSticker -> ResultsVisibility.Author
    }
}
