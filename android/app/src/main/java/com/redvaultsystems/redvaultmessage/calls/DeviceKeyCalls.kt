// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.calls

import com.redvaultsystems.redvaultmessage.data.api.CarrierDto
import com.redvaultsystems.redvaultmessage.devicekeys.CallEnvelopeHub
import com.redvaultsystems.redvaultmessage.devicekeys.CallKeyMessage
import com.redvaultsystems.redvaultmessage.devicekeys.CallMessage
import com.redvaultsystems.redvaultmessage.devicekeys.V3Chat
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive

/**
 * Part B (docs/signal-integration/09-part-b-calls.md): what the call managers need from device keys. In a chat on
 * device keys a one-to-one call's offer and answer, and a group call's room keys, travel as device-key messages
 * (Signal wherever every device declares it); the server only gets {"carrier":"device-keys"} for the call's steps.
 * Call links and chats that aren't on device keys keep protocol 1 ([com.redvaultsystems.redvaultmessage.crypto.CallCrypto]).
 */
interface DeviceKeyCallCarrier {
    /** Where the mailbox puts call messages until a call takes them. */
    val envelopes: CallEnvelopeHub

    /** Whether this 1:1 chat's calls go over device keys (null: not available here, the usual way). Throws for a pinned chat that can't. */
    suspend fun usesDeviceKeys(conversationId: String, peerUserId: String): Boolean?

    /** Whether this chat is on device keys here (it has sent or received on them): its calls never go the older way. */
    fun chatOnDeviceKeys(conversationId: String): Boolean

    /** Whether this group's calls go over device keys. Same answers as [usesDeviceKeys]. */
    suspend fun groupUsesDeviceKeys(groupId: String): Boolean?

    /** Sends a call step to [peerUserId]'s devices (or [onlyDevice] alone). Throws when it can't go out within 30 s. */
    suspend fun sendCall(conversationId: String, peerUserId: String, call: CallMessage, onlyDevice: String?)

    /** Sends a room key pairwise to the devices of [participants]. Throws when it can't go out within 30 s. */
    suspend fun sendCallKey(groupId: String, participants: List<String>, key: CallKeyMessage)

    /** Reads the device-key mailbox now (a call event woke this phone, like `v3-mailbox`). Never throws. */
    suspend fun readMailbox()
}

/** The carrier over this phone's [V3Chat]; [readMailbox] is the chat list's own mailbox pass (so chat messages show too). */
class V3CallCarrier(private val chat: V3Chat, private val mailbox: suspend () -> Unit) : DeviceKeyCallCarrier {
    override val envelopes: CallEnvelopeHub get() = chat.callEnvelopes

    override suspend fun usesDeviceKeys(conversationId: String, peerUserId: String): Boolean? = chat.callsOnDeviceKeys(conversationId, peerUserId)

    override fun chatOnDeviceKeys(conversationId: String): Boolean = chat.pinned(conversationId)

    override suspend fun groupUsesDeviceKeys(groupId: String): Boolean? = chat.groupCallsOnDeviceKeys(groupId)

    override suspend fun sendCall(conversationId: String, peerUserId: String, call: CallMessage, onlyDevice: String?) =
        chat.sendCall(conversationId, peerUserId, call, onlyDevice)

    override suspend fun sendCallKey(groupId: String, participants: List<String>, key: CallKeyMessage) =
        chat.sendCallKey(groupId, participants, key)

    override suspend fun readMailbox() {
        try {
            mailbox()
        } catch (e: kotlinx.coroutines.CancellationException) {
            throw e
        } catch (e: Exception) {
            // The wait for the call message decides what happens; nothing here names a key or content.
        }
    }
}

/** What the call routes take instead of a sealed box in a chat on device keys. */
val DEVICE_KEYS_MARKER = CarrierDto("device-keys")

/** Whether a call event's offer, answer or signal is exactly the device-key marker (the SDP is in the mailbox). */
fun isDeviceKeysMarker(element: JsonElement?): Boolean {
    val o = element as? JsonObject ?: return false
    val carrier = o["carrier"] as? JsonPrimitive ?: return false
    return o.size == 1 && carrier.isString && carrier.content == DEVICE_KEYS_MARKER.carrier
}

/**
 * The callee's rule for a 1:1 contact call (never a call link): an offer that came as a protocol-1 sealed box
 * ([markedForDeviceKeys] false) in a chat that is on device keys is refused and declined, the same "no way back" as
 * step 1, so an offer sealed with the legacy key (possibly forged with it) never rings there.
 */
fun refuseOlderOffer(markedForDeviceKeys: Boolean, chatOnDeviceKeys: Boolean): Boolean = !markedForDeviceKeys && chatOnDeviceKeys

const val OLDER_WAY_REFUSED = "This call came the older way and was refused"

/** How long a phone waits for a call message the live events announced before the call fails (part B step 3). */
const val CALL_MESSAGE_WAIT_MS = 20_000L
