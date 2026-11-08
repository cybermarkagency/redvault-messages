// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.jsonArray
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom

/** The automatic "ensure device keys" check (docs/signal-integration/07-step1-default-path.md section 1). */
class EnsureDeviceKeysTest {
    @get:Rule val folder = TemporaryFolder()

    private val user = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val legacy = ByteArray(32).also { SecureRandom().nextBytes(it) }

    // --- The rule alone ---

    @Test fun `no signed list sets this phone up as the first device`() {
        assertEquals(EnsureStep.SetUpFirstDevice, ensureStep(hasEngine = true, enabled = true, listExists = false, linkedHere = false, trustedHere = false))
    }

    @Test fun `a trusted phone keeps Signal declared and topped up`() {
        assertEquals(EnsureStep.MaintainSignal, ensureStep(hasEngine = true, enabled = true, listExists = true, linkedHere = true, trustedHere = true))
    }

    @Test fun `a list without this phone is the banner, never an automatic link`() {
        assertEquals(EnsureStep.NeedsLink, ensureStep(hasEngine = true, enabled = true, listExists = true, linkedHere = false, trustedHere = false))
        assertEquals(EnsureStep.NeedsLink, ensureStep(hasEngine = true, enabled = true, listExists = true, linkedHere = true, trustedHere = false))
    }

    @Test fun `a failed or negative check does nothing`() {
        // The config check failed (offline, session not ready): never taken for "off", nothing is done either way.
        assertEquals(EnsureStep.Nothing, ensureStep(hasEngine = true, enabled = null, listExists = false, linkedHere = false, trustedHere = false))
        // The device list couldn't be read: not taken for "no list".
        assertEquals(EnsureStep.Nothing, ensureStep(hasEngine = true, enabled = true, listExists = null, linkedHere = false, trustedHere = false))
        assertEquals(EnsureStep.Nothing, ensureStep(hasEngine = true, enabled = false, listExists = false, linkedHere = false, trustedHere = false))
        assertEquals(EnsureStep.Nothing, ensureStep(hasEngine = false, enabled = true, listExists = false, linkedHere = false, trustedHere = false))
        // A phone that holds the account's device is never replaced because the server says there's no list.
        assertEquals(EnsureStep.Nothing, ensureStep(hasEngine = true, enabled = true, listExists = false, linkedHere = true, trustedHere = true))
    }

    // --- The check with the real engine and the in-memory server ---

    private fun engine(): EngineRunner {
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't built for this computer", call != null)
        return EngineRunner(call!!)
    }

    // One Keystore stand-in per phone, so a phone "restarted" here still has its keys.
    private val keystores = HashMap<String, SoftwareKeyWrapper>()

    private fun session(server: FakeV3Server, name: String, engine: EngineRunner?) =
        DeviceKeysSession(user, { legacy.copyOf() }, DeviceStorage(File(folder.root, name), keystores.getOrPut(name) { SoftwareKeyWrapper() }), engine, server.api(user))

    @Test fun `the first phone is set up without asking, with Signal and a recovery-key notice`() = runBlocking {
        val engine = engine()
        val server = FakeV3Server()
        val phone = session(server, "a", engine)
        assertFalse(phone.notice.value.recoveryKey)

        assertEquals(EnsureStep.SetUpFirstDevice, phone.ensure())
        val list = server.api(user).deviceList(user).deviceList
        assertNotNull(list)
        assertEquals(1, list!!["devices"]!!.jsonArray.size)
        assertNotNull(phone.chatDevice())
        assertTrue(phone.notice.value.recoveryKey)
        assertFalse(phone.notice.value.needsLink)
        if (com.redvaultsystems.redvaultmessage.BuildConfig.E2E_SIGNAL && engineHasSignal(phone.chatDevice()!!)) {
            // Signal is declared in the signed list right away.
            assertEquals(1, server.api(user).deviceList(user).deviceList!!["capabilities"]!!.jsonArray.size)
        }

        // The next run only keeps things in order; the notice stays until it's seen, across restarts.
        assertEquals(EnsureStep.MaintainSignal, phone.ensure())
        assertEquals(1, server.api(user).deviceList(user).deviceList!!["devices"]!!.jsonArray.size)
        val restarted = session(server, "a", engine)
        assertTrue(restarted.notice.value.recoveryKey)
        restarted.dismissRecoveryNotice()
        assertFalse(session(server, "a", engine).notice.value.recoveryKey)
    }

    @Test fun `another phone of the same account gets the banner, and nothing is linked`() = runBlocking {
        val engine = engine()
        val server = FakeV3Server()
        session(server, "a", engine).ensure()
        val before = server.api(user).deviceList(user).deviceList.toString()

        val second = session(server, "b", engine)
        assertEquals(EnsureStep.NeedsLink, second.ensure())
        assertTrue(second.notice.value.needsLink)
        assertNull(second.chatDevice())
        // The account's list is exactly what it was: a password alone never adds a device.
        assertEquals(before, server.api(user).deviceList(user).deviceList.toString())
    }

    @Test fun `a failed config check sets nothing up and is asked again next time`() = runBlocking {
        val engine = engine()
        val server = FakeV3Server()
        val phone = session(server, "a", engine)
        server.failEnabledOnce = true
        assertEquals(EnsureStep.Nothing, phone.ensure())
        assertNull(server.api(user).deviceList(user).deviceList)
        assertFalse(phone.notice.value.needsLink)
        // The next trigger asks again and does it.
        assertEquals(EnsureStep.SetUpFirstDevice, phone.ensure())
    }

    @Test fun `a build without the engine does nothing`() = runBlocking {
        val server = FakeV3Server()
        assertEquals(EnsureStep.Nothing, session(server, "a", null).ensure())
        assertNull(server.api(user).deviceList(user).deviceList)
    }

    @Test fun `a phone whose keys were put back from an older copy reads as not linked`() = runBlocking {
        val engine = engine()
        val server = FakeV3Server()
        session(server, "a", engine).ensure()
        val state = File(folder.root, "a/v3-$user.json")
        val old = state.readText()
        // More saves happen (a message, a key top-up), then the old file is put back.
        val phone = session(server, "a", engine)
        phone.chatDevice()!!.publishKeys()
        phone.ensure()
        state.writeText(old)

        val afterRestore = session(server, "a", engine)
        assertNull(afterRestore.chatDevice())
        assertEquals(EnsureStep.NeedsLink, afterRestore.ensure())
        assertTrue(afterRestore.notice.value.needsLink)
        assertTrue(DeviceStorage(File(folder.root, "a"), SoftwareKeyWrapper()).localEvents(user).any { it.kind == DeviceStorage.RESTORED_OLDER_COPY })
    }
}
