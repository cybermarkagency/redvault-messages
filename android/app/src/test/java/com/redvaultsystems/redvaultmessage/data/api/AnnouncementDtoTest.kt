// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.api

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The Inbox and message-request bodies in the exact shapes the server returns (routes/broadcastRoutes.ts, conversationRoutes.ts). */
class AnnouncementDtoTest {
    private val json = ApiClient.json

    @Test
    fun aPollAsTheInboxListSendsIt() {
        val items = json.decodeFromString<List<AnnouncementDto>>(
            """[{"id":"a1","kind":"poll","body":"Which colour?","allowReplies":true,"createdAt":"2026-10-20T01:02:03.000Z","read":false,
                "options":[{"id":"o1","label":"Red"},{"id":"o2","label":"Blue"}],"myOptionId":null},
               {"id":"a2","kind":"message","body":"Maintenance tonight","allowReplies":false,"createdAt":"2026-10-19T01:02:03.000Z","read":true,
                "options":[],"myOptionId":null}]""",
        )
        assertEquals(2, items.size)
        val poll = items.first()
        assertEquals("poll", poll.kind)
        assertEquals(listOf("Red", "Blue"), poll.options.map { it.label })
        assertNull(poll.myOptionId)
        assertTrue(poll.allowReplies)
        assertFalse(poll.read)
        assertTrue(items.last().read)
    }

    @Test
    fun aVoteIsRememberedAndTheDetailCarriesReplies() {
        val voted = json.decodeFromString<AnnouncementDto>(
            """{"id":"a1","kind":"poll","body":"b","allowReplies":false,"createdAt":"2026-10-20T01:02:03.000Z","read":true,"options":[{"id":"o1","label":"Red"}],"myOptionId":"o1"}""",
        )
        assertEquals("o1", voted.myOptionId)
        // The detail also repeats the item's fields and each reply carries a user id (unused here).
        val detail = json.decodeFromString<AnnouncementDetailDto>(
            """{"id":"a1","kind":"poll","body":"b","allowReplies":true,"createdAt":"2026-10-20T01:02:03.000Z","read":true,"options":[],"myOptionId":null,
                "replies":[{"id":"r1","userId":"u1","body":"Thanks","createdAt":"2026-10-20T02:00:00.000Z"}]}""",
        )
        assertEquals("Thanks", detail.replies.single().body)
    }

    @Test
    fun voteAndReplyBodiesAreWhatTheServerValidates() {
        assertEquals("""{"optionId":"o1"}""", json.encodeToString(AnnouncementVoteRequest.serializer(), AnnouncementVoteRequest("o1")))
        assertEquals("""{"body":"Hi"}""", json.encodeToString(AnnouncementReplyRequest.serializer(), AnnouncementReplyRequest("Hi")))
    }
}
