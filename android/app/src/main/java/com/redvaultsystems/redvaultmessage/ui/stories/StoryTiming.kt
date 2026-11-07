package com.redvaultsystems.redvaultmessage.ui.stories

import com.redvaultsystems.redvaultmessage.data.model.AnswerableSticker
import com.redvaultsystems.redvaultmessage.data.model.StorySticker

/** How long a story stays on screen: the clock the viewer runs, kept apart from the screen so it can be tested. */
internal object StoryTiming {
    /** Photos and text stay five seconds, as on the web; a video stays as long as it plays. */
    const val STILL_MS = 5_000L

    /** A story with something to answer stays longer, as on the web. */
    const val STICKER_MS = 20_000L

    /** How long a photo or text story stays: longer when a poll, quiz, question or slider is waiting for an answer. */
    fun stillMs(stickers: List<StorySticker>): Long = if (stickers.any { it is AnswerableSticker }) STICKER_MS else STILL_MS

    /** The progress bar after [elapsedNanos] more time on a story that stays [stillMs]. Not capped: the viewer moves on once it reaches 1. */
    fun advance(progress: Float, elapsedNanos: Long, stillMs: Long): Float = progress + elapsedNanos / 1_000_000f / stillMs

    /** A playing video's bar: where it is, as a fraction of its length (nothing until the length is known). */
    fun videoProgress(positionMs: Long, durationMs: Long): Float? = if (durationMs > 0) (positionMs.toFloat() / durationMs).coerceIn(0f, 1f) else null
}
