package com.redvaultsystems.redvaultmessage.data.model

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull

// Interactive and informational stickers on a story (port of the sticker half of ui/src/lib/storyCrypto.ts and of
// ui/src/lib/storyInteractions.ts). Stickers are part of a story's sealed details, so the server never sees a question.
// The rules are the web's: a sticker of a type this app doesn't know, or one that's malformed, is dropped on its own
// and the story still shows. All text is shown as text, never as markup. Pinned to the web by vectors it made
// (resources/vectors/story_interactive_vectors.json).

/** Who sees a sticker's totals: everyone who can see the story, or only its author. */
enum class ResultsVisibility { Everyone, Author }

enum class DateStyle(val wire: String) { Date("date"), Time("time"), DateTime("datetime") }

sealed class StorySticker {
    abstract val id: String

    /** Centre, as fractions of the frame. */
    abstract val x: Double
    abstract val y: Double

    /** 0.6 to 2.5. */
    abstract val scale: Double

    /** Degrees, 0 until 360. */
    abstract val rotation: Double
}

/** The stickers somebody can answer. */
sealed class AnswerableSticker : StorySticker()

data class PollSticker(
    override val id: String, override val x: Double, override val y: Double, override val scale: Double, override val rotation: Double,
    val question: String,
    val options: List<String>,
    /** Epoch ms; voting stops then (left out: until the story ends). */
    val closesAt: Long?,
    val results: ResultsVisibility,
) : AnswerableSticker()

data class QuizSticker(
    override val id: String, override val x: Double, override val y: Double, override val scale: Double, override val rotation: Double,
    val question: String,
    val options: List<String>,
    /** Index into [options]. The viewers' apps hold it too (they hold the story key): it isn't a secret from them. */
    val correct: Int,
    val explanation: String?,
    val results: ResultsVisibility,
) : AnswerableSticker()

data class QuestionSticker(
    override val id: String, override val x: Double, override val y: Double, override val scale: Double, override val rotation: Double,
    val prompt: String,
    /** Answers arrive without the sender's name inside (the server still knows who sent them). */
    val anonymous: Boolean,
) : AnswerableSticker()

data class SliderSticker(
    override val id: String, override val x: Double, override val y: Double, override val scale: Double, override val rotation: Double,
    val prompt: String,
    val emoji: String,
    val results: ResultsVisibility,
) : AnswerableSticker()

data class CountdownSticker(
    override val id: String, override val x: Double, override val y: Double, override val scale: Double, override val rotation: Double,
    val title: String,
    val endsAt: Long,
) : StorySticker()

data class LinkSticker(
    override val id: String, override val x: Double, override val y: Double, override val scale: Double, override val rotation: Double,
    val url: String,
    val label: String,
) : StorySticker()

data class MentionSticker(
    override val id: String, override val x: Double, override val y: Double, override val scale: Double, override val rotation: Double,
    val userId: String,
) : StorySticker()

data class DateSticker(
    override val id: String, override val x: Double, override val y: Double, override val scale: Double, override val rotation: Double,
    val at: Long,
    val style: DateStyle,
) : StorySticker()

/** A place the author typed. Never read from a device. */
data class LocationSticker(
    override val id: String, override val x: Double, override val y: Double, override val scale: Double, override val rotation: Double,
    val label: String,
) : StorySticker()

object StickerLimits {
    const val MAX_STICKERS = 12
    const val MAX_STICKER_TEXT = 120
    const val MAX_STICKER_OPTION = 40
    const val MAX_QUESTION_ANSWER = 500
    const val MAX_LINK_LENGTH = 300
}

object StoryStickerParser {
    private val ID = Regex("^[a-z0-9]{6,16}$")
    private val USER_ID = Regex("^[0-9a-fA-F-]{36}$")

    /** Drops anything malformed or unknown (one bad sticker never hides a story); a repeated id keeps its first. */
    fun parse(raw: JsonElement?): List<StorySticker> {
        val items = (raw as? JsonArray)?.take(StickerLimits.MAX_STICKERS) ?: return emptyList()
        val seen = HashSet<String>()
        val out = ArrayList<StorySticker>()
        for (item in items) {
            val sticker = parseSticker(item) ?: continue
            if (seen.add(sticker.id)) out.add(sticker)
        }
        return out
    }

    private fun parseSticker(raw: JsonElement): StorySticker? {
        val r = raw as? JsonObject ?: return null
        val id = r["id"]?.string() ?: return null
        if (!ID.matches(id)) return null
        val x = r["x"]?.fractionOrNull() ?: return null
        val y = r["y"]?.fractionOrNull() ?: return null
        val scale = r["scale"]?.number()?.takeIf { it.isFinite() }?.coerceIn(0.6, 2.5) ?: 1.0
        val rotation = r["rotation"]?.number()?.takeIf { it.isFinite() }?.let { ((it % 360) + 360) % 360 } ?: 0.0
        val visibility = if (r["results"]?.string() == "author") ResultsVisibility.Author else ResultsVisibility.Everyone
        fun clean(key: String, max: Int) = cleanStickerText(r[key], max)
        return when (r["type"]?.string()) {
            "poll", "quiz" -> {
                val question = clean("question", StickerLimits.MAX_STICKER_TEXT) ?: return null
                val rawOptions = (r["options"] as? JsonArray)?.takeIf { it.size in 2..4 } ?: return null
                val options = rawOptions.map { cleanStickerText(it, StickerLimits.MAX_STICKER_OPTION) ?: return null }
                if (r["type"]?.string() == "poll") {
                    PollSticker(id, x, y, scale, rotation, question, options, whenOrNull(r["closesAt"]), visibility)
                } else {
                    val correct = r["correct"]?.number()?.takeIf { it == Math.floor(it) && it >= 0 && it < options.size }?.toInt() ?: return null
                    QuizSticker(id, x, y, scale, rotation, question, options, correct, clean("explanation", StickerLimits.MAX_STICKER_TEXT), visibility)
                }
            }
            "question" -> QuestionSticker(id, x, y, scale, rotation, clean("prompt", StickerLimits.MAX_STICKER_TEXT) ?: return null, r["anonymous"].isTrue())
            "slider" -> SliderSticker(id, x, y, scale, rotation, clean("prompt", StickerLimits.MAX_STICKER_TEXT) ?: return null, clean("emoji", 8) ?: return null, visibility)
            "countdown" -> CountdownSticker(id, x, y, scale, rotation, clean("title", 60) ?: return null, whenOrNull(r["endsAt"]) ?: return null)
            "link" -> {
                val url = safeStoryLink(r["url"]?.string()) ?: return null
                LinkSticker(id, x, y, scale, rotation, url, clean("label", 40) ?: return null)
            }
            "mention" -> {
                val userId = r["userId"]?.string()?.takeIf { USER_ID.matches(it) && it.length == 36 } ?: return null
                MentionSticker(id, x, y, scale, rotation, userId.lowercase())
            }
            "date" -> {
                val at = whenOrNull(r["at"]) ?: return null
                val style = DateStyle.entries.firstOrNull { it.wire == r["style"]?.string() } ?: return null
                DateSticker(id, x, y, scale, rotation, at, style)
            }
            "location" -> LocationSticker(id, x, y, scale, rotation, clean("label", 60) ?: return null)
            // A sticker type from a newer app: left out, the rest of the story still shows.
            else -> null
        }
    }

    private fun JsonElement.fractionOrNull(): Double? = number()?.takeIf { it.isFinite() && it >= 0.0 && it <= 1.0 }

    private fun JsonElement?.isTrue() = this is JsonPrimitive && !isString && content == "true"

    /** A time that makes sense: a positive number below JavaScript's largest date. */
    private fun whenOrNull(v: JsonElement?): Long? = v?.number()?.takeIf { it.isFinite() && it > 0 && it < 8.64e15 }?.toLong()

    /** The sticker as the JSON the web's parser would hold (for tests). */
    fun toJson(s: StorySticker): JsonObject = buildJsonObject {
        put("id", s.id)
        put(
            "type",
            when (s) {
                is PollSticker -> "poll"
                is QuizSticker -> "quiz"
                is QuestionSticker -> "question"
                is SliderSticker -> "slider"
                is CountdownSticker -> "countdown"
                is LinkSticker -> "link"
                is MentionSticker -> "mention"
                is DateSticker -> "date"
                is LocationSticker -> "location"
            },
        )
        put("x", s.x)
        put("y", s.y)
        put("scale", s.scale)
        put("rotation", s.rotation)
        when (s) {
            is PollSticker -> {
                put("question", s.question)
                put("options", buildJsonArray { s.options.forEach { add(JsonPrimitive(it)) } })
                put("results", if (s.results == ResultsVisibility.Author) "author" else "everyone")
                s.closesAt?.let { put("closesAt", it) }
            }
            is QuizSticker -> {
                put("question", s.question)
                put("options", buildJsonArray { s.options.forEach { add(JsonPrimitive(it)) } })
                put("correct", s.correct)
                put("results", if (s.results == ResultsVisibility.Author) "author" else "everyone")
                s.explanation?.let { put("explanation", it) }
            }
            is QuestionSticker -> {
                put("prompt", s.prompt)
                put("anonymous", s.anonymous)
            }
            is SliderSticker -> {
                put("prompt", s.prompt)
                put("emoji", s.emoji)
                put("results", if (s.results == ResultsVisibility.Author) "author" else "everyone")
            }
            is CountdownSticker -> {
                put("title", s.title)
                put("endsAt", s.endsAt)
            }
            is LinkSticker -> {
                put("url", s.url)
                put("label", s.label)
            }
            is MentionSticker -> put("userId", s.userId)
            is DateSticker -> {
                put("at", s.at)
                put("style", s.style.wire)
            }
            is LocationSticker -> put("label", s.label)
        }
    }
}

/**
 * A link a story may carry: https only, no credentials, a real host (one with a dot), at most 300 characters. The URL
 * comes back in the form the parser writes it (host in lower case, default port dropped), which is what the confirm
 * screen shows; a link the web would rewrite but this parser can't reproduce exactly is refused instead (never looser).
 */
fun safeStoryLink(raw: String?): String? {
    if (raw == null || raw.length > StickerLimits.MAX_LINK_LENGTH) return null
    val url = jsTrim(raw).toHttpUrlOrNull() ?: return null
    if (url.scheme != "https" || url.username.isNotEmpty() || url.password.isNotEmpty() || url.host.isEmpty() || !url.host.contains('.')) return null
    return url.toString()
}

// --- Answers and totals ---------------------------------------------------------------------------------------------

sealed class StoryAnswer {
    class Poll(val choice: Int) : StoryAnswer()
    class Quiz(val choice: Int) : StoryAnswer()
    class Slider(val value: Int) : StoryAnswer()
    class Question(val text: String) : StoryAnswer()
}

class AnswerException(message: String = "malformed answer") : Exception(message)

object StoryAnswers {
    /** The JSON that gets sealed. */
    fun toJson(a: StoryAnswer): String = buildJsonObject {
        when (a) {
            is StoryAnswer.Poll -> { put("k", "poll"); put("choice", a.choice) }
            is StoryAnswer.Quiz -> { put("k", "quiz"); put("choice", a.choice) }
            is StoryAnswer.Slider -> { put("k", "slider"); put("value", a.value) }
            is StoryAnswer.Question -> { put("k", "question"); put("text", a.text) }
        }
    }.toString()

    /** Checks an answer against the sticker it is for; anything that doesn't fit is refused, never half counted. */
    fun parse(raw: JsonElement, sticker: AnswerableSticker): StoryAnswer {
        val r = raw as? JsonObject ?: throw AnswerException()
        val kind = r["k"]?.string()
        return when (sticker) {
            is PollSticker, is QuizSticker -> {
                val expected = if (sticker is PollSticker) "poll" else "quiz"
                val options = if (sticker is PollSticker) sticker.options.size else (sticker as QuizSticker).options.size
                if (kind != expected) throw AnswerException()
                val choice = r["choice"]?.number()?.takeIf { it == Math.floor(it) && it >= 0 && it < options }?.toInt() ?: throw AnswerException()
                if (sticker is PollSticker) StoryAnswer.Poll(choice) else StoryAnswer.Quiz(choice)
            }
            is SliderSticker -> {
                if (kind != "slider") throw AnswerException()
                StoryAnswer.Slider(r["value"]?.number()?.takeIf { it == Math.floor(it) && it >= 0 && it <= 100 }?.toInt() ?: throw AnswerException())
            }
            is QuestionSticker -> {
                if (kind != "question") throw AnswerException()
                val text = r["text"]?.string()?.let { jsTrim(it) }.orEmpty()
                if (text.isEmpty() || text.length > StickerLimits.MAX_QUESTION_ANSWER) throw AnswerException()
                StoryAnswer.Question(text)
            }
        }
    }
}

/** Aggregates only, never who answered what. */
class StickerTotals(
    /** How many answers were counted. */
    val total: Int,
    /** Poll and quiz: answers per option. */
    val counts: List<Int>? = null,
    /** Quiz: how many chose the right option. */
    val correct: Int? = null,
    /** Slider: the mean position, 0 to 100. */
    val average: Double? = null,
)

object StickerTotalsParser {
    private const val MAX_COUNT = 1_000_000.0

    private fun JsonElement.countOrNull(): Int? = number()?.takeIf { it == Math.floor(it) && it >= 0 && it <= MAX_COUNT }?.toInt()

    /** Totals as the author sealed them; refused unless they fit [sticker]. */
    fun parse(raw: JsonElement, sticker: AnswerableSticker): StickerTotals {
        val r = raw as? JsonObject ?: throw AnswerException("malformed results")
        val total = r["total"]?.countOrNull() ?: throw AnswerException("malformed results")
        return when (sticker) {
            is PollSticker, is QuizSticker -> {
                val optionCount = if (sticker is PollSticker) sticker.options.size else (sticker as QuizSticker).options.size
                val counts = (r["counts"] as? JsonArray)?.takeIf { it.size == optionCount }?.map { it.countOrNull() ?: throw AnswerException("malformed results") }
                    ?: throw AnswerException("malformed results")
                val correct = if (sticker is QuizSticker) r["correct"]?.countOrNull() ?: throw AnswerException("malformed results") else null
                StickerTotals(total, counts, correct)
            }
            is SliderSticker -> {
                val average = r["average"]?.number()?.takeIf { it.isFinite() && it >= 0 && it <= 100 } ?: throw AnswerException("malformed results")
                StickerTotals(total, average = average)
            }
            is QuestionSticker -> StickerTotals(total)
        }
    }

    fun toJson(t: StickerTotals): String = buildJsonObject {
        put("total", t.total)
        t.counts?.let { put("counts", buildJsonArray { it.forEach { c -> add(JsonPrimitive(c)) } }) }
        t.correct?.let { put("correct", it) }
        t.average?.let { put("average", it) }
    }.toString()
}

/** One answer the author has opened, ready to be counted. */
class InboxEntry(
    val sticker: AnswerableSticker,
    /** Null for an answer sent without a name. */
    val responderId: String?,
    val answer: StoryAnswer,
    /** Epoch ms, to the minute. */
    val at: Long,
    val revision: Int,
    /** The sealed envelope as stored (base64). */
    val envelope: String,
)

object StoryTally {
    /**
     * The totals for one sticker. A poll ignores answers sent after its deadline (to the minute: the server keeps times only that
     * precisely); a quiz counts only a first answer.
     */
    fun tally(sticker: AnswerableSticker, entries: List<InboxEntry>): StickerTotals {
        val mine = entries.filter { it.sticker.id == sticker.id }
        return when (sticker) {
            is PollSticker -> {
                val counts = IntArray(sticker.options.size)
                var total = 0
                for (e in mine) {
                    val a = e.answer as? StoryAnswer.Poll ?: continue
                    if (sticker.closesAt != null && e.at > sticker.closesAt) continue
                    counts[a.choice]++
                    total++
                }
                StickerTotals(total, counts.toList())
            }
            is QuizSticker -> {
                val counts = IntArray(sticker.options.size)
                var total = 0
                var correct = 0
                for (e in mine) {
                    val a = e.answer as? StoryAnswer.Quiz ?: continue
                    if (e.revision > 0) continue
                    counts[a.choice]++
                    total++
                    if (a.choice == sticker.correct) correct++
                }
                StickerTotals(total, counts.toList(), correct)
            }
            is SliderSticker -> {
                val values = mine.mapNotNull { (it.answer as? StoryAnswer.Slider)?.value }
                StickerTotals(values.size, average = if (values.isEmpty()) 0.0 else jsRound(values.sum().toDouble() / values.size * 10) / 10.0)
            }
            is QuestionSticker -> StickerTotals(mine.count { it.answer is StoryAnswer.Question })
        }
    }
}
