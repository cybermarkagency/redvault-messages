// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * The rollback counter of the stored device state (docs/signal-integration/07-step1-default-path.md section 4):
 * an older copy of the state file is refused, a state ahead of the counter is accepted, a deleted counter record is refused once the counter is in use (only a state from before the counter
 * starts one), and a counter edited in the file is caught. No engine needed: the state is opaque here.
 */
class DeviceStorageCounterTest {
    @get:Rule val folder = TemporaryFolder()

    private val user = "11111111-1111-4111-8111-111111111111"

    /** The Keystore stand-in, recording which aliases were used, and able to fail the counter's wrap once. */
    private class Recording : KeyWrapper {
        val inner = SoftwareKeyWrapper()
        val aliases = mutableSetOf<String>()
        var failCounterWrap = false

        override fun wrap(alias: String, secret: ByteArray, aad: ByteArray): KeyWrapper.Wrapped {
            aliases += alias
            if (failCounterWrap && alias.startsWith(DeviceStorage.COUNTER_ALIAS)) {
                failCounterWrap = false
                error("the app stopped here")
            }
            return inner.wrap(alias, secret, aad)
        }

        override fun unwrap(alias: String, wrapped: KeyWrapper.Wrapped, aad: ByteArray) = inner.unwrap(alias, wrapped, aad)
        override fun delete(alias: String) = inner.delete(alias)
        override fun protection(alias: String) = inner.protection(alias)
    }

    private val keys = Recording()
    private val dir get() = File(folder.root, "e2eV3")
    private val storage by lazy { DeviceStorage(dir, keys) }
    private val stateFile get() = File(dir, "v3-$user.json")
    private val counterFile get() = File(dir, "v3-counter-$user.json")

    private fun device(vararg states: String): OpenDevice {
        val opened = storage.create(user)
        for (s in states) storage.saveState(user, opened.deviceId, s)
        return opened
    }

    private fun edit(file: File, change: (MutableMap<String, kotlinx.serialization.json.JsonElement>) -> Unit) {
        val fields = Json.parseToJsonElement(file.readText()).jsonObject.toMutableMap()
        change(fields)
        file.writeText(JsonObject(fields).toString())
    }

    private fun restored() = storage.localEvents(user).any { it.kind == DeviceStorage.RESTORED_OLDER_COPY }

    @Test fun `the counter has its own Keystore alias and its own file`() {
        device("one")
        assertTrue("redvault.v3.counter.$user" in keys.aliases)
        assertTrue("redvault.v3.$user" in keys.aliases)
        assertTrue(counterFile.exists())
        assertFalse(counterFile.readText().contains("\"one\""))
        // The counter is inside the state file, next to the state, with its HMAC.
        val stored = Json.parseToJsonElement(stateFile.readText()).jsonObject
        assertEquals("1", (stored["counter"] as JsonPrimitive).content)
        assertNotNull(stored["mac"])
    }

    @Test fun `equal counters load`() {
        device("one", "two", "three")
        assertEquals("three", storage.load(user)!!.state)
        assertEquals("three", DeviceStorage(dir, keys).load(user)!!.state)
        assertFalse(restored())
    }

    @Test fun `an older copy of the state is refused and recorded`() {
        device("one")
        val old = stateFile.readText()
        val opened = storage.load(user)!!
        storage.saveState(user, opened.deviceId, "two")
        stateFile.writeText(old)

        assertNull(DeviceStorage(dir, keys).load(user))
        assertTrue(restored())
        // Refused every time, not only once.
        assertNull(DeviceStorage(dir, keys).load(user))
        storage.clearLocalEvents(user)
        assertFalse(restored())
    }

    @Test fun `a counter edited in the file is caught by its HMAC`() {
        device("one")
        val old = stateFile.readText()
        storage.saveState(user, storage.load(user)!!.deviceId, "two")
        stateFile.writeText(old)
        edit(stateFile) { it["counter"] = JsonPrimitive(99) }
        assertNull(DeviceStorage(dir, keys).load(user))
        assertTrue(restored())
    }

    @Test fun `a state changed without its HMAC is refused`() {
        device("one")
        edit(stateFile) { it["state"] = JsonPrimitive("someone else's") }
        assertNull(DeviceStorage(dir, keys).load(user))
        assertTrue(restored())
    }

    @Test fun `a state ahead of the counter is accepted and the counter raised`() {
        val opened = device("one")
        // The app stops between the two writes: the state is saved, the counter isn't.
        keys.failCounterWrap = true
        assertTrue(runCatching { storage.saveState(user, opened.deviceId, "two") }.isFailure)
        // The state went first.
        assertTrue(stateFile.readText().contains("\"two\""))

        val again = DeviceStorage(dir, keys)
        assertEquals("two", again.load(user)!!.state)
        assertFalse(restored())

        // The counter caught up: the copy from before "two" is now older and refused.
        val ahead = stateFile.readText()
        storage.saveState(user, opened.deviceId, "three")
        val three = stateFile.readText()
        stateFile.writeText(ahead)
        assertNull(DeviceStorage(dir, keys).load(user))
        stateFile.writeText(three)
        assertEquals("three", DeviceStorage(dir, keys).load(user)!!.state)
    }

    @Test fun `a deleted counter record is refused, so an older state can't come in with it`() {
        device("one")
        val old = stateFile.readText()
        storage.saveState(user, storage.load(user)!!.deviceId, "two")
        counterFile.delete()
        stateFile.writeText(old)

        assertNull(DeviceStorage(dir, keys).load(user))
        assertTrue(restored())
    }

    @Test fun `a deleted counter record is refused even with the newest state`() {
        device("one")
        storage.saveState(user, storage.load(user)!!.deviceId, "two")
        counterFile.delete()

        assertNull(DeviceStorage(dir, keys).load(user))
        assertTrue(restored())
    }

    @Test fun `a state saved before the counter existed loads, and its next save is counted`() {
        val opened = device("legacy")
        edit(stateFile) {
            it.remove("counter")
            it.remove("mac")
        }
        counterFile.delete()
        val legacy = stateFile.readText()

        assertEquals("legacy", DeviceStorage(dir, keys).load(user)!!.state)
        storage.saveState(user, opened.deviceId, "counted")
        assertEquals("counted", DeviceStorage(dir, keys).load(user)!!.state)
        // Putting the uncounted file back now is a rollback.
        stateFile.writeText(legacy)
        assertNull(DeviceStorage(dir, keys).load(user))
    }

    @Test fun `an unreadable counter record is refused, not skipped`() {
        device("one")
        edit(counterFile) { it["wrapped"] = JsonPrimitive("AAAA") }
        assertNull(DeviceStorage(dir, keys).load(user))
        assertTrue(restored())
    }

    @Test fun `a new device starts a new counter, and wipe removes it`() {
        device("one", "two", "three")
        val fresh = storage.create(user)
        storage.saveState(user, fresh.deviceId, "new")
        assertEquals("new", DeviceStorage(dir, keys).load(user)!!.state)
        assertFalse(restored())

        storage.wipe(user)
        assertFalse(counterFile.exists())
        assertFalse(stateFile.exists())
        assertEquals("none", keys.protection("redvault.v3.counter.$user"))
        assertNull(storage.load(user))
    }
}
