// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import java.time.LocalDate
import java.time.ZoneOffset
import java.util.UUID

/**
 * The v3 chat path (docs/multi-device-threat-model.md section 9): 1:1 and group (Megolm) text, files, reactions,
 * edits and deletes with copies for the sender's own devices, and the receive loop (the envelope and what the chat
 * shows are in ChatContent.kt). DEVELOPMENT ONLY. Implemented against 9.3-9.6;
 * ask the session that owns the server before changing the format.
 */
object ChatLimits {
    const val MAX_TEXT_BYTES = 16 * 1024
    const val MAX_RECIPIENTS = 100
    const val ACK_BATCH = 500
    const val PAST_MS = 30L * 24 * 3600 * 1000
    const val FUTURE_MS = 5L * 60 * 1000
    const val ROOM_KEY_WAIT_DAYS = 2L
}

/** Errors about the sender's list, not the message: the item waits until the person accepts the change. */
private val HOLD_CODES = setOf(
    "account_key_changed", "rollback", "equivocation", "uncertified_account_key", "bad_device_list",
    // rv-signal-v1: a device's Signal identity changed or doesn't match its signed list. Held, never dropped (M5).
    "identity_changed", "certificate_mismatch", "untrusted_identity",
)

/** What a send did. [Protocol1] means this chat isn't on v3 (no device keys on the other side, not pinned): send it the usual way. */
sealed interface SendOutcome {
    data class Sent(val record: ChatRecord) : SendOutcome
    data object Protocol1 : SendOutcome
}

private fun JsonObject.identityKey(): Pair<String, String> =
    this["userId"]!!.jsonPrimitive.content to this["deviceId"]!!.jsonPrimitive.content

/**
 * Claims keys and encrypts [plaintext] to [recipients] with encryptMany; `no_session` ones get a claimed key and a second go (9.4 step 2).
 * The engine picks the protocol per device: rv-signal-v1 where both devices speak it (it then asks for a Signal bundle with
 * `no_signal_session`), Olm elsewhere. A device that speaks Signal but has no keys on the server fails the send: never an Olm copy.
 */
private suspend fun encryptToDevices(
    device: V3Device,
    conversationId: String,
    plaintext: String,
    recipients: List<JsonObject>,
): List<Triple<String, String, String>> {
    if (recipients.isEmpty()) return emptyList()
    fun results(response: EngineResponse) = response.array("results").map { it.jsonObject }
    val first = results(device.must("encryptMany", "plaintext" to plaintext, "recipients" to recipientsJson(recipients.map { it to null })))
    val out = ArrayList<Triple<String, String, String>>()
    val needKeys = ArrayList<JsonObject>()
    val needSignal = ArrayList<JsonObject>()
    first.forEachIndexed { i, r ->
        when {
            r["error"]?.jsonPrimitive?.contentOrNull == "no_session" -> needKeys += recipients[i]
            r["error"]?.jsonPrimitive?.contentOrNull == "no_signal_session" -> needSignal += recipients[i]
            r["error"] != null -> throw EngineException(r["error"]!!.jsonPrimitive.content, r["message"]?.jsonPrimitive?.contentOrNull ?: "encryption failed")
            else -> out += Triple(recipients[i]["userId"]!!.jsonPrimitive.content, recipients[i]["deviceId"]!!.jsonPrimitive.content, r["message"].toString())
        }
    }
    if (needKeys.isNotEmpty()) {
        val claimed = device.api.claimKeys(conversationId, needKeys.map { it.identityKey() })
        val keyed = needKeys.mapIndexed { i, r -> r to (claimed[i]["oneTimeKey"].takeIf { it is JsonObject } ?: claimed[i]["fallbackKey"].takeIf { it is JsonObject }) }
        val retry = results(device.must("encryptMany", "plaintext" to plaintext, "recipients" to recipientsJson(keyed)))
        retry.forEachIndexed { i, r ->
            if (r["error"] != null) throw EngineException(r["error"]!!.jsonPrimitive.content, r["message"]?.jsonPrimitive?.contentOrNull ?: "encryption failed")
            out += Triple(needKeys[i]["userId"]!!.jsonPrimitive.content, needKeys[i]["deviceId"]!!.jsonPrimitive.content, r["message"].toString())
        }
    }
    if (needSignal.isNotEmpty()) {
        val claimed = device.api.signalClaim(conversationId, needSignal.map { it.identityKey() })
        val withBundles = kotlinx.serialization.json.buildJsonArray {
            needSignal.forEachIndexed { i, identity ->
                add(buildJsonObject {
                    put("identity", identity)
                    put("signalBundle", buildJsonObject {
                        put("certificate", claimed[i]["certificate"] ?: kotlinx.serialization.json.JsonNull)
                        put("bundle", claimed[i]["bundle"] ?: kotlinx.serialization.json.JsonNull)
                    })
                })
            }
        }
        val retry = results(device.must("encryptMany", "plaintext" to plaintext, "recipients" to withBundles))
        retry.forEachIndexed { i, r ->
            if (r["error"] != null) throw EngineException(r["error"]!!.jsonPrimitive.content, r["message"]?.jsonPrimitive?.contentOrNull ?: "encryption failed")
            out += Triple(needSignal[i]["userId"]!!.jsonPrimitive.content, needSignal[i]["deviceId"]!!.jsonPrimitive.content, r["message"].toString())
        }
    }
    return out
}

private fun recipientsJson(list: List<Pair<JsonObject, kotlinx.serialization.json.JsonElement?>>) = kotlinx.serialization.json.buildJsonArray {
    for ((identity, key) in list) add(buildJsonObject { put("identity", identity); if (key != null) put("oneTimeKey", key) })
}

private fun staleOnce(e: Throwable) = e is ApiException && e.code == "device_list_stale"

/**
 * Sends [text] to every device of [contactUserId] and every other device of this account (9.4). Returns
 * [SendOutcome.Protocol1] when the contact has no device keys and the chat isn't pinned to v3; throws
 * EngineException("contact_has_no_devices") when it is pinned (never a silent fall-back, 9.1 rule 5).
 * A failed send is kept in history as `failed`; pass the same [id] to retry (it is re-encrypted).
 */
suspend fun sendText(
    device: V3Device,
    history: ChatHistory,
    conversationId: String,
    contactUserId: String,
    text: String,
    id: String = UUID.randomUUID().toString(),
    now: Long = System.currentTimeMillis(),
    legacySigningKey: String? = null,
): SendOutcome {
    require(text.toByteArray().size <= ChatLimits.MAX_TEXT_BYTES) { "A message is at most 16 KiB." }
    val conversation = conversationId.lowercase()
    val messageId = id.lowercase()
    val record = ChatRecord(conversation, messageId, device.userId, device.deviceId, now, now, text, "out", "sent")
    return deliverDirect(device, history, contactUserId, encodeText(conversation, text, messageId, now), record, legacySigningKey = legacySigningKey)
}

/**
 * Whether a one-to-one chat goes out on device keys, by the rule [sendText] follows: asked before a file is uploaded,
 * so a chat that has to refuse does so first. Throws like [sendText] for a pinned chat whose contact has no devices.
 */
suspend fun contactOnDeviceKeys(device: V3Device, history: ChatHistory, conversationId: String, contactUserId: String): Boolean {
    val conversation = conversationId.lowercase()
    if (verifiedDevices(device, contactUserId, conversation).devices.isNotEmpty()) return true
    if (history.pinned(conversation)) throw noDevices()
    return false
}

private fun noDevices() = EngineException("contact_has_no_devices", "This chat uses device keys, but the contact has none listed. Nothing was sent.")

/** Sends [envelope] and files [record] in the history. A failed send stays there as `failed` only when [keepFailed]. */
private suspend fun deliverDirect(
    device: V3Device,
    history: ChatHistory,
    contactUserId: String,
    envelope: String,
    record: ChatRecord,
    keepFailed: Boolean = true,
    legacySigningKey: String? = null,
): SendOutcome {
    val conversation = record.conversationId
    var attempt = 0
    while (true) {
        attempt++
        try {
            val theirs = verifiedDevices(device, contactUserId, conversation, legacySigningKey).devices
            if (theirs.isEmpty()) {
                if (history.pinned(conversation)) throw noDevices()
                return SendOutcome.Protocol1
            }
            val mine = verifiedDevices(device, device.userId, null).devices.filter { it["deviceId"]!!.jsonPrimitive.content != device.deviceId }
            val messages = encryptToDevices(device, conversation, envelope, theirs + mine)
            try {
                device.api.sendMessages(device.deviceId, conversation, messages)
            } catch (e: Throwable) {
                if (keepFailed && (!staleOnce(e) || attempt > 1)) history.put(record.copy(status = "failed"), replace = true)
                throw e
            }
            history.pin(conversation)
            history.put(record, replace = true)
            return SendOutcome.Sent(record)
        } catch (e: ApiException) {
            if (staleOnce(e) && attempt == 1) continue
            throw e
        }
    }
}

/** Fetches and verifies every member's list and returns all their listed devices (9.5 step 1). */
private suspend fun verifiedGroupDevices(device: V3Device, groupId: String): List<JsonObject> {
    val out = ArrayList<JsonObject>()
    for ((userId, view) in device.api.conversationDeviceLists(groupId)) {
        val list = view.deviceList ?: continue
        val verified = device.op("verifyDeviceList", "deviceList" to list)
        if (!verified.ok) throw EngineException(verified.error ?: "bad_device_list", "$userId: " + (verified.message ?: "the device list doesn't verify"))
        out += verified.array("devices").map { it.jsonObject }
    }
    return out
}

/** Sends [text] to a group (9.5): Megolm session keys to unshared devices first, then one group message for everyone. */
suspend fun sendGroupText(
    device: V3Device,
    history: ChatHistory,
    groupId: String,
    text: String,
    id: String = UUID.randomUUID().toString(),
    now: Long = System.currentTimeMillis(),
): SendOutcome {
    require(text.toByteArray().size <= ChatLimits.MAX_TEXT_BYTES) { "A message is at most 16 KiB." }
    val group = groupId.lowercase()
    val messageId = id.lowercase()
    val record = ChatRecord(group, messageId, device.userId, device.deviceId, now, now, text, "out", "sent")
    return deliverGroup(device, history, encodeText(group, text, messageId, now), record)
}

/** [deliverDirect] for a group; [record]'s conversation is the group. */
private suspend fun deliverGroup(device: V3Device, history: ChatHistory, envelope: String, record: ChatRecord, keepFailed: Boolean = true): SendOutcome {
    val group = record.conversationId
    var attempt = 0
    while (true) {
        attempt++
        try {
            val members = verifiedGroupDevices(device, group)
            if (members.none { it["deviceId"]!!.jsonPrimitive.content == device.deviceId }) {
                throw EngineException("device_not_listed", "This device isn't in your account's list.")
            }
            if (members.size > ChatLimits.MAX_RECIPIENTS + 1) throw EngineException("too_many_devices", "A group on device keys has at most 100 devices.")
            // M6: Signal sender keys when every device in the group speaks Signal (B1); Megolm otherwise.
            if (groupUsesSignal(device, members)) {
                try {
                    sendSignalGroup(device, group, envelope, members)
                } catch (e: Throwable) {
                    if (keepFailed && (!staleOnce(e) || attempt > 1)) history.put(record.copy(status = "failed"), replace = true)
                    throw e
                }
                history.pin(group)
                history.put(record, replace = true)
                return SendOutcome.Sent(record)
            }
            device.must("groupSetMembers", "groupId" to group, "members" to kotlinx.serialization.json.JsonArray(members))
            val session = device.must("groupSession", "groupId" to group)
            val unshared = session.array("unshared").map { it.jsonObject }
            if (unshared.isNotEmpty()) {
                val keyMessages = ArrayList<Triple<String, String, String>>()
                val needKeys = ArrayList<JsonObject>()
                for (member in unshared) {
                    val r = device.op("shareRoomKey", "groupId" to group, "recipient" to member)
                    when {
                        r.ok -> keyMessages += Triple(member["userId"]!!.jsonPrimitive.content, member["deviceId"]!!.jsonPrimitive.content, r.obj("message").toString())
                        r.error == "no_session" -> needKeys += member
                        else -> throw EngineException(r.error ?: "engine_error", r.message ?: "shareRoomKey")
                    }
                }
                if (needKeys.isNotEmpty()) {
                    val claimed = device.api.claimKeys(group, needKeys.map { it.identityKey() })
                    needKeys.forEachIndexed { i, member ->
                        val key = claimed[i]["oneTimeKey"].takeIf { it is JsonObject } ?: claimed[i]["fallbackKey"].takeIf { it is JsonObject }
                        val r = device.must("shareRoomKey", "groupId" to group, "recipient" to member, "oneTimeKey" to key)
                        keyMessages += Triple(member["userId"]!!.jsonPrimitive.content, member["deviceId"]!!.jsonPrimitive.content, r.obj("message").toString())
                    }
                }
                try {
                    keyMessages.chunked(ChatLimits.MAX_RECIPIENTS).forEach { device.api.sendMessages(device.deviceId, group, it) }
                } catch (e: Throwable) {
                    // The engine counts these keys as shared; a new session makes the next send share again.
                    device.op("groupSession", "groupId" to group, "rotate" to true)
                    throw e
                }
            }
            val sealed = device.must("groupEncrypt", "groupId" to group, "plaintext" to envelope)
            val body = buildJsonObject {
                put("v", 1)
                put("kind", "group")
                put("groupId", group)
                put("sessionId", sealed.string("sessionId"))
                put("ciphertext", sealed.string("ciphertext"))
            }.toString()
            val recipients = members.map { it.identityKey() }.filter { it.second != device.deviceId }
            try {
                device.api.sendGroupMessage(device.deviceId, group, body, recipients)
            } catch (e: Throwable) {
                if (keepFailed && (!staleOnce(e) || attempt > 1)) history.put(record.copy(status = "failed"), replace = true)
                throw e
            }
            history.pin(group)
            history.put(record, replace = true)
            return SendOutcome.Sent(record)
        } catch (e: ApiException) {
            if (staleOnce(e) && attempt == 1) continue
            throw e
        }
    }
}

/**
 * Sends a file already uploaded and released. Its key only ever travels in the envelope. [SendOutcome.Protocol1] and
 * failures as in [sendText]: a failed one stays in the history as `failed`, so it can be retried with the same [id].
 */
suspend fun sendChatFile(
    device: V3Device,
    history: ChatHistory,
    conversationId: String,
    isGroup: Boolean,
    contactUserId: String,
    file: FileDescriptor,
    id: String = UUID.randomUUID().toString(),
    now: Long = System.currentTimeMillis(),
): SendOutcome {
    val conversation = conversationId.lowercase()
    val messageId = id.lowercase()
    val record = ChatRecord(conversation, messageId, device.userId, device.deviceId, now, now, "", "out", "sent", file = file)
    val envelope = encodeFile(conversation, file, messageId, now)
    return if (isGroup) deliverGroup(device, history, envelope, record) else deliverDirect(device, history, contactUserId, envelope, record)
}

/**
 * Sends a reaction, edit or delete. A failed one isn't kept: the person sees it didn't work and can try again. It's
 * about a message on device keys, so it never goes the usual way: a contact with no devices is a refusal here.
 */
suspend fun sendChatOp(
    device: V3Device,
    history: ChatHistory,
    conversationId: String,
    isGroup: Boolean,
    contactUserId: String,
    op: ChatOp,
    id: String = UUID.randomUUID().toString(),
    now: Long = System.currentTimeMillis(),
) {
    if (op.body != null && op.body.toByteArray().size > ChatLimits.MAX_TEXT_BYTES) throw EngineException("too_long", "That message is too long")
    val conversation = conversationId.lowercase()
    val messageId = id.lowercase()
    val record = ChatRecord(conversation, messageId, device.userId, device.deviceId, now, now, "", "out", "sent", op = op)
    val envelope = encodeOp(conversation, op, messageId, now)
    val outcome = if (isGroup) deliverGroup(device, history, envelope, record, keepFailed = false) else deliverDirect(device, history, contactUserId, envelope, record, keepFailed = false)
    if (outcome !is SendOutcome.Sent) throw noDevices()
    if (op.t == ChatOp.DELETE) history.eraseDeleted(conversation)
}

/** What one pass of the receive loop found. */
class ReceiveReport(
    /** New messages written to history this pass (including copies of what this account sent from another device). */
    val received: List<ChatRecord>,
    /** Items kept on the server because the sender's keys changed, or a room key hasn't arrived: shown as "contact's keys changed" etc. */
    val held: List<String>,
    /** Items kept because they need a newer app ("update the app"). */
    val needUpdate: Int,
    /** Why items were acknowledged without being shown (codes only, never content): duplicates, unknown senders, undecryptable... */
    val dropped: List<String> = emptyList(),
)

private fun JsonObject.str(name: String) = this[name]?.jsonPrimitive?.contentOrNull

private fun incoming(conversationId: String, id: String, senderUserId: String, senderDeviceId: String, ts: Long, now: Long, device: V3Device): ChatRecord {
    val own = senderUserId == device.userId
    return ChatRecord(conversationId, id, senderUserId, senderDeviceId, ts, now, "", if (own) "out" else "in", if (own) "sent" else "received")
}

/**
 * Reads this device's mailbox (9.6). Every item is verified against the sender's signed list, decrypted,
 * validated (9.3), written to [history], and only then acknowledged. Items that can never succeed are
 * acknowledged too; items held for the sender's identity change, a late room key, or a newer app are not.
 */
suspend fun receiveMessages(device: V3Device, history: ChatHistory, now: Long = System.currentTimeMillis(), today: LocalDate = LocalDate.now(ZoneOffset.UTC)): ReceiveReport {
    val items = device.api.inbox(device.deviceId)
    val acks = ArrayList<String>()
    val received = ArrayList<ChatRecord>()
    val held = ArrayList<String>()
    var needUpdate = 0
    val dropped = ArrayList<String>()
    fun drop(id: String, why: String) {
        acks += id
        dropped += why
    }

    class Known(val devices: List<JsonObject>, val error: String?)
    val lists = HashMap<String, Known>()

    suspend fun sender(userId: String, conversationId: String?): Known = lists.getOrPut("$userId/${conversationId.orEmpty()}") {
        try {
            Known(verifiedDevices(device, userId, conversationId).devices, null)
        } catch (e: EngineException) {
            if (e.code == "not_linked" && userId == device.userId) {
                Known(device.api.deviceList(device.userId).deviceList?.get("devices")?.let { (it as kotlinx.serialization.json.JsonArray).map { d -> d.jsonObject } }.orEmpty(), null)
            } else {
                Known(emptyList(), e.code)
            }
        } catch (e: ApiException) {
            Known(emptyList(), "unreachable")
        }
    }

    suspend fun refreshMembers(groupId: String) {
        runCatching {
            val members = verifiedGroupDevices(device, groupId)
            device.op("groupSetMembers", "groupId" to groupId, "members" to kotlinx.serialization.json.JsonArray(members))
        }
    }

    for (item in items) {
        val id = item.str("id") ?: continue
        val senderUserId = item.str("senderUserId") ?: run { drop(id, "no_sender"); null } ?: continue
        val senderDeviceId = item.str("senderDeviceId").orEmpty()
        val conversationId = item.str("conversationId")?.lowercase()
        val known = sender(senderUserId, conversationId)
        if (known.error != null && known.error in HOLD_CODES) {
            held += id
            continue
        }
        val senderIdentity = known.devices.firstOrNull { it.str("deviceId") == senderDeviceId }
        if (senderIdentity == null) {
            // Unknown sender (not in their signed list), or the list couldn't be fetched right now.
            if (known.error == "unreachable") held += id else drop(id, "unknown_sender:" + (known.error ?: "not_in_list"))
            continue
        }
        val body = runCatching { Json.parseToJsonElement(item.str("body").orEmpty()).jsonObject }.getOrNull()
        if (body == null) {
            drop(id, "r1")
            continue
        }

        var plaintext: String? = null
        if (body.str("protocol") == "rv-signal-group-v1") {
            // M6: a Signal group message. Its roster is verified first; a key or roster that hasn't arrived waits.
            val groupId = body.str("groupId").orEmpty().lowercase()
            if (conversationId != groupId) {
                drop(id, "r2")
                continue
            }
            var r = device.op("groupSignalDecrypt", "sender" to senderIdentity, "message" to body)
            if (!r.ok && r.error == "roster_unknown") {
                runCatching { verifyRosters(device, groupId) }
                r = device.op("groupSignalDecrypt", "sender" to senderIdentity, "message" to body)
            }
            when {
                r.ok -> plaintext = r.string("plaintext")
                r.error == "no_sender_key" || r.error == "roster_unknown" -> {
                    val created = runCatching { LocalDate.parse(item.str("createdOn")) }.getOrNull()
                    if (created != null && today.toEpochDay() - created.toEpochDay() > ChatLimits.ROOM_KEY_WAIT_DAYS) drop(id, "no_sender_key") else held += id
                    continue
                }
                r.error in HOLD_CODES -> {
                    held += id
                    continue
                }
                else -> {
                    drop(id, "g:" + (r.error ?: "failed"))
                    continue
                }
            }
        } else if (body.str("kind") == "group") {
            val groupId = body.str("groupId").orEmpty().lowercase()
            if (conversationId != groupId) {
                drop(id, "r2")
                continue
            }
            var r = device.op("groupDecrypt", "groupId" to groupId, "sessionId" to body.str("sessionId"), "ciphertext" to body.str("ciphertext"))
            if (!r.ok && (r.error == "unknown_group" || r.error == "not_a_member")) {
                refreshMembers(groupId)
                r = device.op("groupDecrypt", "groupId" to groupId, "sessionId" to body.str("sessionId"), "ciphertext" to body.str("ciphertext"))
            }
            when {
                r.ok -> {
                    val author = r.obj("sender")
                    if (author.str("deviceId") != senderDeviceId || author.str("userId") != senderUserId) {
                        drop(id, "r3")
                        continue
                    }
                    plaintext = r.string("plaintext")
                }
                r.error == "unknown_session" -> {
                    val created = runCatching { LocalDate.parse(item.str("createdOn")) }.getOrNull()
                    if (created != null && today.toEpochDay() - created.toEpochDay() > ChatLimits.ROOM_KEY_WAIT_DAYS) drop(id, "no_room_key") else held += id
                    continue
                }
                else -> {
                    drop(id, "r4")
                    continue
                }
            }
        } else {
            var r = device.op("decrypt", "sender" to senderIdentity, "message" to body)
            // M6: a Signal sender key for a roster epoch this phone hasn't verified yet.
            if (!r.ok && r.error == "roster_unknown" && conversationId != null) {
                runCatching { verifyRosters(device, conversationId) }
                r = device.op("decrypt", "sender" to senderIdentity, "message" to body)
            }
            if (!r.ok && (r.error == "unknown_group" || r.error == "not_a_member") && conversationId != null) {
                refreshMembers(conversationId)
                r = device.op("decrypt", "sender" to senderIdentity, "message" to body)
            }
            if (!r.ok) {
                if (r.error in HOLD_CODES) held += id else drop(id, "decrypt:" + (r.error ?: "failed"))
                continue
            }
            if (r.json["kind"]?.jsonPrimitive?.contentOrNull != "message") {
                // A room key, a link or a story key: the engine has taken it; nothing to show.
                drop(id, "r5")
                continue
            }
            plaintext = r.string("plaintext")
        }

        val decoded = decodeEnvelope(plaintext!!, conversationId, now)
        val record = when (decoded) {
            is DecodedEnvelope.Newer -> {
                needUpdate++
                continue
            }
            is DecodedEnvelope.Ignore -> {
                drop(id, decoded.reason)
                continue
            }
            is DecodedEnvelope.Shown -> incoming(conversationId!!, decoded.id, senderUserId, senderDeviceId, decoded.ts, now, device).copy(text = decoded.text, file = decoded.file)
            is DecodedEnvelope.Op -> incoming(conversationId!!, decoded.id, senderUserId, senderDeviceId, decoded.ts, now, device).copy(op = decoded.op)
        }
        // Written to history before it is acknowledged: a crash here redelivers the item.
        if (history.put(record)) {
            received += record
            history.pin(record.conversationId)
            // Covers a delete that arrived before the message it deletes, too.
            history.eraseDeleted(record.conversationId)
        } else {
            dropped += "duplicate"
        }
        acks += id
    }
    acks.chunked(ChatLimits.ACK_BATCH).forEach { device.api.ack(device.deviceId, it) }
    return ReceiveReport(received, held, needUpdate, dropped)
}
