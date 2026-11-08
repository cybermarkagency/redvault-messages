// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RichMessageTest {
    private val msg = "3f2504e0-4f89-11d3-9a0c-0305e82c3301"
    private val user = "9b2e6a3c-1d4f-4e7a-8b5c-2a1f0e9d8c7b"

    @Test
    fun matchesTheWebWireFormatExactly() {
        // The same expected strings as ui/src/test/richMessage.test.ts.
        assertEquals(
            "\u0000rv-reaction:{\"type\":\"reaction\",\"version\":1,\"messageId\":\"$msg\",\"emoji\":\"👍\"}",
            RichMessage.serializeReaction(msg, "👍"),
        )
        assertEquals(
            "\u0000rv-msg:{\"type\":\"text\",\"version\":1,\"text\":\"hi\",\"replyTo\":{\"messageId\":\"$msg\",\"senderId\":\"$user\",\"preview\":\"yo\"}}",
            RichMessage.serializeReply("hi", ReplyRef(msg, user, "yo")),
        )
    }

    @Test
    fun roundTripsRepliesAndReactions() {
        val reply = RichMessage.serializeReply("sounds good", ReplyRef(msg, user, "lunch at 1?"))
        assertTrue(RichMessage.isStructured(reply))
        assertEquals(ParsedBody.Text("sounds good", ReplyRef(msg, user, "lunch at 1?")), RichMessage.parse(reply))
        assertEquals(ParsedBody.Reaction(msg, "❤️"), RichMessage.parse(RichMessage.serializeReaction(msg, "❤️")))
    }

    @Test
    fun rejectsAnythingButOneEmojiAndMalformedBodies() {
        // Any single emoji is a reaction (RichFeaturesTest has the full rule).
        assertEquals(ParsedBody.Reaction(msg, "💩"), RichMessage.parse(RichMessage.serializeReaction(msg, "💩")))
        assertEquals(ParsedBody.Invalid, RichMessage.parse(RichMessage.serializeReaction(msg, "👍").replace("👍", "👍👍")))
        assertEquals(ParsedBody.Invalid, RichMessage.parse("\u0000rv-msg:not json"))
        assertEquals(ParsedBody.Invalid, RichMessage.parse("\u0000rv-msg:{\"type\":\"text\",\"version\":2,\"text\":\"x\"}"))
        assertEquals(ParsedBody.Invalid, RichMessage.parse("\u0000rv-msg:{\"type\":\"text\",\"version\":1,\"text\":\"x\",\"replyTo\":{\"messageId\":\"../x\"}}"))
        assertFalse(RichMessage.isStructured("plain text"))
    }

    @Test
    fun sanitizesAndCapsPreviews() {
        assertEquals("abc d", RichMessage.makePreview("a‮b​c\n\nd"))
        assertEquals(200, RichMessage.makePreview("x".repeat(300)).length)
    }
}
