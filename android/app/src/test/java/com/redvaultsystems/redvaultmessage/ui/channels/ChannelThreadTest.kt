// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.channels

import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.time.LocalDate
import java.time.ZoneId
import java.time.ZoneOffset

/** The channel screen's decisions, and the rule that nothing on it suggests channels are encrypted. */
class ChannelThreadTest {
    private fun post(id: String, at: String, body: String = "text", views: Int = 0) = ChannelPostDto(id, body, at, views = views)

    private val utc: (ChannelPostDto) -> LocalDate? = { ChannelThread.dayOf(it.createdAt, ZoneOffset.UTC) }

    private fun rows(vararg posts: ChannelPostDto) = ChannelThread.rows(posts.toList(), utc) { "label $it" }

    @Test
    fun theSubtitleSaysWhatKindOfChannelAndThatItIsNotEncrypted() {
        assertEquals("Public channel · not end-to-end encrypted", ChannelThread.subtitle(isPrivate = false))
        assertEquals("Private channel · not end-to-end encrypted", ChannelThread.subtitle(isPrivate = true))
    }

    @Test
    fun onlyTheOwnerGetsAComposer() {
        assertEquals(ChannelBottomBar.Composer, ChannelThread.bottomBar(isOwner = true))
        assertEquals(ChannelBottomBar.ReadOnly, ChannelThread.bottomBar(isOwner = false))
    }

    @Test
    fun readersSuggestOnlyWhereTheChannelTakesSuggestions() {
        assertTrue(ChannelThread.canSuggest(isOwner = false, acceptSuggestions = true))
        assertFalse(ChannelThread.canSuggest(isOwner = false, acceptSuggestions = false))
        assertFalse(ChannelThread.canSuggest(isOwner = true, acceptSuggestions = true))
    }

    @Test
    fun aDayRowFollowsEachDaysOldestPostSoItIsDrawnAboveIt() {
        val rows = rows(
            post("c", "2026-10-21T09:00:00.000Z"),
            post("b", "2026-10-20T18:00:00.000Z"),
            post("a", "2026-10-20T08:00:00.000Z"),
        )
        assertEquals(listOf("c", "day:c", "b", "a", "day:a"), rows.map { it.key })
        assertEquals("label 2026-10-20", (rows[4] as ChannelRow.Day).label)
    }

    @Test
    fun anEmptyChannelHasNoRows() {
        assertTrue(rows().isEmpty())
    }

    @Test
    fun daysAreThePhonesDays() {
        val tokyo = ZoneId.of("Asia/Tokyo")
        assertEquals(LocalDate.of(2026, 10, 21), ChannelThread.dayOf("2026-10-20T20:00:00.000Z", tokyo))
        assertEquals(LocalDate.of(2026, 10, 20), ChannelThread.dayOf("2026-10-20T20:00:00.000Z", ZoneOffset.UTC))
    }

    @Test
    fun keysStayUniqueEvenWhenDaysComeBackOutOfOrder() {
        // A scheduled post published early can land among the newest with an unusual date; a repeated key would crash the list.
        val rows = rows(
            post("c", "2026-10-22T09:00:00.000Z"),
            post("b", "2026-10-20T09:00:00.000Z"),
            post("a", "2026-10-22T08:00:00.000Z"),
        )
        assertEquals(rows.size, rows.map { it.key }.toSet().size)
    }

    @Test
    fun aPostWithAnUnreadableDateIsStillShown() {
        val rows = rows(post("b", "not a date"), post("a", "2026-10-20T08:00:00.000Z"))
        assertEquals(listOf("b", "a", "day:a"), rows.map { it.key })
        assertNull(ChannelThread.epochMs("not a date"))
        assertTrue(ChannelThread.footer(post("b", "not a date", views = 2)) { "?" }.startsWith("not a date"))
    }

    @Test
    fun olderPagesGoOnTheEndWithoutRepeats() {
        val loaded = listOf(post("c", "2026-10-22T09:00:00.000Z"), post("b", "2026-10-21T09:00:00.000Z"))
        val page = listOf(post("b", "2026-10-21T09:00:00.000Z"), post("a", "2026-10-20T09:00:00.000Z"))
        assertEquals(listOf("c", "b", "a"), ChannelThread.appendOlder(loaded, page).map { it.id })
    }

    @Test
    fun theFooterIsTheTimeAndTheViews() {
        assertEquals("09:00 · 1,204 views", ChannelThread.footer(post("a", "2026-10-20T09:00:00.000Z", views = 1204)) { "09:00" })
        assertEquals("09:00 · 1 view", ChannelThread.footer(post("a", "2026-10-20T09:00:00.000Z", views = 1)) { "09:00" })
    }

    @Test
    fun whatCanBeDoneToAPost() {
        val text = post("a", "2026-10-20T09:00:00.000Z")
        val photoOnly = post("b", "2026-10-20T09:00:00.000Z", body = "  ")
        assertEquals(listOf(PostAction.CopyText, PostAction.Forward), ChannelThread.postActions(text, isOwner = false, isAdmin = false))
        assertEquals(listOf(PostAction.CopyText, PostAction.Forward, PostAction.Delete), ChannelThread.postActions(text, isOwner = true, isAdmin = false))
        assertEquals(listOf(PostAction.CopyText, PostAction.Forward, PostAction.Delete), ChannelThread.postActions(text, isOwner = false, isAdmin = true))
        // Forwarding needs text; a reader has nothing to do to a photo-only post, so it shows no overflow.
        assertTrue(ChannelThread.postActions(photoOnly, isOwner = false, isAdmin = false).isEmpty())
        assertEquals(listOf(PostAction.Delete), ChannelThread.postActions(photoOnly, isOwner = true, isAdmin = false))
    }

    @Test
    fun theOwnerStripShowsOnlyWhatIsThere() {
        assertTrue(ChannelThread.ownerStrip(0, acceptSuggestions = true, waitingSuggestions = 0).isEmpty())
        assertTrue(ChannelThread.ownerStrip(0, acceptSuggestions = false, waitingSuggestions = null).isEmpty())
        assertEquals(
            listOf(OwnerStripItem(OwnerPanel.Scheduled, "Scheduled (2)"), OwnerStripItem(OwnerPanel.Suggestions, "Suggested posts (3)")),
            ChannelThread.ownerStrip(2, acceptSuggestions = true, waitingSuggestions = 3),
        )
        // The count couldn't be read: the list can still be opened.
        assertEquals(listOf(OwnerStripItem(OwnerPanel.Suggestions, "Suggested posts")), ChannelThread.ownerStrip(0, acceptSuggestions = true, waitingSuggestions = null))
    }

    @Test
    fun whenTheComposerCanPost() {
        fun can(busy: Boolean = false, uploading: Boolean = false, recording: Boolean = false, text: Boolean = false, extra: Boolean = false, media: Boolean = false, ready: Int = 0) =
            ChannelThread.canPost(busy, uploading, recording, text, extra, media, ready)
        assertFalse(can())
        assertTrue(can(text = true))
        assertTrue(can(extra = true))
        assertTrue(can(media = true, ready = 1))
        assertFalse(can(text = true, busy = true))
        assertFalse(can(text = true, recording = true))
        assertFalse(can(text = true, uploading = true))
        // Every upload failed and there's no text: nothing would be posted.
        assertFalse(can(media = true, ready = 0))
        assertTrue(can(media = true, ready = 0, text = true))
    }

    @Test
    fun theMicShowsOnlyOnAnEmptyComposer() {
        assertTrue(ChannelThread.showMic(hasText = false, hasExtra = false, hasMedia = false))
        assertFalse(ChannelThread.showMic(hasText = true, hasExtra = false, hasMedia = false))
        assertFalse(ChannelThread.showMic(hasText = false, hasExtra = true, hasMedia = false))
        assertFalse(ChannelThread.showMic(hasText = false, hasExtra = false, hasMedia = true))
    }

    /** Checked in the source, as the screens only run on a phone (see privacy/SecretHandlingSourceTest). */
    @Test
    fun theChannelScreensNeverSuggestEncryption() {
        val folder = File("src/main/java/com/redvaultsystems/redvaultmessage/ui/channels")
        val screens = listOf("ChannelScreen.kt", "ChannelComposer.kt", "ChannelPostViews.kt", "ChannelSuggestions.kt", "Uploads.kt").map { File(folder, it) }
        for (screen in screens) {
            val source = screen.readText()
            for (banned in listOf("ui_lock", "ic_lock", "ui_shield", "SecurityPanel", "SecurityLine", "compactStatus", "MessageBubble", "✓ Ready")) {
                assertFalse("${screen.name} uses $banned", banned in source)
            }
        }
        // The only things allowed to say "encrypted": the negative disclosure, and the forward dialog about the copy in the chat.
        val stray = screens.flatMap { it.readLines() }.filter {
            "encrypted" in it.lowercase() && "not end-to-end encrypted" !in it && "The copy goes into that chat end-to-end encrypted" !in it
        }
        assertEquals(emptyList<String>(), stray)
    }
}
