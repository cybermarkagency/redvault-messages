// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The part of opening a room key that doesn't need libsodium, run against a payload the web client's own
 * sealRoomKey produced (ui/src/lib/groupCallCrypto.ts, opened with its own libsodium). The sealing itself
 * is pinned by androidTest/.../GroupCallCryptoCompatTest.kt, which needs a device.
 */
class GroupCallKeyChecksTest {
    /** What the web sealed, once opened. */
    private val webPayload =
        "{\"v\":1,\"type\":\"group-call-key\",\"callId\":\"6f1d2c3e-4b5a-4c6d-8e7f-0a1b2c3d4e5f\"," +
            "\"conversationId\":\"9a8b7c6d-5e4f-4a3b-8c2d-1e0f9a8b7c6d\",\"epoch\":19," +
            "\"from\":\"11111111-1111-4111-8111-111111111111\",\"to\":\"22222222-2222-4222-8222-222222222222\"," +
            "\"key\":\"AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dw=\",\"sentAt\":1792150906279}"
    private val webKey = "AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dw="
    private val sentAt = 1792150906279L
    private val roomKey = Base64Codec.decode(webKey)
    private val expected = ExpectedRoomKey(
        callId = "6f1d2c3e-4b5a-4c6d-8e7f-0a1b2c3d4e5f",
        conversationId = "9a8b7c6d-5e4f-4a3b-8c2d-1e0f9a8b7c6d",
        epoch = 19,
        from = "11111111-1111-4111-8111-111111111111",
        to = "22222222-2222-4222-8222-222222222222",
    )

    private fun check(payload: String = webPayload, expected: ExpectedRoomKey = this.expected, now: Long = sentAt) =
        GroupCallCrypto.checkRoomKey(payload, expected, now)

    private fun refusal(block: () -> Unit) = assertThrows(GroupKeyException::class.java) { block() }.message.orEmpty()

    @Test
    fun acceptsTheKeyTheWebSealed() {
        assertArrayEquals(roomKey, check())
        assertEquals(32, check().size)
    }

    @Test
    fun refusesAKeyReplayedIntoAnotherEpochCallGroupOrMeantForSomeoneElse() {
        for (other in listOf(
            ExpectedRoomKey(expected.callId, expected.conversationId, 20, expected.from, expected.to),
            ExpectedRoomKey("other-call", expected.conversationId, expected.epoch, expected.from, expected.to),
            ExpectedRoomKey(expected.callId, "other-group", expected.epoch, expected.from, expected.to),
            ExpectedRoomKey(expected.callId, expected.conversationId, expected.epoch, expected.from, "carol"),
            ExpectedRoomKey(expected.callId, expected.conversationId, expected.epoch, "mallory", expected.to),
        )) {
            assertTrue(refusal { check(expected = other) }.contains("didn't match"))
        }
    }

    @Test
    fun refusesAWrongVersionOrType() {
        assertTrue(refusal { check(webPayload.replace("\"v\":1", "\"v\":2")) }.contains("didn't match"))
        assertTrue(refusal { check(webPayload.replace("group-call-key", "something-else")) }.contains("didn't match"))
    }

    @Test
    fun refusesAStaleKeyOrOneFromTheFuture() {
        val fiveMinutes = 5 * 60 * 1000L
        assertArrayEquals(roomKey, check(now = sentAt + fiveMinutes))
        assertTrue(refusal { check(now = sentAt + fiveMinutes + 1) }.contains("too old"))
        assertTrue(refusal { check(now = sentAt - fiveMinutes - 1) }.contains("too old"))
    }

    @Test
    fun refusesAKeyThatIsntThirtyTwoBytes() {
        val short = Base64Codec.encode(ByteArray(31))
        assertTrue(refusal { check(webPayload.replace(webKey, short)) }.contains("wrong length"))
        assertTrue(refusal { check(webPayload.replace(webKey, "***not base64***")) }.contains("wrong length"))
    }

    @Test
    fun refusesAPayloadThatIsntAKeyMessage() {
        assertTrue(refusal { check("not json") }.contains("couldn't be verified"))
        assertTrue(refusal { check("{}") }.contains("couldn't be verified"))
        // A number where the web sends a string.
        assertTrue(refusal { check(webPayload.replace("\"callId\":\"6f1d2c3e-4b5a-4c6d-8e7f-0a1b2c3d4e5f\"", "\"callId\":5")) }.contains("couldn't be verified"))
    }

    @Test
    fun anEpochsKeyGoesInTheSlotEpochModSixteen() {
        assertEquals(0, GroupCallCrypto.keySlot(16))
        assertEquals(3, GroupCallCrypto.keySlot(19))
        assertEquals(15, GroupCallCrypto.keySlot(31))
        assertEquals(1, GroupCallCrypto.keySlot(1))
    }
}
