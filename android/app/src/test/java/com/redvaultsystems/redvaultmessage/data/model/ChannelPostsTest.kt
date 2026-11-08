// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistItemDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPollDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The web's lib/channelPosts.ts, channelViews.ts and channelWatch.ts rules,
 * as the desktop app pins them (desktop/tests/channel_posts_test.cpp).
 */
class ChannelPostsTest {
    private val id = "11111111-2222-4333-8444-555555555555"

    @Test
    fun picturesArePhotosAndEverythingElseGoesByItsType() {
        assertEquals("photo", ChannelPosts.mediaKindFor("image/JPEG"))
        assertEquals("photo", ChannelPosts.mediaKindFor("image/heic"))
        // Not SVG: it can carry script.
        assertEquals("file", ChannelPosts.mediaKindFor("image/svg+xml"))
        assertEquals("video", ChannelPosts.mediaKindFor("video/mp4"))
        assertEquals("audio", ChannelPosts.mediaKindFor("audio/mpeg"))
        assertEquals("file", ChannelPosts.mediaKindFor("application/pdf"))
        assertEquals("Voice clip", ChannelPosts.kindLabel("voice"))
        assertEquals("Round video", ChannelPosts.kindLabel("round"))
        assertEquals("File", ChannelPosts.kindLabel("anything"))
    }

    @Test
    fun durationsReadLikeTheWebs() {
        assertEquals("0:00", ChannelPosts.formatDuration(0.0))
        assertEquals("0:09", ChannelPosts.formatDuration(9.99))
        assertEquals("3:05", ChannelPosts.formatDuration(185.0))
        assertEquals("1:02:03", ChannelPosts.formatDuration(3723.4))
        assertEquals("0:00", ChannelPosts.formatDuration(-1.0))
        assertEquals("0:00", ChannelPosts.formatDuration(Double.NaN))
        assertEquals("0:00", ChannelPosts.formatDuration(Double.POSITIVE_INFINITY))
    }

    @Test
    fun speedsGoRoundAndAStrangeOneStartsOver() {
        assertEquals(1.5f, ChannelPosts.nextSpeed(1f))
        assertEquals(2f, ChannelPosts.nextSpeed(1.5f))
        assertEquals(1f, ChannelPosts.nextSpeed(2f))
        assertEquals(1f, ChannelPosts.nextSpeed(3f))
        assertEquals("1×", ChannelPosts.speedLabel(1f))
        assertEquals("1.5×", ChannelPosts.speedLabel(1.5f))
    }

    @Test
    fun theToolbarWrapsTheSelectionOrPutsAPairAtTheCursor() {
        assertEquals(Triple("say *hello* there", 5, 10), ChannelPosts.wrapSelection("say hello there", 4, 9, "*"))
        assertEquals(Triple("ab||||c", 4, 4), ChannelPosts.wrapSelection("abc", 2, 2, "||"))
        // A selection past the end is clamped rather than throwing.
        assertEquals(Triple("x``", 2, 2), ChannelPosts.wrapSelection("x", 5, 9, "`"))
    }

    @Test
    fun pollSharesRoundLikeMathRound() {
        val poll = ChannelPollDto("Q", listOf("a", "b", "c"), counts = listOf(1, 1, 1), voters = 3)
        assertEquals(listOf(33, 33, 33), ChannelPosts.percents(poll))
        assertEquals(listOf(50, 50), ChannelPosts.percents(ChannelPollDto("Q", listOf("a", "b"), counts = listOf(1, 1), voters = 2)))
        // A multiple-choice poll: shares of the voters, not of the votes.
        assertEquals(listOf(100, 50), ChannelPosts.percents(ChannelPollDto("Q", listOf("a", "b"), multiple = true, counts = listOf(2, 1), voters = 2)))
        assertEquals(listOf(0, 0), ChannelPosts.percents(ChannelPollDto("Q", listOf("a", "b"))))
        // Fewer counts than options: the rest are 0.
        assertEquals(listOf(100, 0), ChannelPosts.percents(ChannelPollDto("Q", listOf("a", "b"), counts = listOf(1), voters = 1)))
    }

    @Test
    fun eachPhoneCountsAPostOnceAndRemembersOnlyTheNewest() {
        val remembered = mutableListOf("aaaa")
        assertEquals(listOf("bbbb", "cccc"), ChannelPosts.uncountedViews(remembered, listOf("AAAA", "BBBB", "bbbb", "cccc")))
        assertEquals(listOf("aaaa", "bbbb", "cccc"), remembered)
        assertEquals(emptyList<String>(), ChannelPosts.uncountedViews(remembered, listOf("cccc")))

        val full = MutableList(ChannelPosts.MAX_REMEMBERED_VIEWS) { "p$it" }
        ChannelPosts.uncountedViews(full, listOf("new"))
        assertEquals(ChannelPosts.MAX_REMEMBERED_VIEWS, full.size)
        assertEquals("p1", full.first())
        assertEquals("new", full.last())
    }

    @Test
    fun aNotificationShowsThePostsTextOrWhatItCarries() {
        fun post(body: String = "", poll: ChannelPollDto? = null, checklist: ChannelChecklistDto? = null, kind: String? = null) =
            ChannelPostDto("p", body, "2026-10-19T09:00:00.000Z", poll = poll, checklist = checklist, media = listOfNotNull(kind?.let { ChannelMediaDto("m", it, url = "/api/x") }))
        assertEquals("New post", ChannelPosts.postPreview(null))
        assertEquals("Big news today", ChannelPosts.postPreview(post("*Big*   news\n\n_today_")))
        val long = ChannelPosts.postPreview(post("x".repeat(200)))
        assertEquals(140, long.length)
        assertTrue(long.endsWith("…"))
        assertEquals("Quiz: Which?", ChannelPosts.postPreview(post(poll = ChannelPollDto("Which?", listOf("a", "b"), quiz = true))))
        assertEquals("Poll: Which?", ChannelPosts.postPreview(post(poll = ChannelPollDto("Which?", listOf("a", "b")))))
        assertEquals("Checklist: Trip", ChannelPosts.postPreview(post(checklist = ChannelChecklistDto("Trip", listOf(ChannelChecklistItemDto("Pack"))))))
        assertEquals("Photo", ChannelPosts.postPreview(post(kind = "photo")))
        assertEquals("Voice message", ChannelPosts.postPreview(post(kind = "voice")))
        assertEquals("Video message", ChannelPosts.postPreview(post(kind = "round")))
        assertEquals("New post", ChannelPosts.postPreview(post()))
    }

    @Test
    fun notifiesOncePerNewestPostButNeverForAMutedOrOpenChannel() {
        val followed = FollowedChannel(id, "News", "2026-10-19T08:00:00.000Z")
        val channel = ChannelDto(id, "News", lastPostAt = "2026-10-19T09:00:00.000Z")
        val notified = mutableMapOf<String, String>()

        // Nothing unseen: not even counted.
        assertEquals(WatchOutcome(), ChannelPosts.watch(followed.copy(seenAt = channel.lastPostAt), channel, notified, null))

        assertEquals(WatchOutcome(unread = true, notify = true), ChannelPosts.watch(followed, channel, notified, null))
        assertEquals(channel.lastPostAt, notified[id])
        // The same post again: counted, not notified twice.
        assertEquals(WatchOutcome(unread = true), ChannelPosts.watch(followed, channel, notified, null))

        val newer = channel.copy(lastPostAt = "2026-10-19T10:00:00.000Z")
        // Muted, or open right now: counted, and not remembered as notified.
        assertEquals(WatchOutcome(unread = true), ChannelPosts.watch(followed.copy(muted = true), newer, notified, null))
        assertEquals(WatchOutcome(unread = true), ChannelPosts.watch(followed, newer, notified, id))
        assertEquals(channel.lastPostAt, notified[id])
        assertEquals(WatchOutcome(unread = true, notify = true), ChannelPosts.watch(followed, newer, notified, "other"))

        // A remembered time that doesn't parse never makes a post count as newer.
        val garbled = mutableMapOf(id to "yesterday")
        assertFalse(ChannelPosts.watch(followed, newer, garbled, null).notify)
    }

    @Test
    fun viewCountsReadNaturally() {
        assertEquals("1 view", ChannelPosts.views(1))
        assertEquals("0 views", ChannelPosts.views(0))
        assertTrue(ChannelPosts.views(1234).endsWith(" views"))
    }
}
