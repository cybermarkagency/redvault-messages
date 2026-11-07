package com.redvaultsystems.redvaultmessage.crypto

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.goterl.lazysodium.LazySodiumAndroid
import com.goterl.lazysodium.SodiumAndroid
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith

/** A huge call setup from a contact is refused before it rings, as on the desktop (2026-11-04 assessment, F-36). */
@RunWith(AndroidJUnit4::class)
class CallSetupLimitsTest {
    private val sodium = LazySodiumAndroid(SodiumAndroid())
    private val e2e = E2eCrypto(sodium)
    private val crypto = CallCrypto(sodium, e2e)
    private val alice = e2e.generateKeyPair()
    private val bob = e2e.generateKeyPair()
    private val expected = CallCrypto.Expected("offer", "call-1", "conv-1", "alice", "bob")

    private fun offer(sdp: String) = CallSignal(
        type = "offer", callId = "call-1", conversationId = "conv-1", from = "alice", to = "bob", kind = "voice",
        sdp = sdp, sentAt = System.currentTimeMillis(),
    )

    @Test
    fun aNormalSetupStillOpens() {
        val sdp = "v=0\r\na=fingerprint:sha-256 AB:CD:EF\r\n"
        val opened = crypto.open(crypto.seal(offer(sdp), bob.publicKey, alice.privateKey), alice.publicKey, bob.privateKey, expected)
        assertEquals(sdp, opened.sdp)
    }

    @Test
    fun aHugeSetupIsRefused() {
        val sdp = "v=0\r\na=fingerprint:sha-256 " + "F".repeat(60_000) + "\r\n"
        val sealed = crypto.seal(offer(sdp), bob.publicKey, alice.privateKey)
        assertThrows(CallSignalException::class.java) { crypto.open(sealed, alice.publicKey, bob.privateKey, expected) }
    }

    @Test
    fun theFingerprintPatternCopesWithAVeryLongLine() {
        // java.util.regex runs this pattern without recursion per character; checked so a regression would show.
        val line = "a=fingerprint:sha-256 " + "F".repeat(1_000_000)
        assertEquals(1_000_000 + 8, CallCrypto.sdpFingerprint(line)!!.length)
    }
}
