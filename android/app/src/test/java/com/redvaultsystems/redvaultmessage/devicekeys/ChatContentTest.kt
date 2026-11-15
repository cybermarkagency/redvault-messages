// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileEncryptionInfo
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * Reactions, edits, deletes and files on device keys, against the vectors every client shares (9.3), and the erasure
 * that goes with a delete. No engine needed: the send path is in ChatTest.
 */
class ChatContentTest {
    @get:Rule val folder = TemporaryFolder()

    private val vectors = Json.parseToJsonElement(File("../../desktop/tests/vectors/v3_chat_content_vectors.json").readText()).jsonObject
    private val conversation = vectors.getValue("conversation").jsonPrimitive.content
    private val key = ByteArray(32).also { SecureRandom().nextBytes(it) }

    private val message1 = "aaaaaaaa-0000-4000-8000-000000000001"
    private val edit2 = "aaaaaaaa-0000-4000-8000-000000000002"
    private val reaction3 = "aaaaaaaa-0000-4000-8000-000000000003"
    private val delete4 = "aaaaaaaa-0000-4000-8000-000000000004"
    private val file5 = "aaaaaaaa-0000-4000-8000-000000000005"

    private fun JsonObject.text(name: String) = get(name)?.jsonPrimitive?.content

    private fun toRecord(v: JsonObject, arrivedAt: Long): ChatRecord {
        val target = v.text("target")
        val op = when (v.text("t")) {
            "edit" -> ChatOp.edit(target!!, v.text("body")!!)
            "delete" -> ChatOp.delete(target!!)
            "reaction" -> ChatOp.reaction(target!!, v.text("emoji")!!)
            else -> null
        }
        return ChatRecord(
            conversation, v.text("id")!!, v.text("from")!!, "d", v.getValue("ts").jsonPrimitive.long, arrivedAt,
            if (op != null) "" else v.text("body")!!, "in", "received", op = op,
        )
    }

    private data class Row(val id: String, val text: String, val edited: Boolean, val deleted: Boolean, val reactions: List<Pair<String, String>>)

    private fun shown(records: List<ChatRecord>) =
        foldChat(records).map { m -> Row(m.record.id, m.record.text, m.editedAt != null, m.deleted, m.reactions.map { it.userId to it.emoji }) }

    private fun expected(c: JsonObject) = c.getValue("expect").jsonArray.map { row ->
        val r = row.jsonObject
        Row(
            r.text("id")!!, r.text("text")!!, r.getValue("edited").jsonPrimitive.boolean, r.getValue("deleted").jsonPrimitive.boolean,
            r.getValue("reactions").jsonArray.map { it.jsonObject.text("user")!! to it.jsonObject.text("emoji")!! },
        )
    }

    @Test fun `every fold case gives the same answer in forward and reversed arrival order`() {
        val cases = vectors.getValue("fold").jsonArray.map { it.jsonObject }
        assertEquals(6, cases.size)
        for (c in cases) {
            val name = c.text("name")!!
            val records = c.getValue("records").jsonArray.mapIndexed { i, v -> toRecord(v.jsonObject, i.toLong()) }
            assertEquals("$name, forward", expected(c), shown(records))
            assertEquals("$name, reversed", expected(c), shown(records.asReversed()))
        }
    }

    @Test fun `every decode case is accepted or ignored as the vectors say`() {
        val cases = vectors.getValue("decode").jsonArray.map { it.jsonObject }
        assertEquals(9, cases.size)
        for (c in cases) {
            val decoded = decodeEnvelope(c.getValue("envelope").toString(), conversation, 2000)
            val kind = when (decoded) {
                is DecodedEnvelope.Op -> "op"
                is DecodedEnvelope.Ignore -> "ignore"
                is DecodedEnvelope.Shown -> "message"
                DecodedEnvelope.Newer -> "newer"
                is DecodedEnvelope.Call -> "call"
                is DecodedEnvelope.CallKey -> "callkey"
                is DecodedEnvelope.StoryKey -> "storykey"
                is DecodedEnvelope.StoryAnswer -> "storyanswer"
            }
            assertEquals(c.text("name"), c.text("accept"), kind)
        }
    }

    private val file = FileDescriptor(
        type = "file",
        version = 1,
        objectId = "33333333-3333-4333-8333-333333333333",
        fileKey = "c2VjcmV0LWZpbGUta2V5LXNlY3JldC1maWxlLWtleSE=",
        filename = "notes.txt",
        mimeType = "text/plain",
        originalSize = 5,
        encryptedSize = 46,
        ciphertextSha256 = "a".repeat(64),
        encryption = FileEncryptionInfo("xchacha20poly1305-secretstream", 1, "aGVhZGVyaGVhZGVyaGVhZGVyaGVhZGVy", 65536),
    )

    @Test fun `envelopes fill 256-byte buckets and read back the same`() {
        assertEquals(256, encodeText(conversation, "hi", edit2, 1000).toByteArray().size)
        assertEquals(256, encodeText(conversation, "hi".repeat(30), edit2, 1000).toByteArray().size)
        assertEquals(0, encodeText(conversation, "é".repeat(200), edit2, 1000).toByteArray().size % 256)
        assertEquals(256, encodeOp(conversation, ChatOp.delete(message1), edit2, 1000).toByteArray().size)
    }

    @Test fun `a file's descriptor and key round-trip in the envelope`() {
        val decoded = decodeEnvelope(encodeFile(conversation, file, message1, 1000), conversation, 2000)
        assertEquals(DecodedEnvelope.Shown(message1, 1000, "", file), decoded)
    }

    @Test fun `the ops round-trip, and the text envelope still reads`() {
        for (op in listOf(ChatOp.reaction(message1, "👍"), ChatOp.reaction(message1, ""), ChatOp.edit(message1, "new"), ChatOp.delete(message1))) {
            assertEquals(DecodedEnvelope.Op(edit2, 1000, op), decodeEnvelope(encodeOp(conversation, op, edit2, 1000), conversation, 2000))
        }
        assertEquals(DecodedEnvelope.Shown(edit2, 1000, "hello"), decodeEnvelope(encodeText(conversation, "hello", edit2, 1000), conversation, 2000))
    }

    @Test fun `a newer version waits, a broken envelope doesn't, and a clock is clamped`() {
        fun env(v: Int = 1, ts: Long = 1000) = buildJsonObject {
            put("v", v)
            put("t", "text")
            put("id", message1)
            put("c", conversation)
            put("ts", ts)
            put("body", "x")
        }.toString()
        assertEquals(DecodedEnvelope.Newer, decodeEnvelope(env(v = 2), conversation, 2000))
        assertTrue(decodeEnvelope(env(v = 0), conversation, 2000) is DecodedEnvelope.Ignore)
        assertTrue(decodeEnvelope("not json", conversation, 2000) is DecodedEnvelope.Ignore)
        assertTrue(decodeEnvelope("[1]", conversation, 2000) is DecodedEnvelope.Ignore)
        assertTrue(decodeEnvelope(env(), null, 2000) is DecodedEnvelope.Ignore)
        val now = 100L * 24 * 3600 * 1000
        assertEquals(now + ChatLimits.FUTURE_MS, (decodeEnvelope(env(ts = Long.MAX_VALUE), conversation, now) as DecodedEnvelope.Shown).ts)
        assertEquals(now - ChatLimits.PAST_MS, (decodeEnvelope(env(ts = 0), conversation, now) as DecodedEnvelope.Shown).ts)
    }

    @Test fun `an edit isn't valid on a file`() {
        val records = listOf(
            ChatRecord(conversation, file5, "alice", "d", 1000, 1, "", "in", "received", file = file),
            ChatRecord(conversation, edit2, "alice", "d", 2000, 2, "", "in", "received", op = ChatOp.edit(file5, "renamed")),
        )
        val shown = foldChat(records).single()
        assertNull(shown.editedAt)
        assertEquals(file, shown.record.file)
    }

    private fun historyFile() = File(folder.root, "history.bin")
    private fun history() = EncryptedChatHistory(historyFile(), key)

    /** What's actually on disk, opened with the key. */
    private fun onDisk(): String {
        val data = historyFile().readBytes()
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, data.copyOfRange(0, 12)))
        cipher.updateAAD("redvault-v3-history".toByteArray())
        return String(cipher.doFinal(data, 12, data.size - 12), Charsets.UTF_8)
    }

    private fun message(id: String, from: String, text: String, ts: Long = 1000, file: FileDescriptor? = null) =
        ChatRecord(conversation, id, from, "d", ts, ts, text, "in", "received", file = file)

    private fun op(id: String, from: String, op: ChatOp, ts: Long) = ChatRecord(conversation, id, from, "d", ts, ts, "", "in", "received", op = op)

    @Test fun `a delete erases the text, the edits and the reactions from what's stored`() {
        val h = history()
        h.put(message(message1, "alice", "the original words"))
        h.put(op(edit2, "alice", ChatOp.edit(message1, "the edited words"), 2000))
        h.put(op(reaction3, "bob", ChatOp.reaction(message1, "🔥"), 2500))
        assertTrue(onDisk().contains("the original words"))
        h.put(op(delete4, "alice", ChatOp.delete(message1), 3000))
        h.eraseDeleted(conversation)

        val stored = onDisk()
        for (gone in listOf("the original words", "the edited words", "🔥", edit2, reaction3)) assertFalse(gone, stored.contains(gone))
        val again = history()
        assertEquals(setOf(message1, delete4), again.messages(conversation).map { it.id }.toSet())
        assertEquals("", again.messages(conversation).first { it.id == message1 }.text)
        assertTrue(foldChat(again.messages(conversation)).single().deleted)
    }

    @Test fun `a delete erases a file's key`() {
        val h = history()
        h.put(message(file5, "alice", "", file = file))
        assertTrue(onDisk().contains(file.fileKey))
        h.put(op(delete4, "alice", ChatOp.delete(file5), 3000))
        h.eraseDeleted(conversation)

        val stored = onDisk()
        assertFalse(stored.contains(file.fileKey))
        assertFalse(stored.contains(file.filename))
        assertNull(history().messages(conversation).first { it.id == file5 }.file)
    }

    @Test fun `a delete that arrives first erases the message when it comes, and a late reaction goes too`() {
        val h = history()
        h.put(op(delete4, "alice", ChatOp.delete(message1), 3000))
        h.eraseDeleted(conversation)
        h.put(message(message1, "alice", "too late"))
        h.eraseDeleted(conversation)
        assertFalse(onDisk().contains("too late"))
        h.put(op(reaction3, "bob", ChatOp.reaction(message1, "👍"), 2500))
        h.eraseDeleted(conversation)
        assertFalse(h.has(conversation, reaction3))
        // A redelivered copy of the original can't bring it back: the first record per id stays.
        assertFalse(h.put(message(message1, "alice", "too late")))
        assertFalse(onDisk().contains("too late"))
    }

    @Test fun `someone else's delete erases nothing, and other chats aren't touched`() {
        val h = history()
        val other = "22222222-2222-4222-8222-222222222222"
        h.put(message(message1, "alice", "stays"))
        h.put(op(delete4, "mallory", ChatOp.delete(message1), 3000))
        h.put(message(message1, "alice", "another chat").copy(conversationId = other))
        h.eraseDeleted(conversation)
        assertEquals("stays", h.messages(conversation).first { it.id == message1 }.text)
        assertFalse(foldChat(h.messages(conversation)).single().deleted)

        h.put(op(delete4, "alice", ChatOp.delete(message1), 3000), replace = true)
        h.eraseDeleted(conversation)
        assertEquals("another chat", h.messages(other).single().text)
    }

    @Test fun `hidden records don't count as unread`() {
        val h = history()
        h.put(message(message1, "alice", "hi"))
        h.put(op(reaction3, "alice", ChatOp.reaction(message1, "👍"), 2000))
        assertEquals(1, h.unread(conversation))
    }

    @Test fun `files and ops survive being stored`() {
        val h = history()
        h.put(message(file5, "alice", "", file = file))
        h.put(op(reaction3, "bob", ChatOp.reaction(file5, "👍"), 2000))
        val again = history().messages(conversation)
        assertEquals(file, again.first { it.id == file5 }.file)
        assertEquals(ChatOp.reaction(file5, "👍"), again.first { it.id == reaction3 }.op)
    }

    @Test fun `a history stored before files and ops existed still opens`() {
        val old = """{"records":[{"conversationId":"$conversation","id":"$message1","fromUserId":"alice","fromDeviceId":"d","ts":1000,""" +
            """"arrivedAt":1000,"text":"from before","direction":"in","status":"received"}],"pins":["$conversation"],"read":{}}"""
        val iv = ByteArray(12).also { SecureRandom().nextBytes(it) }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, iv))
        cipher.updateAAD("redvault-v3-history".toByteArray())
        historyFile().writeBytes(iv + cipher.doFinal(old.toByteArray()))

        val h = history()
        assertTrue(h.pinned(conversation))
        val shown = foldChat(h.messages(conversation)).single()
        assertEquals("from before", shown.record.text)
        assertNull(shown.record.file)
        assertNull(shown.record.op)
    }

    // --- Part A (docs/signal-integration/08-part-a-chat-features.md): the shared vectors and erasure ---

    private val partA = Json.parseToJsonElement(File("../../desktop/tests/vectors/v3_chat_content_part_a_vectors.json").readText()).jsonObject

    /** A vector record as this phone would store it: through the same decoder as a received envelope (ts kept as given). */
    private fun partARecord(v: JsonObject, arrivedAt: Long): ChatRecord {
        val ts = v.getValue("ts").jsonPrimitive.long
        val envelope = buildJsonObject {
            put("v", 1)
            put("c", conversation)
            v.forEach { (k, value) -> if (k != "from") put(k, value) }
        }.toString()
        val base = ChatRecord(conversation, v.text("id")!!, v.text("from")!!, "d", ts, arrivedAt, "", "in", "received")
        return when (val d = decodeEnvelope(envelope, conversation, ts)) {
            is DecodedEnvelope.Shown -> base.copy(text = d.text, file = d.file, reply = d.reply, forwarded = d.forwarded, exp = d.exp, poll = d.poll)
            is DecodedEnvelope.Op -> base.copy(op = d.op)
            else -> throw AssertionError("vector record ${v.text("id")} doesn't decode: $d")
        }
    }

    /** Every field the vectors compare, in their shape. */
    private fun asVector(m: ShownMessage): JsonObject = buildJsonObject {
        put("id", m.record.id)
        put("text", m.record.text)
        put("edited", m.editedAt != null)
        put("deleted", m.deleted)
        put("reactions", JsonArray(m.reactions.map { buildJsonObject { put("user", it.userId); put("emoji", it.emoji) } }))
        put("replyTo", m.replyTo?.let(::JsonPrimitive) ?: JsonNull)
        put("forwarded", m.forwarded)
        put("pinned", m.pinned)
        put(
            "poll",
            m.poll?.let { p ->
                buildJsonObject {
                    put("options", JsonArray(p.options.map(::JsonPrimitive)))
                    put("multi", p.multi)
                    put("votes", JsonArray(p.votes.map(::JsonPrimitive)))
                    put("ended", p.ended)
                }
            } ?: JsonNull,
        )
        put("receipts", JsonArray(m.receipts.map { buildJsonObject { put("user", it.userId); put("status", it.status) } }))
        put("expiresAt", m.expiresAt?.let(::JsonPrimitive) ?: JsonNull)
    }

    @Test fun `every part A fold case gives the vectors' answer, field by field, in any arrival order`() {
        val cases = partA.getValue("fold").jsonArray.map { it.jsonObject }
        assertEquals(21, cases.size)
        for (c in cases) {
            val name = c.text("name")!!
            val now = c["now"]?.jsonPrimitive?.long
            val records = c.getValue("records").jsonArray.mapIndexed { i, v -> partARecord(v.jsonObject, i.toLong()) }
            val expected = c.getValue("expect").jsonArray.map { it.jsonObject }
            for ((order, arrival) in listOf("forward" to records, "reversed" to records.asReversed(), "shuffled" to records.shuffled(java.util.Random(7)))) {
                assertEquals("$name, $order", expected, foldChat(arrival, now).map(::asVector))
            }
        }
    }

    @Test fun `every part A decode case is accepted or ignored as the vectors say`() {
        val cases = partA.getValue("decode").jsonArray.map { it.jsonObject }
        assertEquals(24, cases.size)
        val part = partA.getValue("conversation").jsonPrimitive.content
        for (c in cases) {
            val kind = when (decodeEnvelope(c.getValue("envelope").toString(), part, 2000)) {
                is DecodedEnvelope.Op -> "op"
                is DecodedEnvelope.Ignore -> "ignore"
                is DecodedEnvelope.Shown -> "message"
                DecodedEnvelope.Newer -> "newer"
                is DecodedEnvelope.Call -> "call"
                is DecodedEnvelope.CallKey -> "callkey"
                is DecodedEnvelope.StoryKey -> "storykey"
                is DecodedEnvelope.StoryAnswer -> "storyanswer"
            }
            assertEquals(c.text("name"), c.text("accept"), kind)
        }
    }

    @Test fun `the part A fields and types round-trip through the envelope`() {
        val reply = ChatReply(message1, "hi?")
        val extras = MessageExtras(reply = reply, forwarded = true, exp = 3600)
        assertEquals(
            DecodedEnvelope.Shown(edit2, 1000, "yes", reply = reply, forwarded = true, exp = 3600),
            decodeEnvelope(encodeText(conversation, "yes", edit2, 1000, extras), conversation, 2000),
        )
        assertEquals(
            DecodedEnvelope.Shown(edit2, 1000, "", file, forwarded = true),
            decodeEnvelope(encodeFile(conversation, file, edit2, 1000, MessageExtras(forwarded = true)), conversation, 2000),
        )
        val poll = ChatPoll(listOf("A", "B"), multi = true)
        assertEquals(
            DecodedEnvelope.Shown(edit2, 1000, "Lunch?", exp = 60, poll = poll),
            decodeEnvelope(encodePoll(conversation, "Lunch?", poll, edit2, 1000, exp = 60), conversation, 2000),
        )
        for (op in listOf(
            ChatOp.vote(message1, listOf(0, 1)), ChatOp.vote(message1, emptyList()), ChatOp.end(message1),
            ChatOp.pin(message1, true), ChatOp.pin(message1, false),
            ChatOp.receipt(listOf(message1, file5), ChatOp.READ), ChatOp.receipt(listOf(message1), ChatOp.DELIVERED),
        )) {
            assertEquals(DecodedEnvelope.Op(edit2, 1000, op), decodeEnvelope(encodeOp(conversation, op, edit2, 1000), conversation, 2000))
        }
        // No "fwd" at all on a message that wasn't forwarded, and a receipt carries no single target.
        assertFalse(encodeText(conversation, "x", edit2, 1000).contains("fwd"))
        assertFalse(encodeOp(conversation, ChatOp.receipt(listOf(message1), ChatOp.READ), edit2, 1000).contains("\"target\""))
    }

    @Test fun `malformed part A fields make the whole envelope ignored`() {
        fun env(t: String, fields: kotlinx.serialization.json.JsonObjectBuilder.() -> Unit) = buildJsonObject {
            put("v", 1)
            put("t", t)
            put("id", message1)
            put("c", conversation)
            put("ts", 1000)
            fields()
        }.toString()
        fun ignored(e: String) = assertTrue(e, decodeEnvelope(e, conversation, 2000) is DecodedEnvelope.Ignore)
        val ab = JsonArray(listOf(JsonPrimitive("A"), JsonPrimitive("B")))
        ignored(env("text") { put("body", "x"); put("exp", 0) })
        ignored(env("text") { put("body", "x"); put("exp", 1.5) })
        ignored(env("text") { put("body", "x"); put("exp", "60") })
        ignored(env("text") { put("body", "x"); put("fwd", "true") })
        ignored(env("text") { put("body", "x"); put("reply", buildJsonObject { put("target", message1.uppercase()); put("preview", "") }) })
        ignored(env("text") { put("body", "x"); put("reply", buildJsonObject { put("target", message1) }) })
        ignored(env("file") { put("file", Json.encodeToJsonElement(FileDescriptor.serializer(), file)); put("fwd", 1) })
        ignored(env("poll") { put("question", ""); put("options", ab); put("multi", false) })
        ignored(env("poll") { put("question", "Q"); put("options", JsonArray(listOf(JsonPrimitive("A"), JsonPrimitive("")))); put("multi", false) })
        ignored(env("poll") { put("question", "Q"); put("options", ab) })
        ignored(env("poll") { put("question", "Q".repeat(301)); put("options", ab); put("multi", false) })
        ignored(env("vote") { put("target", message1); put("choices", JsonArray(listOf(JsonPrimitive(10)))) })
        ignored(env("vote") { put("target", message1); put("choices", JsonArray(listOf(JsonPrimitive(-1)))) })
        ignored(env("vote") { put("target", message1); put("choices", JsonArray(listOf(JsonPrimitive(1.5)))) })
        ignored(env("vote") { put("target", message1) })
        ignored(env("pin") { put("target", message1); put("pinned", "true") })
        ignored(env("receipt") { put("targets", JsonArray(emptyList())); put("status", "read") })
        ignored(env("receipt") { put("targets", JsonArray(listOf(JsonPrimitive("nope")))); put("status", "read") })
        ignored(env("end") { put("target", "nope") })
        // At the limits: still fine.
        val longest = env("text") {
            put("body", "x")
            put("exp", 2_419_200)
            put("reply", buildJsonObject { put("target", message1); put("preview", "é".repeat(200)) })
        }
        assertTrue(decodeEnvelope(longest, conversation, 2000) is DecodedEnvelope.Shown)
    }

    @Test fun `a reply's preview is cut to 200 characters without splitting one, and only a message id can be answered`() {
        assertEquals(200, replyPreview("x".repeat(500)).length)
        assertEquals(199, replyPreview("x".repeat(199) + "😀" + "y").length)
        assertEquals("short", replyPreview("short"))
        assertNull(chatReply("not-an-id", "x"))
        assertEquals(ChatReply(message1, "x"), chatReply(message1.uppercase(), "x"))
    }

    @Test fun `an expired message is erased from what's stored, with its file key and everything about it`() {
        val h = history()
        h.put(message(file5, "alice", "", file = file).copy(exp = 60, reply = ChatReply(message1, "the quoted words")))
        h.put(message(message1, "bob", "stays", ts = 1500))
        h.put(op(reaction3, "bob", ChatOp.reaction(file5, "🔥"), 2000))
        h.put(op(edit2, "bob", ChatOp.receipt(listOf(file5, message1), ChatOp.READ), 2500))
        h.eraseDeleted(conversation, now = 30_000)
        assertTrue(onDisk().contains(file.fileKey))

        h.eraseDeleted(conversation, now = 61_000)
        val stored = onDisk()
        for (gone in listOf(file.fileKey, file.filename, "the quoted words", "🔥", reaction3)) assertFalse(gone, stored.contains(gone))
        assertEquals(listOf(message1), foldChat(h.messages(conversation), 61_000).map { it.record.id })
        // The receipt keeps only the message that's still there.
        assertEquals(listOf(message1), h.messages(conversation).first { it.id == edit2 }.op!!.about)
        // A reaction that comes late for it is erased too.
        h.put(op(delete4, "carol", ChatOp.reaction(file5, "👍"), 3000))
        h.eraseDeleted(conversation, now = 62_000)
        assertFalse(h.has(conversation, delete4))
    }

    @Test fun `an expired message doesn't count as unread, nor one read on another of your devices`() {
        val h = history()
        val now = System.currentTimeMillis()
        h.put(message(message1, "alice", "gone", ts = now - 120_000).copy(exp = 60))
        h.put(message(edit2, "alice", "read elsewhere", ts = now))
        h.put(message(file5, "alice", "unread", ts = now))
        h.put(op(reaction3, "me", ChatOp.receipt(listOf(edit2), ChatOp.READ), now).copy(direction = "out", status = "sent"))
        assertEquals(1, h.unread(conversation))
    }

    @Test fun `what receipts are owed, and none twice`() {
        val now = 10_000L
        val records = listOf(
            message(message1, "alice", "a", ts = 1000),
            message(edit2, "bob", "b", ts = 2000),
            message(file5, "me", "mine", ts = 3000).copy(direction = "out", status = "sent"),
            op(reaction3, "alice", ChatOp.pin(message1, true), 4000),
        )
        assertEquals(
            mapOf(("alice" to ChatOp.DELIVERED) to listOf(message1), ("bob" to ChatOp.DELIVERED) to listOf(edit2)),
            V3Chat.owedReceipts(records, "me", emptySet(), now),
        )
        assertEquals(
            mapOf(("alice" to ChatOp.READ) to listOf(message1), ("bob" to ChatOp.DELIVERED) to listOf(edit2)),
            V3Chat.owedReceipts(records, "me", setOf(message1), now),
        )
        val sent = records + op(delete4, "me", ChatOp.receipt(listOf(message1, edit2), ChatOp.DELIVERED), 5000)
        assertEquals(emptyMap<Pair<String, String>, List<String>>(), V3Chat.owedReceipts(sent, "me", emptySet(), now))
        assertEquals(mapOf(("alice" to ChatOp.READ) to listOf(message1)), V3Chat.owedReceipts(sent, "me", setOf(message1), now))
    }
}
