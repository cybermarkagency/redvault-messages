// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom
import java.util.Base64

/**
 * The per-device-keys flow on the JVM: the real engine (crypto-core, built for this computer by the
 * buildE2eeHostLib task and found through -Drv.lib) with an in-memory server. Skipped, not failed, where
 * the engine can't be built. The real server comes in the end-to-end run (docs/multi-device-threat-model.md).
 */
class DeviceKeysTest {
    @get:Rule val folder = TemporaryFolder()

    private lateinit var engine: EngineRunner
    private val user = "11111111-1111-4111-8111-111111111111"
    private val legacy = ByteArray(32).also { SecureRandom().nextBytes(it) }

    @Before fun loadEngine() {
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't built for this computer", call != null)
        engine = EngineRunner(call!!)
    }

    private fun phone(name: String) = DeviceStorage(File(folder.root, name), SoftwareKeyWrapper())

    @Test fun `the engine answers`() {
        assertTrue(engine.must("version").json.containsKey("engine"))
    }

    @Test fun `first device is set up, listed, and its storage key is not on disk`() = runBlocking {
        val server = FakeV3Server()
        val storage = phone("a")
        val device = setUpFirstDevice(user, legacy, storage, engine, server.api(user))
        assertEquals(1, server.api(user).deviceList(user).deviceList!!["devices"]!!.jsonArray.size)
        assertEquals(listOf(device.deviceId), server.api(user).listDevices().filter { it.listed }.map { it.deviceId })
        assertTrue(device.op("deviceList").bool("trusted") == true)

        // Reopened from storage it is the same device.
        val again = V3Device.open(user, storage, engine, server.api(user))!!
        assertEquals(device.deviceId, again.deviceId)

        // The wrapped storage key, the legacy key and the state's plaintext identity keys are not in the file as such.
        val text = File(folder.root, "a").listFiles()!!.joinToString { it.readText() }
        val opened = storage.load(user)!!
        assertFalse(text.contains(opened.storageKey))
        assertFalse(text.contains(Base64.getEncoder().encodeToString(legacy)))
        // Nothing private went to the server.
        val secrets = listOf(opened.storageKey, Base64.getEncoder().encodeToString(legacy))
        for (s in secrets) assertTrue(server.sent.none { it.contains(s) })
    }

    @Test fun `storage refuses a swapped or wiped device`() {
        val storage = phone("b")
        val opened = storage.create(user)
        storage.saveState(user, opened.deviceId, "state")
        assertEquals("state", storage.load(user)!!.state)
        val error = runCatching { storage.saveState(user, "other-device", "x") }.exceptionOrNull()
        assertNotNull(error)
        storage.wipe(user)
        assertNull(storage.load(user))
    }

    @Test fun `recovery key formats and parses`() {
        val key = ByteArray(32) { it.toByte() }
        val shown = formatRecoveryKey(key)
        assertEquals(64 + 15, shown.length)
        assertTrue(parseRecoveryKey(shown.lowercase())!!.contentEquals(key))
        assertNull(parseRecoveryKey("1234"))
    }

    @Test fun `a recovery key restores the account on a new phone`() = runBlocking {
        val server = FakeV3Server()
        val first = setUpFirstDevice(user, legacy, phone("first"), engine, server.api(user))
        val key = createRecoveryBackup(first)
        assertEquals(79, key.length)
        // The recovery key itself never reached the server.
        assertTrue(server.sent.none { it.contains(key) })

        val restored = restoreWithRecoveryKey(user, key, phone("second"), engine, server.api(user))
        assertTrue(restored.op("deviceList").bool("trusted") == true)
        assertEquals(2, server.api(user).deviceList(user).deviceList!!["devices"]!!.jsonArray.size)

        val wrong = runCatching { restoreWithRecoveryKey(user, formatRecoveryKey(ByteArray(32)), phone("third"), engine, server.api(user)) }.exceptionOrNull()
        assertNotNull(wrong)
        // The failed restore left no device behind on the third phone, nor in the directory.
        assertEquals(2, server.api(user).listDevices().size)
    }

    @Test fun `pairing by comparing codes links the new phone, and revoking removes it`() = runBlocking {
        val server = FakeV3Server()
        val api = server.api(user)
        val trusted = setUpFirstDevice(user, legacy, phone("trusted"), engine, api)

        val joining = startNewDevice(user, phone("new"), engine, api)
        assertEquals(1, api.listDevices().count { !it.listed })
        val fast = WaitOptions(intervalMs = 5, timeoutMs = 20_000)

        val newSide = NewDevicePairing.begin(joining, engine)
        val request = api.enrollments().single()
        assertEquals(joining.deviceId, request.newDeviceId)
        val results = listOf(
            async(kotlinx.coroutines.Dispatchers.Default) { newSide.codes(fast) },
            async(kotlinx.coroutines.Dispatchers.Default) { TrustedPairing.begin(trusted, engine, request.id, fast) },
        ).awaitAll()
        @Suppress("UNCHECKED_CAST") val newCodes = results[0] as List<Int>
        val answered = results[1] as TrustedPairing.Begun
        assertEquals(newCodes, answered.codes)
        assertTrue(newCodes.isNotEmpty())

        val lists = listOf(
            async(kotlinx.coroutines.Dispatchers.Default) { newSide.confirm(fast) },
            async(kotlinx.coroutines.Dispatchers.Default) { answered.pairing.confirm(fast) },
        ).awaitAll()
        assertEquals(2, lists[0].getValue("devices").jsonArray.size)
        assertEquals(2, lists[1].getValue("devices").jsonArray.size)
        assertTrue(joining.op("deviceList").bool("trusted") == true)
        assertEquals(2, api.listDevices().count { it.listed })

        revokeDevice(trusted, joining.deviceId)
        assertEquals(listOf(trusted.deviceId), api.deviceList(user).deviceList!!["devices"]!!.jsonArray.map { it.jsonObject["deviceId"]!!.jsonPrimitive.content })
    }

    @Test fun `a revoke that never reached the server is undone, so the retry works`() = runBlocking {
        val server = FakeV3Server()
        val api = server.api(user)
        val first = setUpFirstDevice(user, legacy, phone("r1"), engine, api)
        val key = createRecoveryBackup(first)
        val second = restoreWithRecoveryKey(user, key, phone("r2"), engine, api)
        assertEquals(2, api.deviceList(user).deviceList!!["devices"]!!.jsonArray.size)

        // The first device reads the server's list (it was changed by the second one), as the screen does on load.
        first.must("verifyDeviceList", "deviceList" to api.deviceList(user).deviceList)
        server.failListPut = true
        val error = runCatching { revokeDevice(first, second.deviceId) }.exceptionOrNull()
        assertNotNull(error)
        // The device's own list still has both: nothing ran ahead of the server.
        assertEquals(2, first.must("deviceList").obj("deviceList")["devices"]!!.jsonArray.size)

        revokeDevice(first, second.deviceId)
        assertEquals(listOf(first.deviceId), api.deviceList(user).deviceList!!["devices"]!!.jsonArray.map { it.jsonObject["deviceId"]!!.jsonPrimitive.content })
    }

    @Test fun `a QR code links a new phone to a trusted one without comparing numbers`() = runBlocking {
        val server = FakeV3Server()
        val api = server.api(user)
        val trusted = setUpFirstDevice(user, legacy, phone("qt"), engine, api)
        val joining = startNewDevice(user, phone("qn"), engine, api)

        val code = qrLinkOffer(joining)
        assertTrue(looksLikeLinkOffer(code))
        // The trusted phone scans it; the new phone picks up the link message.
        val list = approveScannedQr(trusted, code)
        assertEquals(2, list.getValue("devices").jsonArray.size)
        val linked = waitForQrLink(joining, WaitOptions(intervalMs = 5, timeoutMs = 20_000))
        assertEquals(2, linked.getValue("devices").jsonArray.size)
        assertTrue(joining.op("deviceList").bool("trusted") == true)
        assertEquals(2, api.listDevices().count { it.listed })
        // Nothing secret about the code was ever sent to the server: only the link message, as ciphertext.
        assertTrue(server.sent.none { it.contains(code) })
    }

    @Test fun `a QR code for another account, or not a link code, is refused`() = runBlocking {
        val server = FakeV3Server()
        val other = "99999999-9999-4999-8999-999999999999"
        val trusted = setUpFirstDevice(user, legacy, phone("t1"), engine, server.api(user))
        val stranger = setUpFirstDevice(other, legacy, phone("t2"), engine, server.api(other))
        val joiningOther = startNewDevice(other, phone("n2"), engine, server.api(other))
        val foreign = qrLinkOffer(joiningOther)
        val wrongAccount = runCatching { approveScannedQr(trusted, foreign) }.exceptionOrNull()
        assertTrue(wrongAccount is EngineException && wrongAccount.code == "wrong_account")
        val notACode = runCatching { approveScannedQr(trusted, "hello") }.exceptionOrNull()
        assertTrue(notACode is EngineException && notACode.code == "not_a_link_code")
        assertNotNull(stranger)
        // Nothing was added to the trusted account's list.
        assertEquals(1, server.api(user).deviceList(user).deviceList!!["devices"]!!.jsonArray.size)
    }

    @Test fun `a relay that swaps a key makes the codes differ`() = runBlocking {
        val server = FakeV3Server()
        val api = server.api(user)
        val trusted = setUpFirstDevice(user, legacy, phone("trusted2"), engine, api)
        val joining = startNewDevice(user, phone("new2"), engine, api)
        val fast = WaitOptions(intervalMs = 5, timeoutMs = 20_000)
        val newSide = NewDevicePairing.begin(joining, engine)
        val request = api.enrollments().single()

        // The relay answers the trusted side with a different commitment's device: it replaces the new phone's
        // identity in the commit message with the trusted phone's own.
        val enrollment = server.enrollments.getValue(request.id)
        val commit = enrollment.messages.first()
        val forged = kotlinx.serialization.json.buildJsonObject {
            for ((k, v) in kotlinx.serialization.json.Json.parseToJsonElement(commit.body).jsonObject) put(k, if (k == "identity") trusted.identity() else v)
        }
        enrollment.messages[0] = commit.copy(body = forged.toString())

        val results = listOf(
            async(kotlinx.coroutines.Dispatchers.Default) { runCatching { newSide.codes(fast) } },
            async(kotlinx.coroutines.Dispatchers.Default) { runCatching { TrustedPairing.begin(trusted, engine, request.id, fast) } },
        ).awaitAll()
        val newCodes = (results[0] as Result<*>).getOrNull() as List<*>?
        val trustedBegun = (results[1] as Result<*>).getOrNull() as TrustedPairing.Begun?
        // Either an engine refusal, or two different sets of numbers: never a silent match.
        if (newCodes != null && trustedBegun != null) assertTrue(newCodes != trustedBegun.codes)
    }
}
