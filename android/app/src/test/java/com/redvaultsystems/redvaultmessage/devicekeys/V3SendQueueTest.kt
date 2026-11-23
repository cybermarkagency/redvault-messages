// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.util.Base64

/**
 * The sealed device-key queue (devicekeys/V3SendQueue.kt, docs/tor-by-default.md rule 5): posts made while Tor is
 * down wait there already encrypted, in order, across restarts, and nothing readable is ever on disk.
 */
class V3SendQueueTest {
    @get:Rule val folder = TemporaryFolder()

    private val user = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val contact = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    private val chat = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"
    private val wrapper = SoftwareKeyWrapper()
    private val dir get() = File(folder.root, "queue")

    /** Records the posts that reach the "server"; [down] stands for Tor being unavailable. */
    private class Server : V3Api by NotUsed {
        var down = true
        var refuseWith: Int? = null
        /** Gap 1: a marked post is refused with 409 device_list_stale (a device was linked meanwhile). */
        var staleWhenMarked = false
        /** Every post is refused with 409 device_list_stale, marked or not. */
        var alwaysStale = false
        val posts = mutableListOf<String>()
        /** The allDevices flag of every post that reached the server, refused or not. */
        val marks = mutableListOf<Boolean>()

        private fun check(allDevices: Boolean) {
            if (down) throw ApiException(0, "tor_unavailable", "Tor connection unavailable")
            marks += allDevices
            refuseWith?.let { throw ApiException(it, "refused", "refused") }
            if (alwaysStale || (staleWhenMarked && allDevices)) throw ApiException(409, "device_list_stale", "stale")
        }

        override suspend fun sendMessages(senderDeviceId: String, conversationId: String?, messages: List<Triple<String, String, String>>, allDevices: Boolean) {
            check(allDevices)
            posts += messages.joinToString { it.third }
        }

        override suspend fun sendGroupMessage(senderDeviceId: String, conversationId: String, body: String, recipients: List<Pair<String, String>>, allDevices: Boolean) {
            check(allDevices)
            posts += "group:$body"
        }
    }

    private object NotUsed : V3Api {
        override suspend fun enabled() = error("not used")
        override suspend fun registerDevice(upload: kotlinx.serialization.json.JsonObject) = error("not used")
        override suspend fun listDevices() = error("not used")
        override suspend fun removeDevice(deviceId: String) = error("not used")
        override suspend fun deviceList(userId: String, conversationId: String?) = error("not used")
        override suspend fun putDeviceList(deviceList: kotlinx.serialization.json.JsonObject, rotationProof: String?) = error("not used")
        override suspend fun cancelReset(signature: String) = error("not used")
        override suspend fun createEnrollment(newDeviceId: String) = error("not used")
        override suspend fun enrollments() = error("not used")
        override suspend fun postEnrollmentMessage(id: String, fromDeviceId: String, body: String) = error("not used")
        override suspend fun enrollmentMessages(id: String, after: Int) = error("not used")
        override suspend fun deleteEnrollment(id: String) = error("not used")
        override suspend fun sendMessages(senderDeviceId: String, conversationId: String?, messages: List<Triple<String, String, String>>, allDevices: Boolean) = error("not used")
        override suspend fun inbox(deviceId: String) = error("not used")
        override suspend fun ack(deviceId: String, ids: List<String>) = error("not used")
        override suspend fun putRecoveryBackup(backup: String, signature: String) = error("not used")
        override suspend fun recoveryBackup() = error("not used")
        override suspend fun events(after: Int) = error("not used")
        override suspend fun claimKeys(conversationId: String?, devices: List<Pair<String, String>>) = error("not used")
        override suspend fun conversationDeviceLists(conversationId: String) = error("not used")
        override suspend fun sendGroupMessage(senderDeviceId: String, conversationId: String, body: String, recipients: List<Pair<String, String>>, allDevices: Boolean) = error("not used")
    }

    private fun queue() = V3SendQueue(dir, user, wrapper)

    private suspend fun QueueingV3Api.post(messageId: String, body: String, allDevices: Boolean = false) = withContext(V3SendTag(V3Chat.tagOf(chat, messageId))) {
        sendMessages("device-1", chat, listOf(Triple(contact, "their-device", body)), allDevices)
    }

    // --- Gap 1 (docs/signal-integration/13-every-device-and-session-reset.md): queued posts marked allDevices ---

    @Test
    fun aQueuedPostKeepsItsAllDevicesMark() = runBlocking {
        val server = Server()
        val api = QueueingV3Api(server, queue())
        api.post("m1", "marked", allDevices = true)
        api.post("m2", "plain")
        withContext(V3SendTag(V3Chat.tagOf(chat, "m3"))) { api.sendGroupMessage("device-1", chat, "g", listOf(contact to "their-device"), allDevices = true) }
        assertEquals(listOf(true, false, true), queue().items().map { it.allDevices })
        server.down = false
        assertFalse(api.drain().waiting)
        assertEquals(listOf(true, false, true), server.marks)
        assertEquals(listOf("marked", "plain", "group:g"), server.posts)
    }

    @Test
    fun aMarkedQueuedPostRefusedAsStaleGoesOnceMoreUnmarkedWithTheSameBodies() = runBlocking {
        val server = Server()
        val api = QueueingV3Api(server, queue())
        api.post("m1", "ciphertext-1", allDevices = true)
        withContext(V3SendTag(V3Chat.tagOf(chat, "m2"))) { api.sendGroupMessage("device-1", chat, "group-ct", listOf(contact to "their-device"), allDevices = true) }
        server.down = false
        server.staleWhenMarked = true
        val drained = api.drain()
        assertEquals(setOf(V3Chat.tagOf(chat, "m1"), V3Chat.tagOf(chat, "m2")), drained.sent)
        assertTrue(drained.refused.isEmpty())
        assertEquals(listOf(true, false, true, false), server.marks)
        assertEquals(listOf("ciphertext-1", "group:group-ct"), server.posts)
        assertTrue(api.queue.isEmpty())
    }

    @Test
    fun aMarkedQueuedPostStillStaleUnmarkedIsRefusedAsBefore() = runBlocking {
        val server = Server()
        val api = QueueingV3Api(server, queue())
        api.post("m1", "one", allDevices = true)
        api.post("m2", "two")
        server.down = false
        server.alwaysStale = true
        val drained = api.drain()
        assertEquals(setOf(V3Chat.tagOf(chat, "m1"), V3Chat.tagOf(chat, "m2")), drained.refused)
        // m1: marked, then once unmarked; m2 (never marked): once.
        assertEquals(listOf(true, false, false), server.marks)
        assertTrue(server.posts.isEmpty())
        assertTrue(api.queue.isEmpty())
    }

    @Test
    fun anUnmarkedQueuedPostRefusedAsStaleIsNotSentAgain() = runBlocking {
        val server = Server()
        val api = QueueingV3Api(server, queue())
        api.post("m1", "one")
        server.down = false
        server.alwaysStale = true
        assertEquals(setOf(V3Chat.tagOf(chat, "m1")), api.drain().refused)
        assertEquals(listOf(false), server.marks)
    }

    @Test
    fun aRecordSavedBeforeTheMarkReadsAsUnmarked() {
        val old = """{"id":"x","order":1,"kind":"direct","senderDeviceId":"d","conversationId":"c","messages":[{"userId":"u","deviceId":"v","body":"b"}],"tag":"t"}"""
        val item = kotlinx.serialization.json.Json { ignoreUnknownKeys = true }.decodeFromString(QueuedV3Send.serializer(), old)
        assertFalse(item.allDevices)
    }

    @Test
    fun anUntaggedPostInsideATaggedSendIsNeverQueued() = runBlocking {
        val server = Server()
        val api = QueueingV3Api(server, queue())
        val failure = withContext(V3SendTag(V3Chat.tagOf(chat, "m1"))) {
            withContext(V3SendTag.UNTAGGED) { runCatching { api.sendMessages("device-1", chat, listOf(Triple(contact, "their-device", "refresh"))) }.exceptionOrNull() }
        }
        assertTrue("$failure", failure is ApiException && failure.code == "tor_unavailable")
        assertTrue(api.queue.isEmpty())
    }

    @Test
    fun postsMadeWhileTorIsDownWaitAndGoOutInOrder() = runBlocking {
        val server = Server()
        val api = QueueingV3Api(server, queue())
        for (i in 1..5) api.post("m$i", "ciphertext-$i")
        assertTrue(server.posts.isEmpty())
        assertEquals(5, api.queue.size())
        assertEquals((1..5).map { "ciphertext-$it" }, api.queue.items().map { it.messages.single().body })

        // Still down: nothing goes, nothing is lost.
        assertTrue(api.drain().waiting)
        assertEquals(5, api.queue.size())

        server.down = false
        val drained = api.drain()
        assertFalse(drained.waiting)
        assertEquals((1..5).map { "ciphertext-$it" }, server.posts)
        assertEquals((1..5).map { V3Chat.tagOf(chat, "m$it") }.toSet(), drained.sent)
        assertTrue(api.queue.isEmpty())
    }

    @Test
    fun whileAnythingWaitsNewPostsQueueBehindIt() = runBlocking {
        val server = Server()
        val api = QueueingV3Api(server, queue())
        api.post("m1", "first")
        // Tor is back, but "first" hasn't gone yet: "second" mustn't overtake it.
        server.down = false
        api.post("m2", "second")
        assertTrue(server.posts.isEmpty())
        api.drain()
        assertEquals(listOf("first", "second"), server.posts)
        // Nothing queued: posts go straight out.
        api.post("m3", "third")
        assertEquals(listOf("first", "second", "third"), server.posts)
        assertTrue(api.queue.isEmpty())
    }

    @Test
    fun theQueueSurvivesARestart() = runBlocking {
        val server = Server()
        QueueingV3Api(server, queue()).apply {
            post("m1", "one")
            post("m2", "two")
            withContext(V3SendTag(V3Chat.tagOf(chat, "m3"))) { sendGroupMessage("device-1", chat, "group-ciphertext", listOf(contact to "their-device")) }
            queue.close()
        }
        // The app was closed and opened again: a new queue over the same folder (the Keystore key is the same).
        server.down = false
        val again = QueueingV3Api(server, queue())
        assertEquals(3, again.queue.size())
        assertFalse(again.drain().waiting)
        assertEquals(listOf("one", "two", "group:group-ciphertext"), server.posts)
    }

    @Test
    fun nothingReadableIsOnDisk() = runBlocking {
        val server = Server()
        val api = QueueingV3Api(server, queue())
        val marker = "PLAINTEXT-MARKER-the-message-body"
        api.post("11111111-1111-4111-8111-111111111111", marker)
        val bytes = dir.walkTopDown().filter { it.isFile }.map { it.readBytes() }.toList()
        assertTrue(bytes.isNotEmpty())
        val forbidden = listOf(marker, contact, chat, "their-device", "11111111-1111-4111-8111-111111111111", "device-1")
        for (file in bytes) {
            val text = String(file, Charsets.ISO_8859_1)
            for (word in forbidden) {
                assertFalse("'$word' is on disk", text.contains(word))
                assertFalse("'$word' is on disk in base64", text.contains(Base64.getEncoder().encodeToString(word.toByteArray()).dropLast(4)))
            }
        }
        // Without the Keystore key the records don't open at all.
        assertTrue(V3SendQueue(dir, user, SoftwareKeyWrapper()).items().isEmpty())
    }

    @Test
    fun recordsCantBeSwapped() = runBlocking {
        val server = Server()
        val api = QueueingV3Api(server, queue())
        api.post("m1", "one")
        api.post("m2", "two")
        val files = dir.listFiles { f -> f.name.endsWith(".q") }!!.sortedBy { it.name }
        val a = files[0].readBytes()
        files[0].writeBytes(files[1].readBytes())
        files[1].writeBytes(a)
        // Each record is bound to its own name: swapped, neither opens.
        assertTrue(queue().items().isEmpty())
    }

    @Test
    fun aRefusedPostDropsItsMessageAndTheRestGoOn() = runBlocking {
        val server = Server()
        val api = QueueingV3Api(server, queue())
        api.post("m1", "one")
        api.post("m2", "two")
        server.down = false
        server.refuseWith = 409
        val drained = api.drain()
        assertEquals(setOf(V3Chat.tagOf(chat, "m1"), V3Chat.tagOf(chat, "m2")), drained.refused)
        assertTrue(api.queue.isEmpty())

        server.refuseWith = 401
        server.down = true
        api.post("m3", "three")
        server.down = false
        try {
            api.drain()
            fail("a 401 means the session expired")
        } catch (e: ApiException) {
            assertEquals(401, e.status)
        }
        assertEquals("kept for after sign-in", 1, api.queue.size())
    }

    @Test
    fun untaggedPostsAreNeverQueued() = runBlocking {
        // Pairing messages and the like aren't chat messages: they fail as before instead of waiting.
        val server = Server()
        val api = QueueingV3Api(server, queue())
        val failure = runCatching { api.sendMessages("device-1", null, listOf(Triple(contact, "their-device", "pairing"))) }.exceptionOrNull()
        assertTrue(failure is ApiException && failure.code == "tor_unavailable")
        assertTrue(api.queue.isEmpty())
    }

    @Test
    fun aServerAnswerOtherThanBusyIsNotQueued() = runBlocking {
        val server = Server()
        server.down = false
        server.refuseWith = 409
        val api = QueueingV3Api(server, queue())
        val failure = runCatching { api.post("m1", "one") }.exceptionOrNull()
        assertTrue(failure is ApiException && failure.status == 409)
        assertTrue(api.queue.isEmpty())
        // Busy (5xx, 429): it waits.
        server.refuseWith = 503
        api.post("m2", "two")
        assertEquals(1, api.queue.size())
    }
}
