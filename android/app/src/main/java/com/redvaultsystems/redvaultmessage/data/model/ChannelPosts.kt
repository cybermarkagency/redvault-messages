// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPollDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import kotlinx.serialization.Serializable
import kotlin.math.floor

/** What this phone remembers of your vote: your choice, and for a quiz the right answer you were shown. */
@Serializable
data class RememberedVote(val mine: List<Int>, val correct: Int? = null, val explanation: String? = null)

/** What a followed channel's check came to (see [ChannelPosts.watch]). */
data class WatchOutcome(
    /** It has posts you haven't seen: it counts on the Channels badge. */
    val unread: Boolean = false,
    /** Notify of its newest post now (the notified map now remembers it). */
    val notify: Boolean = false,
)

/**
 * Channel posts on this phone, as the web does it (ui/src/lib/channelPosts.ts,
 * channelViews.ts, channelWatch.ts) and the desktop app too
 * (desktop/core/src/model/channel_posts.cpp): what kind of media a file is,
 * how long things are, the formatting toolbar, view counting, and the
 * notification logic for followed channels. The server keeps no follower
 * list, so the app checks each followed channel itself (public reads, no
 * cookie).
 */
object ChannelPosts {
    const val MAX_POST = 4000
    const val MAX_POLL_OPTIONS = 10
    const val MAX_POLL_QUESTION = 300
    const val MAX_POLL_OPTION = 100
    const val MAX_EXPLANATION = 200
    const val MAX_CHECKLIST_ITEMS = 30
    const val MAX_CHECKLIST_TITLE = 200
    const val MAX_CHECKLIST_ITEM = 200
    const val MAX_MEDIA_PER_POST = 10
    const val MAX_MEDIA_PER_SUGGESTION = 4
    /** Remember at most this many counted posts; the oldest are forgotten first. */
    const val MAX_REMEMBERED_VIEWS = 5000
    /** Post ids in one view report. */
    const val VIEWS_BATCH = 100
    /** How often the app checks followed channels while it's running (and whenever it comes back on screen). */
    const val CHECK_EVERY_MS = 2 * 60 * 1000L
    private const val MAX_PREVIEW = 140

    /** Pictures that go into a post as photos (the server's PHOTO_MIMES; not SVG, which can carry script). */
    val PHOTO_MIMES = setOf("image/jpeg", "image/png", "image/gif", "image/webp", "image/avif", "image/heic", "image/heif", "image/bmp")

    /** The playback speeds offered, in order. */
    val SPEEDS = listOf(1f, 1.5f, 2f)

    fun isPhotoMime(mime: String) = mime.lowercase() in PHOTO_MIMES

    /** How a picked file goes into a post: pictures as photos (at full resolution), videos, audio (podcasts), anything else as a file. */
    fun mediaKindFor(mime: String): String {
        val lower = mime.lowercase()
        return when {
            isPhotoMime(lower) -> "photo"
            lower.startsWith("video/") -> "video"
            lower.startsWith("audio/") -> "audio"
            else -> "file"
        }
    }

    fun kindLabel(kind: String): String = when (kind) {
        "photo" -> "Photo"
        "video" -> "Video"
        "audio" -> "Audio"
        "voice" -> "Voice clip"
        "round" -> "Round video"
        else -> "File"
    }

    /** "3:05", or "1:02:03" for an hour or more. */
    fun formatDuration(seconds: Double): String {
        if (!seconds.isFinite() || seconds < 0) return "0:00"
        val s = floor(seconds).toLong()
        val h = s / 3600
        val m = (s % 3600) / 60
        val rest = s % 60
        return if (h > 0) "%d:%02d:%02d".format(h, m, rest) else "%d:%02d".format(m, rest)
    }

    fun nextSpeed(current: Float): Float {
        val i = SPEEDS.indexOf(current)
        // indexOf -1 on the web too: back to the first.
        return SPEEDS[(i + 1) % SPEEDS.size]
    }

    fun speedLabel(speed: Float): String = if (speed == floor(speed)) "${speed.toInt()}×" else "$speed×"

    /** Wraps the selection [start]..[end] in [marker] (or puts an empty pair at the cursor); returns the text and the new selection. */
    fun wrapSelection(text: String, start: Int, end: Int, marker: String): Triple<String, Int, Int> {
        val from = start.coerceIn(0, text.length)
        val to = end.coerceIn(from, text.length)
        val next = text.substring(0, from) + marker + text.substring(from, to) + marker + text.substring(to)
        return Triple(next, from + marker.length, to + marker.length)
    }

    /** Each option's share of the voters in whole percent (Math.round), all 0 before anyone voted. */
    fun percents(poll: ChannelPollDto): List<Int> = poll.options.indices.map { i ->
        val count = poll.counts.getOrElse(i) { 0 }
        if (poll.voters > 0) floor(count.toDouble() / poll.voters * 100 + 0.5).toInt() else 0
    }

    /**
     * Of [postIds], those not in [remembered] yet (lowercased, each once, in
     * order), which are added to it; it keeps the newest [MAX_REMEMBERED_VIEWS].
     */
    fun uncountedViews(remembered: MutableList<String>, postIds: List<String>): List<String> {
        val known = remembered.toHashSet()
        val fresh = ArrayList<String>()
        for (id in postIds) {
            val lower = id.lowercase()
            if (lower in known) continue
            known += lower
            fresh += lower
        }
        remembered += fresh
        if (remembered.size > MAX_REMEMBERED_VIEWS) remembered.subList(0, remembered.size - MAX_REMEMBERED_VIEWS).clear()
        return fresh
    }

    /** A notification's text for a post: its text without the markers (140 characters at most), or what it carries. */
    fun postPreview(post: ChannelPostDto?): String {
        if (post == null) return "New post"
        val text = TextFormat.toPlain(post.body).replace(Regex("\\s+"), " ").trim()
        if (text.isNotEmpty()) return if (text.length > MAX_PREVIEW) text.substring(0, MAX_PREVIEW - 1) + "…" else text
        post.poll?.let { return "${if (it.quiz) "Quiz" else "Poll"}: ${it.question}" }
        post.checklist?.let { return "Checklist: ${it.title}" }
        return when (post.media.firstOrNull()?.kind) {
            "photo" -> "Photo"
            "video" -> "Video"
            "voice" -> "Voice message"
            "round" -> "Video message"
            "audio" -> "Audio"
            "file" -> "File"
            else -> "New post"
        }
    }

    /**
     * One followed channel, as just fetched. Notifies once per newest post:
     * never for a muted channel or the one open right now ([currentChannelId]),
     * and those aren't remembered as notified, as on the web.
     */
    fun watch(followed: FollowedChannel, channel: ChannelDto, notified: MutableMap<String, String>, currentChannelId: String?): WatchOutcome {
        if (!Channels.hasUnread(followed, channel)) return WatchOutcome()
        val last = notified[followed.id]
        val newest = channel.lastPostAt
        // Like Date.parse: a time that doesn't parse never counts as newer.
        val isNew = newest != null && (last == null || run {
            val a = Channels.epochMs(newest)
            val b = Channels.epochMs(last)
            a != null && b != null && a > b
        })
        if (!isNew || followed.muted || currentChannelId == followed.id) return WatchOutcome(unread = true)
        notified[followed.id] = channel.lastPostAt!!
        return WatchOutcome(unread = true, notify = true)
    }

    /** "1 view", "1,234 views". */
    fun views(n: Int): String = "%,d %s".format(n, if (n == 1) "view" else "views")
}
