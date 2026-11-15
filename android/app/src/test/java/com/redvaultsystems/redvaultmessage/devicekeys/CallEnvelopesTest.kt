// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.coroutines.async
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.yield
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * Part B's envelopes (docs/signal-integration/09-part-b-calls.md): the shared decode vectors both apps run, the
 * round trip, and the context checks each app makes itself (which call, who sent it, for which device, how old).
 */
class CallEnvelopesTest {
    private val vectors = Json.parseToJsonElement(File("../../desktop/tests/vectors/v3_call_vectors.json").readText()).jsonObject
    private val conversation = vectors.getValue("conversation").jsonPrimitive.content
    private val now = vectors.getValue("now").jsonPrimitive.long

    private val callId = "cccccccc-0000-4000-8000-000000000001"
    private val caller = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val callerDevice = "aaaaaaaa-0000-4000-8000-000000000001"
    private val thisDevice = "dddddddd-0000-4000-8000-000000000001"
    private val otherDevice = "dddddddd-0000-4000-8000-000000000002"

    private fun JsonObject.text(name: String) = getValue(name).jsonPrimitive.content

    @Test fun `every call decode case is accepted or ignored as the vectors say`() {
        val cases = vectors.getValue("decode").jsonArray.map { it.jsonObject }
        assertEquals(16, cases.size)
        for (c in cases) {
            val kind = when (decodeEnvelope(c.getValue("envelope").toString(), conversation, now)) {
                is DecodedEnvelope.Call -> "call"
                is DecodedEnvelope.CallKey -> "callkey"
                is DecodedEnvelope.StoryKey -> "storykey"
                is DecodedEnvelope.StoryAnswer -> "storyanswer"
                is DecodedEnvelope.Ignore -> "ignore"
                is DecodedEnvelope.Op -> "op"
                is DecodedEnvelope.Shown -> "message"
                DecodedEnvelope.Newer -> "newer"
            }
            assertEquals(c.text("name"), c.text("accept"), kind)
        }
    }

    @Test fun `a call and a room key round-trip through the envelope`() {
        val call = CallMessage(callId, CallMessage.RESTART_ANSWER, "voice", "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\n", thisDevice)
        val decoded = decodeEnvelope(encodeCall(conversation, call, "bbbbbbbb-0000-4000-8000-000000000009", now - 1000), conversation, now)
        assertEquals(DecodedEnvelope.Call("bbbbbbbb-0000-4000-8000-000000000009", now - 1000, call), decoded)

        val key = CallKeyMessage(callId, 7, ByteArray(32) { it.toByte() })
        val opened = decodeEnvelope(encodeCallKey(conversation, key, "bbbbbbbb-0000-4000-8000-00000000000a", now), conversation, now)
        assertEquals(DecodedEnvelope.CallKey("bbbbbbbb-0000-4000-8000-00000000000a", now, key), opened)
        // Padded like every envelope, and never printable with its SDP or key.
        assertEquals(0, encodeCallKey(conversation, key, "bbbbbbbb-0000-4000-8000-00000000000a", now).toByteArray().size % 256)
        assertFalse(call.toString().contains("v=0"))
        assertFalse(key.toString().contains("AAECAwQF"))
    }

    private fun envelope(fields: kotlinx.serialization.json.JsonObjectBuilder.() -> Unit) = buildJsonObject {
        put("v", 1)
        put("t", "call")
        put("id", "bbbbbbbb-0000-4000-8000-000000000001")
        put("c", conversation)
        put("ts", now)
        put("callId", callId)
        put("step", "offer")
        put("kind", "voice")
        put("sdp", "v=0\r\n")
        put("to", "*")
        fields()
    }.toString()

    @Test fun `a missing or null required field, a missing time, or another chat makes a call envelope ignored`() {
        for (field in listOf("callId", "step", "kind", "sdp", "to", "ts")) {
            val without = Json.parseToJsonElement(envelope {}).jsonObject.let { JsonObject(it - field) }.toString()
            assertTrue(field, decodeEnvelope(without, conversation, now) is DecodedEnvelope.Ignore)
            val nulled = Json.parseToJsonElement(envelope {}).jsonObject.let { JsonObject(it + (field to kotlinx.serialization.json.JsonNull)) }.toString()
            assertTrue(field, decodeEnvelope(nulled, conversation, now) is DecodedEnvelope.Ignore)
        }
        assertTrue(decodeEnvelope(envelope { put("ts", JsonPrimitive(now.toDouble())) }, conversation, now) is DecodedEnvelope.Ignore)
        assertTrue(decodeEnvelope(envelope {}, "11111111-1111-4111-8111-111111111112", now) is DecodedEnvelope.Ignore)
        // A clock far ahead isn't taken at its word either.
        assertTrue(decodeEnvelope(envelope { put("ts", now + 10 * 60_000) }, conversation, now) is DecodedEnvelope.Ignore)
        // A device id is a lowercase uuid; an unknown extra field changes nothing.
        assertTrue(decodeEnvelope(envelope { put("to", thisDevice.uppercase()) }, conversation, now) is DecodedEnvelope.Ignore)
        assertTrue(decodeEnvelope(envelope { put("extra", true) }, conversation, now) is DecodedEnvelope.Call)
        // The SDP limit counts UTF-16 units: 49,152 is fine.
        assertTrue(decodeEnvelope(envelope { put("sdp", "x".repeat(CallLimits.MAX_SDP_UNITS)) }, conversation, now) is DecodedEnvelope.Call)
    }

    private fun incoming(call: CallMessage, from: String = caller, fromDevice: String = callerDevice, ts: Long = now, chat: String = conversation) =
        IncomingCallEnvelope(from, fromDevice, chat, thisDevice, ts, call = call)

    @Test fun `a call message is used only for its call, step, chat, sender, device, kind and time`() {
        val offer = CallMessage(callId, CallMessage.OFFER, "video", "v=0\r\n", CallMessage.EVERY_DEVICE)
        val expected = ExpectedCallMessage(callId, conversation, caller, CallMessage.OFFER, "video")
        assertNull(refuseCall(incoming(offer), expected, now))
        assertEquals("unknown_call", refuseCall(incoming(offer), expected.copy(callId = "cccccccc-0000-4000-8000-000000000002"), now))
        assertEquals("wrong_step", refuseCall(incoming(offer), expected.copy(step = CallMessage.ANSWER), now))
        assertEquals("wrong_conversation", refuseCall(incoming(offer, chat = "11111111-1111-4111-8111-111111111112"), expected, now))
        assertEquals("wrong_sender", refuseCall(incoming(offer, from = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"), expected, now))
        assertEquals("wrong_device", refuseCall(incoming(offer, fromDevice = otherDevice), expected.copy(peerDeviceId = callerDevice), now))
        assertEquals("not_for_this_device", refuseCall(incoming(CallMessage(callId, CallMessage.OFFER, "video", "v=0\r\n", otherDevice)), expected, now))
        assertNull(refuseCall(incoming(CallMessage(callId, CallMessage.OFFER, "video", "v=0\r\n", thisDevice)), expected, now))
        assertEquals("wrong_kind", refuseCall(incoming(offer), expected.copy(kind = "voice"), now))
        assertNull(refuseCall(incoming(offer, ts = now - 120_000), expected, now))
        assertEquals("stale_call", refuseCall(incoming(offer, ts = now - 120_001), expected, now))
    }

    @Test fun `a room key is used only from a member, for its call and group, at an epoch at least the newest held`() {
        val group = conversation
        fun key(epoch: Long, from: String = caller) =
            IncomingCallEnvelope(from, callerDevice, group, thisDevice, now, key = CallKeyMessage(callId, epoch, ByteArray(32)))
        val members = setOf(caller, "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")
        assertNull(refuseCallKey(key(2), callId, group, members, heldEpoch = 0))
        assertNull(refuseCallKey(key(2), callId, group, members, heldEpoch = 2))
        assertEquals("old_epoch", refuseCallKey(key(1), callId, group, members, heldEpoch = 2))
        assertEquals("not_a_member", refuseCallKey(key(3, from = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"), callId, group, members, 0))
        assertEquals("unknown_call", refuseCallKey(key(3), "cccccccc-0000-4000-8000-000000000002", group, members, 0))
        assertEquals("wrong_conversation", refuseCallKey(key(3), callId, "11111111-1111-4111-8111-111111111112", members, 0))
    }

    @Test fun `the hub holds call messages until their call takes them, and drops old ones`() = runBlocking {
        var clock = now
        val hub = CallEnvelopeHub { clock }
        val offer = CallMessage(callId, CallMessage.OFFER, "voice", "v=0\r\n", "*")
        // Delivered before the live event that names the call: still there when the call asks.
        hub.offer(incoming(offer))
        assertNotNull(hub.await(10) { it.callId == callId })
        assertNull(hub.take { it.callId == callId })
        // Arriving while the call waits.
        val waiting = async { hub.await(5_000) { it.call?.step == CallMessage.OFFER } }
        yield()
        hub.offer(incoming(offer))
        assertNotNull(waiting.await())
        // Never claimed and too old: gone, its key wiped.
        val key = CallKeyMessage(callId, 1, ByteArray(32) { 1 })
        hub.offer(IncomingCallEnvelope(caller, callerDevice, conversation, thisDevice, clock, key = key))
        clock += CallLimits.MAX_AGE_MS + 1
        assertTrue(hub.takeAll { true }.isEmpty())
        assertTrue(key.key.all { it == 0.toByte() })
        assertNull(hub.await(10) { true })
    }
}
