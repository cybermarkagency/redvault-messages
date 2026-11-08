// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.data.api.MessageDto
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

/** The desktop's fetchLatest, ported: pages with `before`, never skipping or repeating a message. */
class MessagePagingTest {
    private val start = Instant.parse("2026-10-01T00:00:00Z").toEpochMilli()

    private fun dto(i: Int, atMs: Long = start + i * 1_000L) =
        MessageDto(id = "m$i", conversationId = "c1", senderId = "u1", createdAt = Instant.ofEpochMilli(atMs).toString())

    /** Stands in for GET /conversations/:id/messages: the newest [limit] created before [before]. */
    private class FakeServer(val all: List<MessageDto>) {
        val calls = ArrayList<Pair<Int, String?>>()
        fun page(limit: Int, before: String?): List<MessageDto> {
            calls += limit to before
            val cutoff = before?.let { Instant.parse(it).toEpochMilli() }
            return all.filter { cutoff == null || Instant.parse(it.createdAt).toEpochMilli() < cutoff }
                .sortedByDescending { Instant.parse(it.createdAt).toEpochMilli() }
                .take(limit)
        }
    }

    @Test
    fun aShortChatIsOnePageWithNothingOlder() = runBlocking {
        val server = FakeServer((1..12).map { dto(it) })
        val latest = MessagePaging.fetchLatest(MessagePaging.PAGE_SIZE, server::page)
        assertEquals(12, latest.messages.size)
        assertFalse(latest.hasOlder)
        assertEquals(listOf(50 to null), server.calls)
    }

    @Test
    fun theFirstPageAsksForTheServersDefaultAndNothingBefore() = runBlocking {
        val server = FakeServer((1..80).map { dto(it) })
        val latest = MessagePaging.fetchLatest(MessagePaging.PAGE_SIZE, server::page)
        assertEquals((80 downTo 31).map { "m$it" }, latest.messages.map { it.id })
        assertTrue(latest.hasOlder)
        assertEquals(1, server.calls.size)
    }

    @Test
    fun aBiggerWindowIsOnePageWhileItFitsInOne() = runBlocking {
        val server = FakeServer((1..130).map { dto(it) })
        val latest = MessagePaging.fetchLatest(100, server::page)
        assertEquals((130 downTo 31).map { "m$it" }, latest.messages.map { it.id })
        assertTrue(latest.hasOlder)
        assertEquals(listOf(100 to null), server.calls)
        assertNull(server.calls[0].second)
    }

    @Test
    fun theWholeChatOnceTheWindowIsPastItsStart() = runBlocking {
        val server = FakeServer((1..130).map { dto(it) })
        val latest = MessagePaging.fetchLatest(150, server::page)
        assertEquals(130, latest.messages.size)
        assertEquals(130, latest.messages.map { it.id }.toSet().size)
        assertFalse(latest.hasOlder)
    }

    @Test
    fun pagesNeverAskForMoreThanTheServerAllows() = runBlocking {
        val server = FakeServer((1..450).map { dto(it) })
        val latest = MessagePaging.fetchLatest(400, server::page)
        assertEquals(400, latest.messages.size)
        assertTrue(server.calls.all { it.first in 1..MessagePaging.MAX_PAGE })
        assertEquals((450 downTo 51).map { "m$it" }, latest.messages.map { it.id })
        // Every later page starts one millisecond after the oldest message held so far.
        val firstOldest = Instant.parse(dto(251).createdAt).toEpochMilli()
        assertEquals(Instant.ofEpochMilli(firstOldest + 1).toString(), server.calls[1].second)
    }

    @Test
    fun messagesInTheSameMillisecondAtAPageEdgeAreNotSkipped() = runBlocking {
        // m96..m106 share one moment, and the first page (200, newest first: m300..m101) ends inside it.
        val same = start + 100_000L
        val all = (1..300).map { i -> if (i in 96..106) dto(i, same) else dto(i) }
        val server = FakeServer(all)
        val latest = MessagePaging.fetchLatest(210, server::page)
        assertEquals(210, latest.messages.size)
        assertEquals(210, latest.messages.map { it.id }.toSet().size)
        assertTrue((96..106).all { i -> latest.messages.any { it.id == "m$i" } })
        assertEquals((300 downTo 91).map { "m$it" }.toSet(), latest.messages.map { it.id }.toSet())
        // The second page re-asks for that moment: 10 more wanted plus the ones already held from it.
        assertEquals(Instant.ofEpochMilli(same + 1).toString(), server.calls[1].second)
        assertTrue(server.calls[1].first > 10)
    }
}
