package com.redvaultsystems.redvaultmessage.calls

import io.livekit.android.e2ee.E2EEState

/**
 * The rules that keep a group call's frames encrypted, apart from LiveKit so they can be tested. What they
 * rest on, read from the LiveKit SDK (2.28.2) and libwebrtc (webrtc-sdk, m144):
 *
 *   - E2EEManager.removePublishedTrack disables the track's frame cryptor and disposes it.
 *   - A disabled cryptor forwards frames unencrypted; only a cryptor that's on drops the ones it can't
 *     encrypt (missing key, failed encryption).
 *   - So between remove and add, and after a remove with no add, anything the track produces leaves in the
 *     clear. A disabled track only produces silence or black, but frames already in the encoder are real.
 */
internal interface RekeyableSender {
    var enabled: Boolean

    /** Takes off the track's frame cryptor and attaches a new one, on the newest key slot. */
    fun reattach()
}

/**
 * Moves [senders] to the newest key. Each stays disabled from before its old cryptor comes off until every one
 * has its new cryptor, and [settle] gives what's already in the encoder time to come out under the old one.
 *
 * Returns false if any of them couldn't be reattached. They're then all left disabled: only the caller, by ending
 * the call, decides what happens next. A track that was muted stays muted.
 */
internal suspend fun moveToNewKey(senders: List<RekeyableSender>, settle: suspend () -> Unit): Boolean {
    if (senders.isEmpty()) return true
    val wasEnabled = senders.map { it.enabled }
    senders.forEach { it.enabled = false }
    settle()
    for (sender in senders) {
        try {
            sender.reattach()
        } catch (e: Exception) {
            return false
        }
    }
    senders.forEachIndexed { i, sender -> sender.enabled = wasEnabled[i] }
    return true
}

/**
 * Installs a new epoch's key. If that fails nothing may go out: without the new key the senders stay on the old one,
 * which someone removed from the call still holds. Every sender is disabled before this returns false, and the caller
 * ends the call.
 */
internal fun installOrSilence(install: () -> Boolean, senders: List<RekeyableSender>): Boolean {
    val installed = try {
        install()
    } catch (e: Exception) {
        false
    }
    if (!installed) senders.forEach { sender -> runCatching { sender.enabled = false } }
    return installed
}

/** Whether anything may be published: the room's E2EE is on and this phone holds a room key. */
internal fun mayPublish(e2eeEnabled: Boolean?, hasKey: Boolean, keyEpoch: Long): Boolean =
    e2eeEnabled == true && hasKey && keyEpoch != 0L

/** States that mean a track's frames aren't getting through: libwebrtc drops what it can't encrypt or decrypt. */
internal fun encryptionFailed(state: E2EEState): Boolean = when (state) {
    E2EEState.MISSING_KEY, E2EEState.ENCRYPTION_FAILED, E2EEState.DECRYPTION_FAILED, E2EEState.INTERNAL_ERROR -> true
    E2EEState.NEW, E2EEState.OK, E2EEState.KEY_RATCHETED -> false
}

/**
 * Which tracks are failing to encrypt or decrypt. [graceMs] lets a passing failure go by unmentioned: a frame
 * under the next epoch's key can arrive just before that key does, which fixes itself.
 */
internal class TrackFailures(private val graceMs: Long) {
    private val failingSince = HashMap<String, Long>()

    fun report(trackSid: String, state: E2EEState, nowMs: Long) {
        if (encryptionFailed(state)) failingSince.putIfAbsent(trackSid, nowMs) else failingSince.remove(trackSid)
    }

    /** Forgets tracks that have gone away. True if that changed anything. */
    fun keep(trackSids: Set<String>): Boolean = failingSince.keys.retainAll(trackSids)

    /** A failure has lasted past the grace period. */
    fun failing(nowMs: Long): Boolean = failingSince.values.any { nowMs - it >= graceMs }

    /** Something is failing that hasn't lasted long enough to mention yet. */
    fun undecided(nowMs: Long): Boolean = failingSince.values.any { nowMs - it < graceMs }
}

/** The server took this account out of a group: {"type":"group-removed","callId":...}. The event names no group. */
internal fun Map<String, ActiveGroupCall>.withoutCall(callId: String): Map<String, ActiveGroupCall> =
    filterValues { it.callId != callId }
