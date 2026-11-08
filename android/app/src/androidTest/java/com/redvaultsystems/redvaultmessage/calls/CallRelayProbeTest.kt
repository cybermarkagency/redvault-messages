// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.calls

import android.util.Log
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.redvaultsystems.redvaultmessage.RedvaultApp
import com.redvaultsystems.redvaultmessage.data.SignupConsent
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.webrtc.PeerConnection
import org.webrtc.RTCStats
import org.webrtc.RTCStatsReport
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Live one-to-one calls on the real Android app through a real TURN relay (docs/tor-routing-plan.md 13.5a): the app's own
 * CallManager, a web user in a headless browser as the other side (tor-core/poc/web/android_call_peer.mjs).
 *
 * Runs only when asked:  am instrument -w -e callProbe true -e androidHandle <h> -e webHandle <w> ...
 * Direction 1: the phone calls the web user. Direction 2: the web user calls the phone and the phone answers.
 * Each step is one JSON line under the "CallProbe" tag; the test fails if any step did. The WebRTC statistics come
 * from the call's own PeerConnection (read by reflection: the manager keeps it private).
 */
@RunWith(AndroidJUnit4::class)
class CallRelayProbeTest {
    private var failures = 0

    private fun step(name: String, ok: Boolean, vararg detail: Pair<String, Any?>) {
        val line = buildJsonObject {
            put("step", name)
            put("ok", ok)
            put("detail", buildJsonObject { detail.forEach { (k, v) -> put(k, v?.toString()) } })
        }
        Log.i("CallProbe", line.toString())
        if (!ok) failures++
    }

    private fun waitFor(ms: Long, condition: () -> Boolean): Boolean {
        val end = System.currentTimeMillis() + ms
        while (System.currentTimeMillis() < end) {
            if (runCatching(condition).getOrDefault(false)) return true
            Thread.sleep(250)
        }
        return false
    }

    private class Stats(
        val policyRelayOnly: Boolean,
        val selectedLocal: String?,
        val selectedRemote: String?,
        val localTypes: Set<String>,
        val audioIn: Long,
        val audioOut: Long,
        val state: String,
    )

    /** The current call's PeerConnection statistics, or null when there's no call. */
    private fun stats(manager: CallManager): Stats? {
        val sessionField = CallManager::class.java.getDeclaredField("session").apply { isAccessible = true }
        val session = sessionField.get(manager) ?: return null
        val pcField = session.javaClass.getDeclaredField("pc").apply { isAccessible = true }
        val pc = pcField.get(session) as? PeerConnection ?: return null
        val latch = CountDownLatch(1)
        var report: RTCStatsReport? = null
        pc.getStats { r ->
            report = r
            latch.countDown()
        }
        if (!latch.await(5, TimeUnit.SECONDS)) return null
        val all: Map<String, RTCStats> = report!!.statsMap
        fun member(s: RTCStats, key: String) = s.members[key]
        val transport = all.values.firstOrNull { it.type == "transport" && member(it, "selectedCandidatePairId") != null }
        val pair = transport?.let { all[member(it, "selectedCandidatePairId") as String] }
            ?: all.values.firstOrNull { it.type == "candidate-pair" && member(it, "nominated") == true && member(it, "state") == "succeeded" }
        val local = pair?.let { all[member(it, "localCandidateId") as? String] }
        val remote = pair?.let { all[member(it, "remoteCandidateId") as? String] }
        val localTypes = all.values.filter { it.type == "local-candidate" }.mapNotNull { member(it, "candidateType") as? String }.toSet()
        val audioIn = all.values.filter { it.type == "inbound-rtp" && member(it, "kind") == "audio" }.sumOf { (member(it, "packetsReceived") as? Number)?.toLong() ?: 0L }
        val audioOut = all.values.filter { it.type == "outbound-rtp" && member(it, "kind") == "audio" }.sumOf { (member(it, "packetsSent") as? Number)?.toLong() ?: 0L }
        return Stats(
            policyRelayOnly = pc.connectionState() != null && localTypes.isNotEmpty() && localTypes.all { it == "relay" },
            selectedLocal = local?.let { member(it, "candidateType") as? String },
            selectedRemote = remote?.let { member(it, "candidateType") as? String },
            localTypes = localTypes,
            audioIn = audioIn,
            audioOut = audioOut,
            state = pc.connectionState().name,
        )
    }

    /** Watches an active call for [seconds]: relay all the way, and audio packets keep arriving and leaving. */
    private fun watch(label: String, manager: CallManager, seconds: Int) {
        var first: Stats? = null
        var last: Stats? = null
        val end = System.currentTimeMillis() + seconds * 1000L
        while (System.currentTimeMillis() < end) {
            Thread.sleep(5_000)
            val s = runCatching { stats(manager) }.getOrNull() ?: continue
            if (first == null) first = s
            last = s
            Log.i("CallProbe", "$label t=${seconds - (end - System.currentTimeMillis()) / 1000}s state=${s.state} in=${s.audioIn} out=${s.audioOut} pair=${s.selectedLocal}/${s.selectedRemote}")
        }
        val relayed = last != null && last.selectedLocal == "relay" && last.selectedRemote == "relay" && last.localTypes.all { it == "relay" }
        step(
            "${label}_relay_only",
            relayed,
            "selected" to "${last?.selectedLocal}/${last?.selectedRemote}",
            "localCandidateTypes" to last?.localTypes,
        )
        val flowing = first != null && last != null && last.audioIn > first.audioIn && last.audioOut > first.audioOut
        step(
            "${label}_audio_flows_both_ways",
            flowing,
            "received" to "${first?.audioIn}->${last?.audioIn}",
            "sent" to "${first?.audioOut}->${last?.audioOut}",
            "state" to last?.state,
        )
    }

    @Test
    fun theRealAppCallsThroughTheRelay() = runBlocking {
        val args = InstrumentationRegistry.getArguments()
        assumeTrue("run with -e callProbe true", args.getString("callProbe") == "true")
        val androidHandle = args.getString("androidHandle") ?: error("-e androidHandle <handle>")
        val webHandle = args.getString("webHandle") ?: error("-e webHandle <handle>")
        val seconds = args.getString("seconds")?.toInt() ?: 40
        val app = ApplicationProvider.getApplicationContext<RedvaultApp>()
        val instrumentation = InstrumentationRegistry.getInstrumentation()

        val password = "correct horse battery staple ${androidHandle.takeLast(6)}"
        val consent = SignupConsent(termsAccepted = true, privacyAcknowledged = true, age18Confirmed = true)
        val me = app.container.authRepository.register(androidHandle, password, consent).identity
        val session = app.container.createSession(me) {}
        val calls = session.calls
        // The live channel (rings, answers, calls-enabled) is only polled while something holds it: the UI does in the app.
        session.live.hold("callProbe")
        step("calls_available_on_this_server", waitFor(60_000) { calls.state.value.available })

        session.account.sendFriendRequest(webHandle)
        val conversation = run {
            var found: com.redvaultsystems.redvaultmessage.data.model.Conversation? = null
            val ok = waitFor(240_000) {
                runBlocking { session.chats.refreshNow() }
                found = session.chats.state.value.conversations.firstOrNull { !it.isGroup && it.handle.startsWith(webHandle) }
                found != null
            }
            step("web_user_accepted_and_chat_exists", ok)
            found ?: throw AssertionError("no chat with $webHandle")
        }

        // Direction 1: the phone calls the web user.
        instrumentation.runOnMainSync { calls.startCall(conversation, "voice") }
        val up1 = waitFor(150_000) { calls.state.value.call?.phase == CallPhase.Active }
        step("outgoing_call_active", up1, "phase" to calls.state.value.call?.phase, "ended" to calls.state.value.call?.endedMessage)
        if (up1) {
            step("safety_words_shown", calls.state.value.call?.sas?.size == 4, "sas" to calls.state.value.call?.sas)
            watch("outgoing", calls, seconds)
        }
        instrumentation.runOnMainSync { calls.hangUp() }
        waitFor(20_000) { calls.state.value.call == null || calls.state.value.call?.phase == CallPhase.Ended }
        instrumentation.runOnMainSync { calls.dismiss() }

        // Direction 2: the web user calls the phone, and the phone answers.
        val ring = waitFor(240_000) { calls.state.value.call?.phase == CallPhase.Incoming }
        step("incoming_call_rings", ring)
        if (ring) {
            instrumentation.runOnMainSync { calls.accept(true) }
            val up2 = waitFor(150_000) { calls.state.value.call?.phase == CallPhase.Active }
            step("incoming_call_active", up2, "phase" to calls.state.value.call?.phase, "ended" to calls.state.value.call?.endedMessage)
            if (up2) watch("incoming", calls, seconds)
            instrumentation.runOnMainSync { calls.hangUp() }
        }
        if (failures > 0) throw AssertionError("$failures probe step(s) failed (see the CallProbe log)")
    }
}
