package com.redvaultsystems.redvaultmessage.ui.chats

import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileEncryptionInfo
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.devicekeys.ChatRecord
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

    @Test fun `device-key messages can't be forwarded or ticked, and a deleted one never matches a search`() {
        val message = V3Messages.toMessage(ShownMessage(record), me)
        assertFalse(MessageSelection.canForward(message))
        assertFalse(MessageSelection.selectable(message))
        val deleted = V3Messages.toMessage(ShownMessage(record.copy(text = ""), deleted = true), me)
        assertEquals(emptyList<String>(), ThreadSearch.hits(listOf(deleted), "deleted"))
    }
}
