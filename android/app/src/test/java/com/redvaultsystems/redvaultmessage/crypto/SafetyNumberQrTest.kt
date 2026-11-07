package com.redvaultsystems.redvaultmessage.crypto

import com.redvaultsystems.redvaultmessage.crypto.SafetyNumberQr.Result
import org.junit.Assert.assertEquals
import org.junit.Test

class SafetyNumberQrTest {
    private val mine = "793202196896733342307744792420430796626930648824555351264617".chunked(5)

    @Test
    fun theirCodeForTheSameKeysMatches() {
        assertEquals(Result.Match, SafetyNumberQr.compare(SafetyNumberQr.encode(mine), mine))
    }

    @Test
    fun aDifferentNumberIsAMismatch() {
        val other = "093202196896733342307744792420430796626930648824555351264617".chunked(5)
        assertEquals(Result.Mismatch, SafetyNumberQr.compare(SafetyNumberQr.encode(other), mine))
    }

    @Test
    fun otherQrCodesAreRejected() {
        assertEquals(Result.NotASafetyCode, SafetyNumberQr.compare("https://example.com", mine))
        assertEquals(Result.NotASafetyCode, SafetyNumberQr.compare("redvault-safety:v1:123", mine))
        assertEquals(Result.NotASafetyCode, SafetyNumberQr.compare("redvault-safety:v1:" + "x".repeat(60), mine))
    }
}
