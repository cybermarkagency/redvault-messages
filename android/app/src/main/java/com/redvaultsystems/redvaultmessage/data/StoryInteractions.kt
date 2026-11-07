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

    suspend fun submit(identity: SelfIdentity, story: Story, authorPublicKey: ByteArray, sticker: AnswerableSticker, answer: StoryAnswer) {
        val anonymous = isAnonymousSticker(sticker)
        val envelope = if (anonymous) {
            crypto.sealAnonymousAnswer(story.id, sticker, answer, authorPublicKey)
        } else {
            crypto.sealAnswer(story.id, sticker, answer, authorPublicKey, identity.privateKey)
        }
        apiCall(ApiClient.json) { api.putAnswer(story.id, sticker.id, StoryAnswerRequest(Base64Codec.encode(envelope), anonymous, isFinalSticker(sticker))) }
    }

    /** What this account already answered on a story (so a reload or another phone shows it). */
    suspend fun mine(identity: SelfIdentity, story: Story, authorPublicKey: ByteArray): Map<String, MyAnswer> {
        val stickers = story.answerableStickers().associateBy { it.id }
        val out = HashMap<String, MyAnswer>()
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
    suspend fun inbox(identity: SelfIdentity, story: Story, friends: Map<String, StoryFriend>): Inbox {
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

    /** Reports an answer on your story. [evidence] is only what the reporter chose to include. */
    suspend fun reportAnswer(storyId: String, stickerId: String, envelope: String, reason: String, evidence: String?) {
        val body = kotlinx.serialization.json.buildJsonObject {
            put("stickerId", stickerId)
            put("envelope", envelope)
            put("reason", reason)
            if (!evidence.isNullOrBlank()) put("evidence", evidence)
        }
        apiCall(ApiClient.json) { api.reportAnswer(storyId, body) }
    }

    fun forgetPublished() = published.clear()

    private val published = HashMap<String, String>()

    private fun fingerprintOf(t: StickerTotals) = "${t.total}|${t.counts}|${t.correct}|${t.average}"

    private fun resultsOf(s: AnswerableSticker): ResultsVisibility = when (s) {
        is PollSticker -> s.results
        is QuizSticker -> s.results
        is SliderSticker -> s.results
        is QuestionSticker -> ResultsVisibility.Author
    }
}
