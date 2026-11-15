// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.calls.OLDER_WAY_REFUSED
import com.redvaultsystems.redvaultmessage.calls.V3CallCarrier
import com.redvaultsystems.redvaultmessage.calls.refuseOlderOffer
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom

/**
 * Part B over device keys end to end (docs/signal-integration/09-part-b-calls.md): a one-to-one call's offer and
 * answer, and a group call's room keys, through V3Chat with the real engine and the in-memory server, then the checks
 * the call managers make before using them (refuseCall, refuseCallKey).
 */
class V3CallTest {
    @get:Rule val folder = TemporaryFolder()

    private lateinit var engine: EngineRunner
    private val server = FakeV3Server()
    private val alice = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val bob = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    private val carol = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
    private val chat = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"
    private val group = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"
    private val callId = "f0f0f0f0-0000-4000-8000-000000000001"

    @Before fun load() {
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't built for this computer", call != null)
        engine = EngineRunner(call!!)
    }

    private fun legacy() = ByteArray(32).also { SecureRandom().nextBytes(it) }

    private class Phone(val chat: V3Chat, val keys: DeviceKeysSession)

    private fun phone(user: String, name: String, storage: DeviceStorage, api: V3Api = server.api(user)): Phone {
        val keys = DeviceKeysSession(user, ::legacy, storage, engine, api)
        return Phone(V3Chat(keys, user, File(folder.root, "$name-history")), keys)
    }

    private suspend fun account(user: String, name: String): Phone {
        val storage = DeviceStorage(File(folder.root, name), SoftwareKeyWrapper())
        setUpFirstDevice(user, legacy(), storage, engine, server.api(user))
        return phone(user, name, storage)
    }

    /** Two phones of one account: the second restored with the first one's recovery key (it's then in the signed list). */
    private suspend fun twoPhones(user: String, name: String): Pair<Phone, Phone> {
        val firstStorage = DeviceStorage(File(folder.root, "$name-1"), SoftwareKeyWrapper())
        val first = setUpFirstDevice(user, legacy(), firstStorage, engine, server.api(user))
        val secondStorage = DeviceStorage(File(folder.root, "$name-2"), SoftwareKeyWrapper())
        restoreWithRecoveryKey(user, createRecoveryBackup(first), secondStorage, engine, server.api(user))
        return phone(user, "$name-1", firstStorage) to phone(user, "$name-2", secondStorage)
    }

    private fun offer(to: String = CallMessage.EVERY_DEVICE) = CallMessage(callId, CallMessage.OFFER, "video", "v=0\r\na=fingerprint:sha-256 AA\r\n", to)

    private fun Phone.takeCall(step: String) = chat.callEnvelopes.take { it.callId == callId && it.call?.step == step }

    @Test fun `a call's offer reaches both of the callee's phones, one answers, and only that phone counts`() = runBlocking {
        val a = account(alice, "a")
        val (b1, b2) = twoPhones(bob, "b")
        assertEquals(true, a.chat.callsOnDeviceKeys(chat, bob))

        a.chat.sendCall(chat, bob, offer())
        val now = System.currentTimeMillis()
        val expectOffer = ExpectedCallMessage(callId, chat, alice, CallMessage.OFFER, "video")
        val offers = listOf(b1, b2).map { phone ->
            val report = phone.chat.receive()!!
            // Ephemeral: acknowledged, never a chat message.
            assertTrue(report.received.isEmpty())
            assertTrue(phone.chat.records(chat).isEmpty())
            val got = phone.takeCall(CallMessage.OFFER)
            assertNotNull(got)
            assertNull(refuseCall(got!!, expectOffer, now))
            got
        }
        val aliceDevice = offers[0].senderDeviceId
        assertEquals(aliceDevice, offers[1].senderDeviceId)
        val b1Device = offers[0].forDevice
        assertNotEquals(b1Device, offers[1].forDevice)
        // Nothing went to the caller's own other devices, and nothing is left in anyone's mailbox.
        assertTrue(a.chat.receive()!!.calls.isEmpty())
        assertTrue(b1.chat.receive()!!.calls.isEmpty())

        // The phone that answers sends it to the offering device only.
        b1.chat.sendCall(chat, alice, CallMessage(callId, CallMessage.ANSWER, "video", "v=0\r\nanswer\r\n", aliceDevice), aliceDevice)
        assertTrue(b2.chat.receive()!!.calls.isEmpty())
        a.chat.receive()
        val answer = a.takeCall(CallMessage.ANSWER)!!
        val expectAnswer = ExpectedCallMessage(callId, chat, bob, CallMessage.ANSWER, "video")
        assertNull(refuseCall(answer, expectAnswer, System.currentTimeMillis()))
        assertEquals(b1Device, answer.senderDeviceId)
        assertEquals("v=0\r\nanswer\r\n", answer.call!!.sdp)

        // The other phone stops: once the call is with b1, b2's late answer (or restart) isn't taken.
        b2.chat.sendCall(chat, alice, CallMessage(callId, CallMessage.ANSWER, "video", "v=0\r\nlate\r\n", aliceDevice), aliceDevice)
        a.chat.receive()
        val late = a.takeCall(CallMessage.ANSWER)!!
        assertEquals("wrong_device", refuseCall(late, expectAnswer.copy(peerDeviceId = answer.senderDeviceId), System.currentTimeMillis()))

        // A restart goes the same way, to the one device the call is with.
        a.chat.sendCall(chat, bob, CallMessage(callId, CallMessage.RESTART_OFFER, "video", "v=0\r\nrestart\r\n", b1Device), b1Device)
        assertTrue(b2.chat.receive()!!.calls.isEmpty())
        b1.chat.receive()
        val restart = b1.takeCall(CallMessage.RESTART_OFFER)!!
        assertNull(refuseCall(restart, ExpectedCallMessage(callId, chat, alice, CallMessage.RESTART_OFFER, "video", aliceDevice), System.currentTimeMillis()))
    }

    @Test fun `a stale offer is refused, and so is one meant for another device`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        val device = a.keys.chatDevice()!!
        sendCallMessage(device, chat, bob, offer(), now = System.currentTimeMillis() - CallLimits.MAX_AGE_MS - 1_000)
        val report = b.chat.receive()!!
        assertTrue(report.calls.isEmpty())
        assertTrue("stale_call" in report.dropped)
        assertNull(b.takeCall(CallMessage.OFFER))

        a.chat.sendCall(chat, bob, offer(to = "99999999-0000-4000-8000-000000000001"))
        val other = b.chat.receive()!!
        assertTrue("call_not_for_this_device" in other.dropped)
        assertNull(b.takeCall(CallMessage.OFFER))
        assertTrue(b.chat.records(chat).isEmpty())
    }

    @Test fun `an offer from someone who isn't the caller is refused`() = runBlocking {
        account(alice, "a")
        val b = account(bob, "b")
        val c = account(carol, "c")
        // Carol knows the call id and sends her own offer for it in Alice and Bob's chat.
        c.chat.sendCall(chat, bob, offer())
        b.chat.receive()
        val forged = b.takeCall(CallMessage.OFFER)!!
        assertEquals("wrong_sender", refuseCall(forged, ExpectedCallMessage(callId, chat, alice, CallMessage.OFFER, "video"), System.currentTimeMillis()))
        // And for a call the live events never named, nothing is waiting to be taken.
        assertEquals("unknown_call", refuseCall(forged, ExpectedCallMessage("f0f0f0f0-0000-4000-8000-000000000002", chat, carol, CallMessage.OFFER, "video"), System.currentTimeMillis()))
    }

    @Test fun `a group call's room key reaches the participants, not from a non-member, and never at a lower epoch`() = runBlocking {
        // Carol is still known to the server here, but no longer in the group's member list on Bob's phone.
        server.participants[group] = listOf(alice, bob, carol)
        val a = account(alice, "a")
        val b = account(bob, "b")
        val c = account(carol, "c")
        assertEquals(true, a.chat.groupCallsOnDeviceKeys(group))
        val members = setOf(alice, bob)

        a.chat.sendCallKey(group, listOf(alice, bob), CallKeyMessage(callId, 3, ByteArray(32) { 3 }))
        c.chat.sendCallKey(group, listOf(bob), CallKeyMessage(callId, 4, ByteArray(32) { 4 }))
        // Pairwise, to the participants named: Carol's phone got nothing from Alice.
        assertTrue(c.chat.receive()!!.calls.isEmpty())
        val report = b.chat.receive()!!
        assertTrue(report.received.isEmpty())
        assertTrue(b.chat.records(group).isEmpty())
        val keys = b.chat.callEnvelopes.takeAll { it.key != null }
        assertEquals(2, keys.size)
        val fromAlice = keys.single { it.senderUserId == alice }
        val fromCarol = keys.single { it.senderUserId == carol }
        assertNull(refuseCallKey(fromAlice, callId, group, members, heldEpoch = 0))
        assertTrue(fromAlice.key!!.key.all { it == 3.toByte() })
        assertEquals("not_a_member", refuseCallKey(fromCarol, callId, group, members, heldEpoch = 0))

        // Bob now holds epoch 3: a key for epoch 2 is refused, even from Alice.
        a.chat.sendCallKey(group, listOf(bob), CallKeyMessage(callId, 2, ByteArray(32) { 2 }))
        b.chat.receive()
        val older = b.chat.callEnvelopes.take { it.key != null }!!
        assertEquals(2L, older.key!!.epoch)
        assertEquals("old_epoch", refuseCallKey(older, callId, group, members, heldEpoch = 3))
    }

    @Test fun `in a chat on device keys a protocol-1 offer is refused, elsewhere it still rings the usual way`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        val carrier = V3CallCarrier(b.chat) {}
        // Before anything went over device keys the chat isn't on them: an older-way offer goes on as protocol 1.
        assertFalse(carrier.chatOnDeviceKeys(chat))
        assertFalse(refuseOlderOffer(markedForDeviceKeys = false, chatOnDeviceKeys = carrier.chatOnDeviceKeys(chat)))

        // A message on device keys puts the chat on them for good on Bob's phone.
        a.chat.send(chat, false, bob, "hi")
        b.chat.receive()
        assertTrue(carrier.chatOnDeviceKeys(chat))
        // A sealed offer (even one a forger made with the legacy key) is refused; the device-key marker isn't.
        assertTrue(refuseOlderOffer(markedForDeviceKeys = false, chatOnDeviceKeys = carrier.chatOnDeviceKeys(chat)))
        assertFalse(refuseOlderOffer(markedForDeviceKeys = true, chatOnDeviceKeys = carrier.chatOnDeviceKeys(chat)))
        assertEquals("This call came the older way and was refused", OLDER_WAY_REFUSED)
        // Another chat isn't affected.
        assertFalse(carrier.chatOnDeviceKeys("dddddddd-dddd-4ddd-8ddd-000000000002"))
    }

    /** The fake server's API with Tor "down" for the posts (as in V3ChatPartATest). */
    private class TorDropsPosts(private val inner: V3Api) : V3Api by inner {
        override suspend fun sendMessages(senderDeviceId: String, conversationId: String?, messages: List<Triple<String, String, String>>) {
            throw ApiException(0, "tor_unavailable", "Tor connection unavailable")
        }
    }

    @Test fun `with Tor down a call message fails at once and is never queued`() = runBlocking {
        account(bob, "b")
        val storage = DeviceStorage(File(folder.root, "a"), SoftwareKeyWrapper())
        setUpFirstDevice(alice, legacy(), storage, engine, server.api(alice))
        val queue = V3SendQueue(File(folder.root, "a-queue"), alice, SoftwareKeyWrapper())
        val outgoing = QueueingV3Api(TorDropsPosts(server.api(alice)), queue)
        val keys = DeviceKeysSession(alice, ::legacy, storage, engine, outgoing)
        val a = V3Chat(keys, alice, File(folder.root, "a-history"), outgoing)

        val error = runCatching { a.sendCall(chat, bob, offer()) }.exceptionOrNull()
        assertTrue(error is ApiException && error.status == 0)
        assertTrue(queue.isEmpty())
        assertEquals(0, a.queuedMessages())
    }
}
