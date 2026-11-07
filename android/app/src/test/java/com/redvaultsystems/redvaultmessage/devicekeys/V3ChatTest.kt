package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom

/** The chat screens' door (V3Chat) end to end on the JVM: real engine, in-memory server, two accounts. */
class V3ChatTest {
    @get:Rule val folder = TemporaryFolder()

    private lateinit var engine: EngineRunner
    private val server = FakeV3Server()
    private val alice = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val bob = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    private val chat = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"

    @Before fun load() {
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't built for this computer", call != null)
        engine = EngineRunner(call!!)
    }

    private fun legacy() = ByteArray(32).also { SecureRandom().nextBytes(it) }

    private suspend fun account(user: String, name: String, setUp: Boolean = true): V3Chat {
        val storage = DeviceStorage(File(folder.root, name), SoftwareKeyWrapper())
        val api = server.api(user)
        if (setUp) setUpFirstDevice(user, legacy(), storage, engine, api)
        val keys = DeviceKeysSession(user, ::legacy, storage, engine, api)
        return V3Chat(keys, user, File(folder.root, "$name-history"))
    }

    @Test fun `messages flow between two accounts through the screens' door`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")

        val sent = a.send(chat, isGroup = false, contactUserId = bob, text = "hi bob")
        assertTrue(sent is SendOutcome.Sent)
        assertTrue(a.pinned(chat))
        assertEquals(listOf("hi bob"), a.records(chat).map { it.text })

        val report = b.receive()!!
        assertEquals(listOf("hi bob"), report.received.map { it.text })
        assertEquals(listOf("hi bob"), b.records(chat).map { it.text })
        assertEquals("in", b.records(chat).single().direction)
        assertTrue(b.pinned(chat))
    }

    @Test fun `a phone without device keys says not mine`() = runBlocking {
        val a = account(alice, "a", setUp = false)
        assertNull(a.send(chat, isGroup = false, contactUserId = bob, text = "usual way"))
        assertNull(a.receive())
        assertTrue(a.records(chat).isEmpty())
    }

    @Test fun `a chat stays pinned when device keys are unavailable, so the app can refuse instead of falling back`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        assertTrue(a.send(chat, isGroup = false, contactUserId = bob, text = "first") is SendOutcome.Sent)

        // The app restarts and this phone's device can't be opened (no device storage here): the history can't be read either.
        val restarted = V3Chat(DeviceKeysSession(alice, ::legacy, DeviceStorage(File(folder.root, "empty"), SoftwareKeyWrapper()), engine, server.api(alice)), alice, File(folder.root, "a-history"))
        assertNull(restarted.send(chat, isGroup = false, contactUserId = bob, text = "second"))
        assertTrue(restarted.pinned(chat))
        assertTrue(!restarted.pinned("11111111-1111-4111-8111-111111111111"))
        assertEquals(listOf("first"), b.receive()!!.received.map { it.text })
    }

    @Test fun `unread counts messages received on device keys until the thread is seen`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        a.send(chat, isGroup = false, contactUserId = bob, text = "one")
        assertEquals(0, b.unread(chat))
        b.receive()
        assertEquals(1, b.unread(chat))
        b.markRead(chat)
        assertEquals(0, b.unread(chat))
        a.send(chat, isGroup = false, contactUserId = bob, text = "two")
        b.receive()
        assertEquals(1, b.unread(chat))
        // Your own sends never count as unread.
        assertEquals(0, a.unread(chat))
    }

    @Test fun `choosing the older protocol stops device-key sends for that chat for good`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        assertTrue(a.send(chat, isGroup = false, contactUserId = bob, text = "on device keys") is SendOutcome.Sent)
        assertTrue(a.pinned(chat))
        a.useOlderProtocol(chat)
        assertTrue(!a.pinned(chat))
        // Even though Bob has device keys, this chat now goes the usual way.
        assertNull(a.send(chat, isGroup = false, contactUserId = bob, text = "usual way"))
        // It stays that way after a restart (the choice is on disk).
        val restarted = V3Chat(DeviceKeysSession(alice, ::legacy, DeviceStorage(File(folder.root, "a"), SoftwareKeyWrapper()), engine, server.api(alice)), alice, File(folder.root, "a-history"))
        assertNull(restarted.send(chat, isGroup = false, contactUserId = bob, text = "still the usual way"))
        // What was already exchanged on device keys is still there and still received.
        assertEquals(listOf("on device keys"), b.receive()!!.received.map { it.text })
    }

    @Test fun `a failed send can be retried with the same id or discarded`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        server.failNext = true
        val id = "11111111-2222-4333-8444-555555555555"
        runCatching { a.send(chat, isGroup = false, contactUserId = bob, text = "flaky", id = id) }
        assertEquals("flaky", a.failed(id)!!.text)
        a.send(chat, isGroup = false, contactUserId = bob, text = "flaky", id = id)
        assertNull(a.failed(id))
        assertEquals(listOf("flaky"), b.receive()!!.received.map { it.text })

        server.failNext = true
        val second = "22222222-3333-4444-8555-666666666666"
        runCatching { a.send(chat, isGroup = false, contactUserId = bob, text = "give up", id = second) }
        assertEquals("give up", a.failed(second)!!.text)
        a.discard(chat, second)
        assertNull(a.failed(second))
    }

    @Test fun `a phone that lost its keys knows its account uses device keys`() = runBlocking {
        val linked = account(alice, "a")
        // The account has a signed list on the server and this phone is a trusted device of it.
        assertTrue(!linked.accountUsesKeysWithoutThisPhone())

        // The same account on a phone with no device (cleared, reinstalled): the list exists, this phone isn't in it.
        val lost = V3Chat(DeviceKeysSession(alice, ::legacy, DeviceStorage(File(folder.root, "lost"), SoftwareKeyWrapper()), engine, server.api(alice)), alice, File(folder.root, "lost-history"))
        assertTrue(lost.accountUsesKeysWithoutThisPhone())
        assertNull(lost.send(chat, isGroup = false, contactUserId = bob, text = "x"))

        // An account that never used device keys isn't affected: chats stay on the usual way.
        val plain = V3Chat(DeviceKeysSession(bob, ::legacy, DeviceStorage(File(folder.root, "plain"), SoftwareKeyWrapper()), engine, server.api(bob)), bob, File(folder.root, "plain-history"))
        assertTrue(!plain.accountUsesKeysWithoutThisPhone())

        // Choosing the older protocol for a chat is remembered.
        lost.useOlderProtocol(chat)
        assertTrue(lost.chosenOlder(chat))
    }

    @Test fun `a failed check of the server is not remembered as protocol 3 being off`() = runBlocking {
        account(alice, "a")
        val lost = V3Chat(DeviceKeysSession(alice, ::legacy, DeviceStorage(File(folder.root, "lost2"), SoftwareKeyWrapper()), engine, server.api(alice)), alice, File(folder.root, "lost2-history"))
        server.failEnabledOnce = true
        // The first look fails (session not ready): no answer yet...
        assertTrue(!lost.accountUsesKeysWithoutThisPhone())
        // ...and the next one asks again and gets it right, instead of waiting out a cached "off".
        assertTrue(lost.accountUsesKeysWithoutThisPhone())
    }

    @Test fun `a contact without device keys goes the usual way until the chat is pinned`() = runBlocking {
        val a = account(alice, "a")
        assertTrue(a.send(chat, isGroup = false, contactUserId = bob, text = "no keys yet") is SendOutcome.Protocol1)
    }

    @Test fun `what only protocol 1 can carry is refused in a chat on device keys until the person chooses it`() = runBlocking {
        val a = account(alice, "a")
        account(bob, "b")
        // Not on device keys yet: nothing to refuse.
        for (what in OlderProtocolOnly.entries) assertNull(a.olderProtocolRefusal(chat, what))

        assertTrue(a.send(chat, isGroup = false, contactUserId = bob, text = "on device keys") is SendOutcome.Sent)
        for (what in OlderProtocolOnly.entries) {
            val refusal = a.olderProtocolRefusal(chat, what)!!
            assertTrue(refusal, refusal.endsWith("Nothing was sent."))
        }
        // Another chat isn't affected.
        assertNull(a.olderProtocolRefusal("11111111-1111-4111-8111-111111111111", OlderProtocolOnly.Reply))

        a.useOlderProtocol(chat)
        for (what in OlderProtocolOnly.entries) assertNull(a.olderProtocolRefusal(chat, what))
    }

    @Test fun `the refusal still stands when device keys can't be opened`() = runBlocking {
        val a = account(alice, "a")
        account(bob, "b")
        assertTrue(a.send(chat, isGroup = false, contactUserId = bob, text = "first") is SendOutcome.Sent)
        val restarted = V3Chat(DeviceKeysSession(alice, ::legacy, DeviceStorage(File(folder.root, "empty"), SoftwareKeyWrapper()), engine, server.api(alice)), alice, File(folder.root, "a-history"))
        assertTrue(restarted.olderProtocolRefusal(chat, OlderProtocolOnly.Poll) != null)
    }

    private val upload = com.redvaultsystems.redvaultmessage.data.model.FileDescriptor(
        "file", 1, "33333333-3333-4333-8333-333333333333", "c2VjcmV0LWZpbGUta2V5LXNlY3JldC1maWxlLWtleSE=", "notes.txt", "text/plain", 5, 46, "a".repeat(64),
        com.redvaultsystems.redvaultmessage.data.model.FileEncryptionInfo("xchacha20poly1305-secretstream", 1, "aGVhZGVyaGVhZGVyaGVhZGVyaGVhZGVy", 65536),
    )

    @Test fun `a file that went out keeps its upload`() = runBlocking {
        val a = account(alice, "a")
        account(bob, "b")
        val deleted = mutableListOf<String>()
        val outcome = a.sendUploaded(chat, false, bob, upload) { deleted += it }
        assertTrue(outcome is SendOutcome.Sent)
        assertTrue(deleted.isEmpty())
    }

    @Test fun `a file whose send failed after encrypting keeps its upload for Retry`() = runBlocking {
        val a = account(alice, "a")
        account(bob, "b")
        val deleted = mutableListOf<String>()
        server.failNext = true
        val failure = runCatching { a.sendUploaded(chat, false, bob, upload) { deleted += it } }
        assertTrue(failure.isFailure)
        assertTrue(deleted.isEmpty())
        assertEquals(1, a.records(chat).count { it.status == "failed" && it.file?.objectId == upload.objectId })
    }

    @Test fun `a refusal before anything was sent deletes the upload and reports the refusal`() = runBlocking {
        val a = account(alice, "a")
        account(bob, "b")
        assertTrue(a.send(chat, isGroup = false, contactUserId = bob, text = "on device keys") is SendOutcome.Sent)
        server.dropDeviceList(bob)

        val deleted = mutableListOf<String>()
        val failure = runCatching { a.sendUploaded(chat, false, bob, upload) { deleted += it } }.exceptionOrNull()
        assertEquals("contact_has_no_devices", (failure as EngineException).code)
        assertEquals(listOf(upload.objectId), deleted)
        assertTrue(a.records(chat).none { it.file != null })
    }

    @Test fun `a failed delete never replaces the reason the file didn't go out`() = runBlocking {
        val a = account(alice, "a")
        account(bob, "b")
        assertTrue(a.send(chat, isGroup = false, contactUserId = bob, text = "on device keys") is SendOutcome.Sent)
        server.dropDeviceList(bob)
        val failure = runCatching { a.sendUploaded(chat, false, bob, upload) { throw IllegalStateException("offline") } }.exceptionOrNull()
        assertEquals("contact_has_no_devices", (failure as EngineException).code)
    }

    @Test fun `a file that turns out not to be on device keys doesn't leave its upload behind`() = runBlocking {
        val a = account(alice, "a")
        val deleted = mutableListOf<String>()
        assertTrue(a.sendUploaded(chat, false, bob, upload) { deleted += it } is SendOutcome.Protocol1)
        assertEquals(listOf(upload.objectId), deleted)

        deleted.clear()
        a.useOlderProtocol(chat)
        assertNull(a.sendUploaded(chat, false, bob, upload) { deleted += it })
        assertEquals(listOf(upload.objectId), deleted)
    }
}
