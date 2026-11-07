package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom

/**
 * The device-key safety number on Android (the same engine and rules as the desktop app): the QR
 * code, verifying by hand, and accepting a contact's changed keys. Real engine, in-memory server.
 */
class DeviceKeySafetyTest {
    @get:Rule val folder = TemporaryFolder()

    private lateinit var engine: EngineRunner
    private val server = FakeV3Server()
    private val alice = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val bob = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"

    @Before fun load() {
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't built for this computer", call != null)
        engine = EngineRunner(call!!)
    }

    private fun legacy() = ByteArray(32).also { SecureRandom().nextBytes(it) }
    private fun storage(name: String) = DeviceStorage(File(folder.root, name), SoftwareKeyWrapper())
    private suspend fun first(user: String, name: String) = setUpFirstDevice(user, legacy(), storage(name), engine, server.api(user))

    @Test fun `both sides see the same number, and scanning or comparing marks it verified`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        val atAlice = deviceKeySafety(a1, bob)!!
        val atBob = deviceKeySafety(b1, alice)!!
        assertEquals(atAlice.groups, atBob.groups)
        assertEquals(12, atAlice.groups.size)
        assertFalse(atAlice.verified)
        assertFalse(atAlice.keyChanged)
        assertTrue(atBob.qrText.startsWith("RVDK1:"))

        // Alice scans the code on Bob's screen; her own code (the wrong way round) doesn't count.
        assertFalse(verifyContactQr(a1, bob, atAlice.accountKey, atAlice.qrText))
        assertTrue(verifyContactQr(a1, bob, atAlice.accountKey, atBob.qrText))
        assertTrue(deviceKeySafety(a1, bob)!!.verified)

        // Bob compares digits instead, and can take it back.
        setContactVerified(b1, alice, atBob.accountKey, true)
        assertTrue(deviceKeySafety(b1, alice)!!.verified)
        setContactVerified(b1, alice, atBob.accountKey, false)
        assertFalse(deviceKeySafety(b1, alice)!!.verified)
    }

    @Test fun `a contact who starts over shows a changed number until the new keys are accepted`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        val before = deviceKeySafety(b1, alice)!!
        setContactVerified(b1, alice, before.accountKey, true)

        // Alice resets her account on a new phone: a new account key.
        server.dropDeviceList(alice)
        val a3 = first(alice, "a3")
        val changed = deviceKeySafety(b1, alice)!!
        assertTrue(changed.keyChanged)
        assertFalse(changed.verified)
        assertNotEquals(before.accountKey, changed.accountKey)
        assertNotEquals(before.groups, changed.groups)
        // Marking verified can't stand in for accepting the new key.
        assertThrows(EngineException::class.java) { setContactVerified(b1, alice, changed.accountKey, true) }

        // Bob scans the code on Alice's new phone: that accepts the new keys, verified.
        val code = deviceKeySafety(a3, bob)!!.qrText
        assertTrue(verifyContactQr(b1, alice, changed.accountKey, code))
        val accepted = deviceKeySafety(b1, alice)!!
        assertFalse(accepted.keyChanged)
        assertTrue(accepted.verified)
        assertEquals(changed.groups, accepted.groups)
        assertEquals(accepted.groups, deviceKeySafety(a3, bob)!!.groups)
    }

    @Test fun `new keys can be trusted without comparing, and then show as not verified`() = runBlocking {
        first(alice, "a1")
        val b1 = first(bob, "b1")
        deviceKeySafety(b1, alice)!!
        server.dropDeviceList(alice)
        first(alice, "a3")
        val changed = deviceKeySafety(b1, alice)!!
        acceptContactKey(b1, alice, changed.accountKey, verified = false)
        val after = deviceKeySafety(b1, alice)!!
        assertFalse(after.keyChanged)
        assertFalse(after.verified)
    }
}
