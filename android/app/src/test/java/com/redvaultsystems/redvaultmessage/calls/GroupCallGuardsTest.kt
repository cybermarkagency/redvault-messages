package com.redvaultsystems.redvaultmessage.calls

import io.livekit.android.e2ee.E2EEState
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GroupCallGuardsTest {
    /** Stands in for a published track: records whether it was ever live without a cryptor on. */
    private class Sender(override var enabled: Boolean, private val failAttach: Boolean = false) : RekeyableSender {
        private var attached = true
        var leakedUnencrypted = false
        val enabledAtReattach = mutableListOf<Boolean>()

        override fun reattach() {
            enabledAtReattach += enabled
            attached = false // The SDK's remove: the cryptor is off from here on, and a disabled one forwards frames in the clear.
            if (failAttach) throw IllegalArgumentException("rtpSender is null")
            attached = true
        }

        fun tick() {
            if (enabled && !attached) leakedUnencrypted = true
        }
    }

    private fun move(vararg senders: Sender, settle: suspend () -> Unit = {}): Boolean = runBlocking {
        moveToNewKey(senders.toList(), settle).also { senders.forEach(Sender::tick) }
    }

    @Test
    fun aMovedTrackComesBackAsItWas() {
        val on = Sender(enabled = true)
        val muted = Sender(enabled = false)
        assertTrue(move(on, muted))
        assertTrue(on.enabled)
        assertFalse(muted.enabled)
    }

    @Test
    fun everyTrackIsMutedBeforeItsCryptorComesOff() {
        val a = Sender(enabled = true)
        val b = Sender(enabled = true)
        assertTrue(move(a, b))
        assertEquals(listOf(false), a.enabledAtReattach)
        assertEquals(listOf(false), b.enabledAtReattach)
    }

    @Test
    fun theEncoderGetsTimeToEmptyWhileTheTrackIsMutedAndTheOldCryptorStillOn() {
        val sender = Sender(enabled = true)
        var mutedDuringSettle = false
        var reattachedBeforeSettle = false
        assertTrue(
            move(sender) {
                mutedDuringSettle = !sender.enabled
                reattachedBeforeSettle = sender.enabledAtReattach.isNotEmpty()
            },
        )
        assertTrue(mutedDuringSettle)
        assertFalse(reattachedBeforeSettle)
    }

    @Test
    fun aFailedReattachLeavesTheTrackDisabledSoNoFrameLeavesWithoutACryptor() {
        val sender = Sender(enabled = true, failAttach = true)
        assertFalse(move(sender))
        assertFalse(sender.enabled)
        assertFalse(sender.leakedUnencrypted)
    }

    @Test
    fun oneFailureLeavesEveryTrackDisabled() {
        val first = Sender(enabled = true)
        val failing = Sender(enabled = true, failAttach = true)
        val last = Sender(enabled = true)
        assertFalse(move(first, failing, last))
        assertFalse(first.enabled)
        assertFalse(failing.enabled)
        assertFalse(last.enabled)
    }

    @Test
    fun nothingPublishedNeedsNoPause() {
        var settled = false
        assertTrue(runBlocking { moveToNewKey(emptyList()) { settled = true } })
        assertFalse(settled)
    }

    @Test
    fun aKeyThatWontInstallStopsEverySenderSoNothingGoesOutOnTheOldKey() {
        val mic = Sender(enabled = true)
        val camera = Sender(enabled = true)
        assertFalse(installOrSilence({ false }, listOf(mic, camera)))
        assertFalse(mic.enabled)
        assertFalse(camera.enabled)
    }

    @Test
    fun anInstallThatThrowsCountsAsFailed() {
        val mic = Sender(enabled = true)
        assertFalse(installOrSilence({ throw IllegalStateException("native key provider gone") }, listOf(mic)))
        assertFalse(mic.enabled)
    }

    @Test
    fun oneSenderThatCantBeStoppedDoesntLeaveTheOthersRunning() {
        val stubborn = object : RekeyableSender {
            override var enabled: Boolean
                get() = true
                set(_) = throw IllegalStateException("track disposed")

            override fun reattach() = Unit
        }
        val camera = Sender(enabled = true)
        assertFalse(installOrSilence({ false }, listOf(stubborn, camera)))
        assertFalse(camera.enabled)
    }

    @Test
    fun aKeyThatWontInstallBeforeAnythingIsPublishedStillFails() {
        assertFalse(installOrSilence({ false }, emptyList()))
    }

    @Test
    fun controlAnInstalledKeyLeavesTheSendersAlone() {
        val mic = Sender(enabled = true)
        val muted = Sender(enabled = false)
        assertTrue(installOrSilence({ true }, listOf(mic, muted)))
        assertTrue(mic.enabled)
        assertFalse(muted.enabled)
    }

    @Test
    fun publishingNeedsE2eeOnAndAKey() {
        assertTrue(mayPublish(e2eeEnabled = true, hasKey = true, keyEpoch = 3))
        assertFalse(mayPublish(e2eeEnabled = null, hasKey = true, keyEpoch = 3))
        assertFalse(mayPublish(e2eeEnabled = false, hasKey = true, keyEpoch = 3))
        assertFalse(mayPublish(e2eeEnabled = true, hasKey = false, keyEpoch = 3))
        assertFalse(mayPublish(e2eeEnabled = true, hasKey = true, keyEpoch = 0))
    }

    @Test
    fun onlyRealFailuresCount() {
        for (state in E2EEState.entries) {
            val expected = state in setOf(E2EEState.MISSING_KEY, E2EEState.ENCRYPTION_FAILED, E2EEState.DECRYPTION_FAILED, E2EEState.INTERNAL_ERROR)
            assertEquals(state.name, expected, encryptionFailed(state))
        }
    }

    @Test
    fun aFailureToDecryptWaitsToSeeIfItPasses() {
        val failures = TrackFailures(graceMs = 4000)
        failures.report("TR_1", E2EEState.MISSING_KEY, nowMs = 1000)
        assertFalse(failures.failing(nowMs = 4999))
        assertTrue(failures.undecided(nowMs = 4999))

        // A repeat doesn't restart the clock.
        failures.report("TR_1", E2EEState.DECRYPTION_FAILED, nowMs = 4000)
        assertTrue(failures.failing(nowMs = 5000))
        assertFalse(failures.undecided(nowMs = 5000))

        failures.report("TR_1", E2EEState.OK, nowMs = 5100)
        assertFalse(failures.failing(nowMs = 9000))
    }

    @Test
    fun theSenderSideIsReportedAtOnce() {
        val failures = TrackFailures(graceMs = 0)
        failures.report("TR_1", E2EEState.ENCRYPTION_FAILED, nowMs = 1000)
        assertTrue(failures.failing(nowMs = 1000))
    }

    @Test
    fun aTrackThatWentAwayStopsCounting() {
        val failures = TrackFailures(graceMs = 0)
        failures.report("TR_1", E2EEState.DECRYPTION_FAILED, nowMs = 0)
        assertFalse(failures.keep(setOf("TR_1", "TR_2")))
        assertTrue(failures.failing(nowMs = 0))
        assertTrue(failures.keep(setOf("TR_2")))
        assertFalse(failures.failing(nowMs = 0))
    }

    @Test
    fun removalTakesTheCallOffTheJoinBanner() {
        val active = mapOf("group-a" to ActiveGroupCall("call-1", "voice", 3), "group-b" to ActiveGroupCall("call-2", "video", 2))
        assertEquals(setOf("group-b"), active.withoutCall("call-1").keys)
        assertEquals(active, active.withoutCall("call-3"))
    }
}
