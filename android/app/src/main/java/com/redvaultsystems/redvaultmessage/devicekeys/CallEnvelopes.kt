// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.withTimeoutOrNull

/**
 * Part B (docs/signal-integration/09-part-b-calls.md): call setup and group call room keys as device-key messages.
 * What the mailbox found ([IncomingCallEnvelope]), where it waits for the call that wants it ([CallEnvelopeHub]),
 * and the checks a call makes before using it ([refuseCall], [refuseCallKey]). No Android classes here.
 */

/**
 * A `call` or `callkey` envelope as the mailbox delivered it: its sender's device was in their verified list and it
 * came pairwise (never through a group session). [forDevice] is the device that read it. Exactly one of [call] and
 * [key] is set.
 */
class IncomingCallEnvelope(
    val senderUserId: String,
    val senderDeviceId: String,
    val conversationId: String,
    val forDevice: String,
    val ts: Long,
    val call: CallMessage? = null,
    val key: CallKeyMessage? = null,
) {
    val callId: String get() = call?.callId ?: key!!.callId

    override fun toString(): String = "IncomingCallEnvelope(from=$senderUserId/$senderDeviceId, c=$conversationId, ${call ?: key})"
}

/**
 * Call envelopes waiting for their call. The mailbox can deliver an offer before the live event that names its call
 * (the caller sends the offer, then posts it), so they're held here for a while: at most [MAX_HELD], none older than
 * [CallLimits.MAX_AGE_MS]. An envelope whose call never comes is dropped unused. Memory only.
 */
class CallEnvelopeHub(private val clock: () -> Long = System::currentTimeMillis) {
    private val held = ArrayList<IncomingCallEnvelope>()
    private val version = MutableStateFlow(0L)

    /** Changes whenever something arrives: a group call watches it for room keys. */
    val arrivals: StateFlow<Long> = version.asStateFlow()

    fun offer(envelope: IncomingCallEnvelope) {
        synchronized(held) {
            prune()
            held += envelope
            while (held.size > MAX_HELD) held.removeAt(0).wipe()
        }
        version.update { it + 1 }
    }

    /** Takes (removes) the oldest envelope [matching], or null. */
    fun take(matching: (IncomingCallEnvelope) -> Boolean): IncomingCallEnvelope? = synchronized(held) {
        prune()
        val i = held.indexOfFirst(matching)
        if (i < 0) null else held.removeAt(i)
    }

    /** Takes every envelope [matching], oldest first. */
    fun takeAll(matching: (IncomingCallEnvelope) -> Boolean): List<IncomingCallEnvelope> = synchronized(held) {
        prune()
        val out = held.filter(matching)
        held.removeAll(out.toSet())
        out
    }

    /** Waits up to [timeoutMs] for an envelope [matching] (one held already counts), and takes it. */
    suspend fun await(timeoutMs: Long, matching: (IncomingCallEnvelope) -> Boolean): IncomingCallEnvelope? {
        // One held already is taken whatever the time left, never lost to a timeout that fires first.
        var found: IncomingCallEnvelope? = take(matching)
        if (found != null) return found
        withTimeoutOrNull(timeoutMs) {
            version.first {
                found = take(matching)
                found != null
            }
        }
        return found
    }

    fun clear() = synchronized(held) {
        held.forEach { it.wipe() }
        held.clear()
    }

    private fun prune() {
        val oldest = clock() - CallLimits.MAX_AGE_MS
        held.removeAll { (it.ts < oldest).also { old -> if (old) it.wipe() } }
    }

    private fun IncomingCallEnvelope.wipe() {
        key?.key?.fill(0)
    }

    companion object {
        const val MAX_HELD = 64
    }
}

/**
 * What a one-to-one call expects next: [step] of call [callId] in the chat [conversationId], from [peerUserId] (the
 * call's other party), of the call's [kind]. [peerDeviceId]: the one device of theirs this call is with, once known
 * (the device that sent the offer, or the one that answered); null while any of their devices may send it.
 */
data class ExpectedCallMessage(
    val callId: String,
    val conversationId: String,
    val peerUserId: String,
    val step: String,
    val kind: String,
    val peerDeviceId: String? = null,
)

/**
 * Why [envelope] can't be used as [expected] at [now] (a code, never content), or null when it can. Refused: not a
 * call message, another call or step, another chat, a sender who isn't the call's other party (or not their device
 * this call is with), meant for another device, or more than 120 s old.
 */
fun refuseCall(envelope: IncomingCallEnvelope, expected: ExpectedCallMessage, now: Long): String? {
    val call = envelope.call ?: return "not_a_call"
    return when {
        call.callId != expected.callId -> "unknown_call"
        call.step != expected.step -> "wrong_step"
        envelope.conversationId != expected.conversationId.lowercase() -> "wrong_conversation"
        envelope.senderUserId != expected.peerUserId -> "wrong_sender"
        expected.peerDeviceId != null && envelope.senderDeviceId != expected.peerDeviceId -> "wrong_device"
        call.to != CallMessage.EVERY_DEVICE && call.to != envelope.forDevice -> "not_for_this_device"
        call.kind != expected.kind -> "wrong_kind"
        now - envelope.ts > CallLimits.MAX_AGE_MS -> "stale_call"
        else -> null
    }
}

/**
 * Why a group call's room key can't be used (a code), or null when it can: it must be for [callId] in [groupId], from
 * someone in [members] (the group's current members), with an epoch at least [heldEpoch] (the newest held for this
 * call). Its sender's device was in their verified list already, or the mailbox wouldn't have delivered it. The call
 * itself still checks that it came from the epoch's keyholder.
 */
fun refuseCallKey(envelope: IncomingCallEnvelope, callId: String, groupId: String, members: Set<String>, heldEpoch: Long): String? {
    val key = envelope.key ?: return "not_a_call_key"
    return when {
        key.callId != callId -> "unknown_call"
        envelope.conversationId != groupId.lowercase() -> "wrong_conversation"
        envelope.senderUserId !in members -> "not_a_member"
        key.epoch < heldEpoch -> "old_epoch"
        else -> null
    }
}
