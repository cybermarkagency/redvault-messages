package com.redvaultsystems.redvaultmessage.calls

import com.redvaultsystems.redvaultmessage.data.api.IceServerDto
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** TURN credential renewal for long one-to-one calls (calls/TurnRenewal.kt), the same rules as the web app. */
class TurnRenewalTest {
    @Test
    fun theExpiryComesFromTheTurnRestUsernameEarliestFirst() {
        assertEquals(1792500000L * 1000, TurnRenewal.expiryMs(listOf(IceServerDto(listOf("turn:x"), "1792500000:ab12", "c"))))
        assertEquals(
            1792500000L * 1000,
            TurnRenewal.expiryMs(listOf(IceServerDto(listOf("turn:x"), "1792500300:a", "c"), IceServerDto(listOf("turn:y"), "1792500000:b", "c"))),
        )
        assertNull(TurnRenewal.expiryMs(listOf(IceServerDto(listOf("turn:x"), "static-user", "c"))))
    }

    @Test
    fun renewalIsAt80PercentOfWhatsLeftAndNeverWithinAMinute() {
        val now = 1_000_000L
        assertEquals((24 * 3_600_000L * 0.8).toLong(), TurnRenewal.delayMs(now + 24 * 3_600_000L, now))
        assertEquals(60_000L, TurnRenewal.delayMs(now + 30_000, now))
        assertEquals(60_000L, TurnRenewal.delayMs(now - 5_000, now))
    }
}
