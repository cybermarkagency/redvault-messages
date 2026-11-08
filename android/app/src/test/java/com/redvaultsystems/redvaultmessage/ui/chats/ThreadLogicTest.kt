// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.data.model.NoticeKind
import com.redvaultsystems.redvaultmessage.data.model.NoticeView
import com.redvaultsystems.redvaultmessage.data.model.PollView
import com.redvaultsystems.redvaultmessage.ui.chats.ChatTestData.file
import com.redvaultsystems.redvaultmessage.ui.chats.ChatTestData.text
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.LocalDate

class ThreadLogicTest {
    private val day = 86_400_000L
    private fun dayOf(ms: Long): LocalDate = LocalDate.ofEpochDay(ms / day)

    @Test
    fun aDayHeaderSitsAboveEachDaysFirstMessage() {
        // Newest first, as the thread's list (drawn bottom-up) holds them.
        val newestFirst = listOf(text("c2", 2 * day + 50), text("c1", 2 * day + 10), text("b1", day + 5), text("a2", 30), text("a1", 10))
        val rows = ThreadRows.build(newestFirst, ::dayOf) { "D${it.toEpochDay()}" }
        val shape = rows.map { if (it is ThreadRow.Day) it.label else (it as ThreadRow.Item).message.id }
        // Index 0 is the bottom: each header comes after (so above) its day's oldest message.
        assertEquals(listOf("c2", "c1", "D2", "b1", "D1", "a2", "a1", "D0"), shape)
        assertEquals(rows.size, rows.map { it.key }.toSet().size)
    }

    @Test
    fun noMessagesNoHeaders() {
        assertTrue(ThreadRows.build(emptyList(), ::dayOf) { "x" }.isEmpty())
    }

    @Test
    fun aMessageIsFoundAmongTheHeadersInAnyCase() {
        val rows = ThreadRows.build(listOf(text("B-2", day + 1), text("a-1", 1)), ::dayOf) { "d" }
        assertEquals(2, ThreadRows.indexOf(rows, "A-1"))
        assertEquals(0, ThreadRows.indexOf(rows, "b-2"))
        assertEquals(-1, ThreadRows.indexOf(rows, "gone"))
    }

    @Test
    fun searchMatchesLoadedTextInAnyCaseOldestFirst() {
        val messages = listOf(
            text("1", 1, text = "Lunch tomorrow?"),
            text("2", 2, text = "no"),
            text("3", 3, text = "LUNCH is at noon"),
            text("4", 4, text = "lunch").copy(kind = MessageKind.Undecryptable),
        )
        assertEquals(listOf("1", "3"), ThreadSearch.hits(messages, "  lunch "))
        assertEquals(emptyList<String>(), ThreadSearch.hits(messages, "   "))
        assertEquals(emptyList<String>(), ThreadSearch.hits(messages, "dinner"))
    }

    @Test
    fun searchStepsWrapAround() {
        assertEquals(1, ThreadSearch.step(2, 3, -1))
        assertEquals(2, ThreadSearch.step(0, 3, -1))
        assertEquals(0, ThreadSearch.step(2, 3, 1))
        assertEquals(-1, ThreadSearch.step(0, 0, 1))
    }

    @Test
    fun searchStaysOnItsMatchWhenOlderMessagesArrive() {
        assertEquals(2, ThreadSearch.keep(listOf("old", "a", "b"), "b"))
        // The match is gone (deleted): the newest instead.
        assertEquals(1, ThreadSearch.keep(listOf("a", "c"), "b"))
        assertEquals(-1, ThreadSearch.keep(emptyList(), "b"))
    }

    @Test
    fun onlyBubblesOnTheServerCanBeTicked() {
        assertTrue(MessageSelection.selectable(text("1", 1)))
        assertFalse(MessageSelection.selectable(text("2", 2).copy(queued = true)))
        assertFalse(MessageSelection.selectable(text("3", 3).copy(notice = NoticeView(NoticeKind.Pin, "1"))))
    }

    @Test
    fun forwardingTakesTextAndFilesOldestFirstButNotPolls() {
        val messages = listOf(
            text("late", 30),
            text("early", 10),
            text("poll", 20).copy(poll = PollView("Q?", listOf("a", "b"), multiple = false)),
            file("pic", "image/jpeg", atMs = 25),
            text("undecryptable", 5).copy(kind = MessageKind.Undecryptable),
        )
        val ids = messages.map { it.id }.toSet()
        assertEquals(listOf("early", "pic", "late"), MessageSelection.forwardable(messages, ids).map { it.id })
        assertEquals(listOf("late"), MessageSelection.forwardable(messages, setOf("late", "missing")).map { it.id })
    }

    @Test
    fun deleteForEveryoneOnlyWhenEveryTickedMessageIsYours() {
        val messages = listOf(text("mine", 1, fromMe = true), text("mine2", 2, fromMe = true), text("theirs", 3))
        assertTrue(MessageSelection.allFromMe(messages, setOf("mine", "mine2")))
        assertFalse(MessageSelection.allFromMe(messages, setOf("mine", "theirs")))
        assertFalse(MessageSelection.allFromMe(messages, emptySet()))
    }

    @Test
    fun deletingForEveryoneTakesEditsAndYourPinNoticesAlong() {
        val mine = text("M1", 1, fromMe = true).copy(editMessageIds = listOf("e1", "e2"))
        val myPin = text("pin", 2, fromMe = true).copy(notice = NoticeView(NoticeKind.Pin, "m1"))
        val theirPin = text("theirpin", 3).copy(notice = NoticeView(NoticeKind.Pin, "m1"))
        val theirs = text("t1", 4)
        val messages = listOf(mine, myPin, theirPin, theirs)
        assertEquals(listOf("M1", "e1", "e2", "pin"), MessageSelection.deletions(messages, setOf("M1", "t1")))
        // A ticked pin notice that also goes with a ticked message is deleted once.
        assertEquals(listOf("M1", "e1", "e2", "pin"), MessageSelection.deletions(messages, setOf("M1", "pin")))
    }
}
