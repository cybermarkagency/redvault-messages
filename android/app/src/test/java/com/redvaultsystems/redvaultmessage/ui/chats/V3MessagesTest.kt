// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileEncryptionInfo
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.data.model.MessageFolding
import com.redvaultsystems.redvaultmessage.data.model.PinnedView
import com.redvaultsystems.redvaultmessage.data.model.PollView
import com.redvaultsystems.redvaultmessage.data.model.ReplyView
import com.redvaultsystems.redvaultmessage.devicekeys.ChatPoll
import com.redvaultsystems.redvaultmessage.devicekeys.ChatRecord
import com.redvaultsystems.redvaultmessage.devicekeys.ShownPoll
import com.redvaultsystems.redvaultmessage.devicekeys.ShownReceipt
import com.redvaultsystems.redvaultmessage.devicekeys.ShownMessage
import com.redvaultsystems.redvaultmessage.devicekeys.ShownReaction
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** How a device-key message becomes a bubble, and what can't be done to one yet. */
class V3MessagesTest {
    private val me = "me"
    private val record = ChatRecord("c", "m1", "them", "d", 1000, 1000, "hello", "in", "received")
    private val file = FileDescriptor(
        "file", 1, "33333333-3333-4333-8333-333333333333", "a2V5", "notes.txt", "text/plain", 5, 46, "a".repeat(64),
        FileEncryptionInfo("xchacha20poly1305-secretstream", 1, "aGVhZGVy", 65536),
    )

    @Test fun `a deleted message says so and keeps nothing`() {
        val message = V3Messages.toMessage(ShownMessage(record.copy(text = ""), deleted = true), me)
        assertEquals("This message was deleted.", message.text)
        assertTrue(message.deleted)
        assertEquals(MessageKind.Text, message.kind)
        assertNull(message.file)
    }

    @Test fun `a file shows its name and can be opened`() {
        val message = V3Messages.toMessage(ShownMessage(record.copy(text = "", file = file)), me)
        assertEquals(MessageKind.File, message.kind)
        assertEquals("notes.txt", message.text)
        assertEquals(file, message.file)
        assertEquals("available", message.fileStatus)
    }

    @Test fun `an edit shows as edited, reactions are counted, and yours is marked`() {
        val message = V3Messages.toMessage(
            ShownMessage(record, editedAt = 2000, reactions = listOf(ShownReaction("a", "🔥"), ShownReaction("b", "🔥"), ShownReaction(me, "👍"))),
            me,
        )
        assertEquals(2000L, message.editedAtMs)
        assertEquals(setOf(Triple("🔥", 2, false), Triple("👍", 1, true)), message.reactions.map { Triple(it.emoji, it.count, it.mine) }.toSet())
    }

    @Test fun `a failed send shows as not sent`() {
        val message = V3Messages.toMessage(ShownMessage(record.copy(direction = "out", status = "failed")), me)
        assertTrue(message.fromMe && message.queued && message.queueHeld)
    }

    @Test fun `device-key messages can be forwarded and ticked (part A), a deleted one can't and never matches a search`() {
        val message = V3Messages.toMessage(ShownMessage(record), me)
        assertTrue(MessageSelection.canForward(message))
        assertTrue(MessageSelection.selectable(message))
        val deleted = V3Messages.toMessage(ShownMessage(record.copy(text = ""), deleted = true), me)
        assertFalse(MessageSelection.canForward(deleted))
        assertFalse(MessageSelection.selectable(deleted))
        assertEquals(emptyList<String>(), ThreadSearch.hits(listOf(deleted), "deleted"))
        // Its delete erases what goes with it on every device: no extra ids to delete.
        assertEquals(listOf("m1"), MessageSelection.deletions(listOf(message.copy(fromMe = true)), setOf("m1")))
    }

    @Test fun `a reply quotes the original as this phone has it, or the preview it carried`() {
        val original = ShownMessage(record.copy(id = "m0", text = "the question", direction = "out", status = "sent"))
        val reply = ShownMessage(record, replyTo = "m0", replyPreview = "old words")
        val known = V3Messages.toMessage(reply, me, mapOf("m0" to original))
        assertEquals(ReplyView("m0", "the question", fromMe = true), known.replyTo)
        val unknown = V3Messages.toMessage(reply, me)
        assertEquals(ReplyView("m0", "old words", fromMe = false), unknown.replyTo)
    }

    @Test fun `a poll becomes the poll card, with your vote marked`() {
        val poll = ShownMessage(
            record.copy(text = "Lunch?", poll = ChatPoll(listOf("A", "B"), multi = false)),
            poll = ShownPoll(listOf("A", "B"), multi = false, votes = listOf(1, 1), ended = true, choices = mapOf(me to listOf(1), "x" to listOf(0))),
        )
        val view = V3Messages.toMessage(poll, me).poll!!
        assertEquals(PollView("Lunch?", listOf("A", "B"), multiple = false, closed = true, counts = listOf(1, 1), mine = listOf(1), voters = 2), view)
    }

    @Test fun `ticks come from receipts on your own messages, and pins and timers carry over`() {
        val mine = record.copy(direction = "out", status = "sent", fromUserId = me)
        val delivered = V3Messages.toMessage(ShownMessage(mine, receipts = listOf(ShownReceipt("a", "delivered"))), me)
        assertTrue(delivered.delivered && !delivered.seen)
        val read = V3Messages.toMessage(ShownMessage(mine, receipts = listOf(ShownReceipt("a", "delivered"), ShownReceipt("b", "read"))), me)
        assertTrue(read.delivered && read.seen)
        assertEquals(2, read.receiptsFrom)
        assertEquals(1, read.readBy)
        // Someone else's message never shows ticks.
        val theirs = V3Messages.toMessage(ShownMessage(record, receipts = listOf(ShownReceipt(me, "read"))), me)
        assertFalse(theirs.delivered || theirs.seen)

        val pinned = V3Messages.toMessage(ShownMessage(record, pinned = true, pinnedBy = me, expiresAt = 61_000, forwarded = true), me)
        assertTrue(pinned.pinned && pinned.pinnedByMe && pinned.forwarded)
        assertEquals(61_000L, pinned.deleteAtMs)
        assertEquals(listOf(PinnedView("m1", "hello", byMe = true, senderId = me)), MessageFolding.pinnedFrom(listOf(pinned)))
    }
}
