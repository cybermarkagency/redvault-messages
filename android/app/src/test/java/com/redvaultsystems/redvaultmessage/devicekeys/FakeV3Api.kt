package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * An in-memory stand-in for the protocol-3 endpoints (server/src/services/v3DeviceService.ts), like
 * ui/src/test/fakeV3Api.ts: compare-and-set on lists, mailboxes, pairing relay. The real policy is
 * tested in the server and in the end-to-end run. It records every request so tests can check nothing
 * private was sent.
 */
class FakeV3Server {
    val sent = mutableListOf<String>()
    private val devices = LinkedHashMap<String, JsonObject>()
    private val devices0 get() = devices
    private val claimed = HashMap<String, Int>()
    private val lists = HashMap<String, JsonObject>()
    private val backups = HashMap<String, String>()

    class Mail(val id: String, val toUser: String, val toDevice: String, val item: JsonObject)

    val mailbox = mutableListOf<Mail>()

    class Enrollment(val userId: String, val newDeviceId: String, val messages: MutableList<EnrollmentMessage> = mutableListOf())

    val enrollments = LinkedHashMap<String, Enrollment>()

    /** Active participants per conversation id (the real server knows them from protocol 1). */
    val participants = HashMap<String, List<String>>()

    /** Makes the next send answer `409 device_list_stale` once, as when a list changed after the sender read it. */
    var staleNext = false

    /** Makes the next send answer `409 mailbox_full`. */
    var failNext = false

    /** Makes the next device-list upload fail like a dropped connection. */
    var failListPut = false

    /** Makes the next "is protocol 3 on" question fail like a request before the session is ready. */
    var failEnabledOnce = false

    // rv-signal-v1 directory: "user/device" -> the device's upload, and its unused one-time keys.
    class SignalEntry(var upload: JsonObject, val preKeys: MutableList<JsonObject> = mutableListOf(), val kyberPreKeys: MutableList<JsonObject> = mutableListOf())

    val signal = HashMap<String, SignalEntry>()

    /** Whether /config says the Signal directory is on. */
    var signalOn = true

    /** A server that hides every device's Signal keys from claims. */
    var withholdSignal = false

    /** M6: conversation -> signed rosters, oldest first. Only the next epoch is accepted (the real checks are the server's). */
    val rosters = HashMap<String, MutableList<JsonObject>>()

    /** The account no longer lists any device, as after it was reset. */
    fun dropDeviceList(userId: String) {
        lists.remove(userId)
    }

    /** Rows deleted without being read, for the test to look at what the server holds. */
    fun pending(userId: String, deviceId: String) = mailbox.count { it.toUser == userId && it.toDevice == deviceId }

    /** The api one signed-in phone sees. */
    fun api(userId: String): V3Api = object : V3Api {
        private fun rec(text: String) {
            sent += text
        }

        private fun key(user: String, device: String) = "$user/$device"
        override suspend fun enabled(): Boolean {
            if (failEnabledOnce) {
                failEnabledOnce = false
                throw ApiException(401, "unauthorized", "not ready")
            }
            return true
        }

        override suspend fun registerDevice(upload: JsonObject) {
            rec(upload.toString())
            val id = upload["identity"]!!.jsonObject
            devices[key(id["userId"]!!.jsonPrimitive.content, id["deviceId"]!!.jsonPrimitive.content)] = upload
        }

        override suspend fun listDevices(): List<OwnDevice> {
            val listed = lists[userId]?.get("devices")?.jsonArray?.map { it.jsonObject["deviceId"]!!.jsonPrimitive.content }.orEmpty()
            return devices.values.map { it["identity"]!!.jsonObject }.filter { it["userId"]!!.jsonPrimitive.content == userId }.map {
                OwnDevice(it["deviceId"]!!.jsonPrimitive.content, it["ed25519"]!!.jsonPrimitive.content, "2026-10-26", it["deviceId"]!!.jsonPrimitive.content in listed)
            }
        }

        override suspend fun removeDevice(deviceId: String) {
            devices.remove(key(userId, deviceId))
        }

        override suspend fun deviceList(userId: String, conversationId: String?) = ListView(lists[userId], null)

        override suspend fun putDeviceList(deviceList: JsonObject, rotationProof: String?) {
            rec(deviceList.toString())
            if (failListPut) {
                failListPut = false
                throw ApiException(0, "network_error", "offline")
            }
            val owner = deviceList["userId"]!!.jsonPrimitive.content
            val current = lists[owner]
            if (current != null && deviceList["version"]!!.jsonPrimitive.content.toInt() != current["version"]!!.jsonPrimitive.content.toInt() + 1) {
                throw ApiException(409, "stale_device_list", "stale")
            }
            lists[owner] = deviceList
        }

        override suspend fun cancelReset(signature: String) {}

        override suspend fun createEnrollment(newDeviceId: String): Enrollment0 {
            val id = UUID.randomUUID().toString()
            enrollments[id] = Enrollment(userId, newDeviceId)
            return Enrollment0(id, newDeviceId)
        }

        override suspend fun enrollments(): List<Enrollment0> = enrollments.filter { it.value.userId == userId }.map { Enrollment0(it.key, it.value.newDeviceId) }

        override suspend fun postEnrollmentMessage(id: String, fromDeviceId: String, body: String) {
            rec(body)
            val e = enrollments[id] ?: throw ApiException(404, "not_found", "no such pairing")
            e.messages += EnrollmentMessage(e.messages.size + 1, fromDeviceId, body)
        }

        override suspend fun enrollmentMessages(id: String, after: Int): List<EnrollmentMessage> {
            val e = enrollments[id] ?: throw ApiException(404, "not_found", "no such pairing")
            return e.messages.filter { it.seq > after }
        }

        override suspend fun deleteEnrollment(id: String) {
            enrollments.remove(id)
        }

        override suspend fun sendMessages(senderDeviceId: String, conversationId: String?, messages: List<Triple<String, String, String>>) {
            rec(messages.joinToString { it.third })
            if (failNext) {
                failNext = false
                throw ApiException(409, "mailbox_full", "full")
            }
            if (staleNext) {
                staleNext = false
                throw ApiException(409, "device_list_stale", "stale")
            }
            val listedHere = lists[userId]?.get("devices")?.jsonArray?.any { it.jsonObject["deviceId"]!!.jsonPrimitive.content == senderDeviceId } == true
            if (!listedHere) throw ApiException(409, "device_not_listed", "not listed")
            for ((toUser, toDevice, body) in messages) {
                val id = UUID.randomUUID().toString()
                mailbox += Mail(
                    id, toUser, toDevice,
                    kotlinx.serialization.json.buildJsonObject {
                        put("id", kotlinx.serialization.json.JsonPrimitive(id))
                        put("senderUserId", kotlinx.serialization.json.JsonPrimitive(userId))
                        put("senderDeviceId", kotlinx.serialization.json.JsonPrimitive(senderDeviceId))
                        put("conversationId", conversationId?.let { kotlinx.serialization.json.JsonPrimitive(it) } ?: kotlinx.serialization.json.JsonNull)
                        put("body", kotlinx.serialization.json.JsonPrimitive(body))
                        put("createdOn", kotlinx.serialization.json.JsonPrimitive(java.time.LocalDate.now(java.time.ZoneOffset.UTC).toString()))
                    },
                )
            }
        }

        override suspend fun inbox(deviceId: String): List<JsonObject> = mailbox.filter { it.toUser == userId && it.toDevice == deviceId }.map { it.item }

        override suspend fun ack(deviceId: String, ids: List<String>) {
            mailbox.removeAll { it.toUser == userId && it.toDevice == deviceId && it.id in ids }
        }

        override suspend fun putRecoveryBackup(backup: String, signature: String) {
            rec(backup)
            backups[userId] = backup
        }

        override suspend fun claimKeys(conversationId: String?, devices: List<Pair<String, String>>): List<JsonObject> = devices.map { (u, d) ->
            val upload = devices0[key(u, d)] ?: throw ApiException(404, "not_found", "no such device")
            val ones = upload["oneTimeKeys"]!!.jsonArray
            val used = claimed.getOrPut(key(u, d)) { 0 }
            val one = ones.getOrNull(used)
            if (one != null) claimed[key(u, d)] = used + 1
            kotlinx.serialization.json.buildJsonObject {
                put("userId", kotlinx.serialization.json.JsonPrimitive(u))
                put("deviceId", kotlinx.serialization.json.JsonPrimitive(d))
                put("oneTimeKey", one ?: kotlinx.serialization.json.JsonNull)
                put("fallbackKey", if (one != null) kotlinx.serialization.json.JsonNull else (upload["fallbackKey"] ?: kotlinx.serialization.json.JsonNull))
            }
        }

        override suspend fun conversationDeviceLists(conversationId: String): List<Pair<String, ListView>> {
            val members = participants[conversationId] ?: throw ApiException(403, "forbidden", "not a participant")
            return members.map { it to ListView(lists[it], null) }
        }

        override suspend fun sendGroupMessage(senderDeviceId: String, conversationId: String, body: String, recipients: List<Pair<String, String>>) {
            sendMessages(senderDeviceId, conversationId, recipients.map { Triple(it.first, it.second, body) })
        }

        override suspend fun recoveryBackup(): String? = backups[userId]
        override suspend fun events(after: Int): List<V3Event> = emptyList()

        override suspend fun signalEnabled(): Boolean = signalOn

        override suspend fun rosters(conversationId: String, after: Int): List<JsonObject> =
            rosters[conversationId].orEmpty().filter { it["epoch"]!!.jsonPrimitive.content.toInt() > after }

        override suspend fun postRoster(conversationId: String, roster: JsonObject) {
            val chain = rosters.getOrPut(conversationId) { mutableListOf() }
            if (roster["epoch"]!!.jsonPrimitive.content.toInt() != chain.size + 1) throw ApiException(409, "stale_roster", "stale")
            chain += roster
        }

        private fun counts(e: SignalEntry?) = kotlinx.serialization.json.buildJsonObject {
            put("enabled", kotlinx.serialization.json.JsonPrimitive(e != null))
            put("preKeys", kotlinx.serialization.json.JsonPrimitive(e?.preKeys?.size ?: 0))
            put("kyberPreKeys", kotlinx.serialization.json.JsonPrimitive(e?.kyberPreKeys?.size ?: 0))
        }

        override suspend fun signalUpload(deviceId: String, upload: JsonObject): JsonObject {
            rec(upload.toString())
            val e = signal.getOrPut(key(userId, deviceId)) { SignalEntry(upload) }
            e.upload = upload
            e.preKeys += upload["preKeys"]!!.jsonArray.map { it.jsonObject }
            e.kyberPreKeys += upload["kyberPreKeys"]!!.jsonArray.map { it.jsonObject }
            return counts(e)
        }

        override suspend fun signalStatus(deviceId: String): JsonObject = counts(signal[key(userId, deviceId)])

        override suspend fun signalClaim(conversationId: String?, devices: List<Pair<String, String>>): List<JsonObject> = devices.map { (u, d) ->
            val e = signal[key(u, d)].takeUnless { withholdSignal }
            kotlinx.serialization.json.buildJsonObject {
                put("userId", kotlinx.serialization.json.JsonPrimitive(u))
                put("deviceId", kotlinx.serialization.json.JsonPrimitive(d))
                if (e == null) {
                    put("certificate", kotlinx.serialization.json.JsonNull)
                    put("bundle", kotlinx.serialization.json.JsonNull)
                } else {
                    val pre = e.preKeys.removeFirstOrNull()
                    val kyber = e.kyberPreKeys.removeFirstOrNull()
                    put("certificate", e.upload["certificate"]!!)
                    put("bundle", kotlinx.serialization.json.buildJsonObject {
                        put("registrationId", e.upload["registrationId"]!!)
                        put("identityKey", e.upload["identityKey"]!!)
                        put("signedPreKey", e.upload["signedPreKey"]!!)
                        put("kyberPreKey", kyber ?: e.upload["lastResortKyberPreKey"]!!)
                        put("lastResort", kotlinx.serialization.json.JsonPrimitive(kyber == null))
                        put("preKey", pre ?: kotlinx.serialization.json.JsonNull)
                    })
                }
            }
        }
    }
}

private typealias Enrollment0 = com.redvaultsystems.redvaultmessage.devicekeys.Enrollment

/** AES-GCM with a key held in memory, standing in for the Keystore on the JVM. */
class SoftwareKeyWrapper : KeyWrapper {
    private val keys = HashMap<String, SecretKeySpec>()

    override fun wrap(alias: String, secret: ByteArray, aad: ByteArray): KeyWrapper.Wrapped {
        val key = keys.getOrPut(alias) { SecretKeySpec(ByteArray(32).also { java.security.SecureRandom().nextBytes(it) }, "AES") }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key)
        cipher.updateAAD(aad)
        return KeyWrapper.Wrapped(cipher.iv, cipher.doFinal(secret))
    }

    override fun unwrap(alias: String, wrapped: KeyWrapper.Wrapped, aad: ByteArray): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, keys[alias] ?: error("gone"), GCMParameterSpec(128, wrapped.iv))
        cipher.updateAAD(aad)
        return cipher.doFinal(wrapped.ciphertext)
    }

    override fun delete(alias: String) {
        keys.remove(alias)
    }

    override fun protection(alias: String) = if (alias in keys) "software" else "none"
}
