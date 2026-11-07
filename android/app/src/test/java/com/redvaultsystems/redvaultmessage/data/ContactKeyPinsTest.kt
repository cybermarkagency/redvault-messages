package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Trust on first use for contacts' identity keys (data/ContactKeyPins.kt), with the desktop's rules: the first key
 * is pinned, a different one is flagged until it's verified, and "verified" belongs to a key, not a contact.
 */
class ContactKeyPinsTest {
    private class MapStorage : ContactKeyPins.Storage {
        val values = HashMap<String, String>()
        var writes = 0
        override fun get(name: String): String? = values[name]
        override fun put(name: String, value: String) {
            writes++
            values[name] = value
        }
    }

    private val storage = MapStorage()
    private val pins = ContactKeyPins(storage)

    private val me = "self-1"
    private val alice = "alice"
    private val oldKey = key(1)
    private val newKey = key(2)

    private fun key(seed: Int) = Base64Codec.encode(ByteArray(32) { (it * 31 + seed).toByte() })

    @Test
    fun theFirstKeySeenIsPinnedAndNotFlagged() {
        assertFalse(pins.keyChanged(me, alice, oldKey, verified = false))
        assertEquals(oldKey, storage.values["$me:$alice"])
        assertFalse(pins.keyChanged(me, alice, oldKey, verified = false))
    }

    @Test
    fun aDifferentKeyIsFlaggedAndNeverAcceptedSilently() {
        pins.keyChanged(me, alice, oldKey, verified = false)
        assertTrue(pins.keyChanged(me, alice, newKey, verified = false))
        // Every later check (each refresh) still says so: the pin isn't moved to the new key.
        repeat(3) { assertTrue(pins.keyChanged(me, alice, newKey, verified = false)) }
        assertEquals(oldKey, storage.values["$me:$alice"])
    }

    @Test
    fun acceptingTheNewKeyClearsTheWarning() {
        pins.keyChanged(me, alice, oldKey, verified = false)
        assertTrue(pins.keyChanged(me, alice, newKey, verified = false))
        pins.accept(me, alice, newKey)
        assertFalse(pins.keyChanged(me, alice, newKey, verified = false))
        // Going back to the old key is a change too.
        assertTrue(pins.keyChanged(me, alice, oldKey, verified = false))
    }

    @Test
    fun aVerifiedKeyIsNeverFlaggedAndIsPinned() {
        pins.keyChanged(me, alice, oldKey, verified = false)
        // A key verified on this phone is trusted even if the pin is older (the desktop's early return)...
        assertFalse(pins.keyChanged(me, alice, newKey, verified = true))
        // ...and becomes the pin, so marking it not verified afterwards doesn't bring a warning back.
        assertFalse(pins.keyChanged(me, alice, newKey, verified = false))
    }

    @Test
    fun aContactVerifiedBeforePinsExistedIsStillCaughtWhenTheirKeyChanges() {
        // An upgrade: a verified mark for the old key, no pin yet. Seeing it verified pins it.
        assertNull(storage.values["$me:$alice"])
        assertFalse(pins.keyChanged(me, alice, oldKey, verified = true))
        // Verified marks are per key, so the new key isn't verified: it must be flagged, not pinned as a "first" key.
        assertTrue(pins.keyChanged(me, alice, newKey, verified = false))
    }

    /** The whole desktop flow, with verified marks kept per key like VerifiedContactsStore. */
    @Test
    fun verifiedFollowsTheKeyThroughAChange() {
        val verifiedKeys = HashSet<String>()
        fun check(key: String) = pins.keyChanged(me, alice, key, verified = key in verifiedKeys)
        fun markVerified(key: String, verified: Boolean) {
            if (verified) verifiedKeys += key else verifiedKeys -= key
            if (verified) pins.accept(me, alice, key)
        }

        assertFalse(check(oldKey))
        markVerified(oldKey, true)
        assertFalse(check(oldKey))

        // Their key changes: not verified any more (the mark was for the old key), and flagged.
        assertFalse(newKey in verifiedKeys)
        assertTrue(check(newKey))

        // Comparing the new number (or a QR match) marks it verified: the warning goes.
        markVerified(newKey, true)
        assertFalse(check(newKey))

        // "Mark as not verified" afterwards just removes the mark; the new key stays accepted.
        markVerified(newKey, false)
        assertFalse(check(newKey))
    }

    @Test
    fun pinsAreKeptPerAccountAndPerContact() {
        pins.keyChanged(me, alice, oldKey, verified = false)
        assertFalse(pins.keyChanged(me, "bob", newKey, verified = false))
        assertFalse(pins.keyChanged("self-2", alice, newKey, verified = false))
        assertTrue(pins.keyChanged(me, alice, newKey, verified = false))
        assertFalse(pins.keyChanged("self-2", alice, newKey, verified = false))
    }

    @Test
    fun theSameBytesWrittenDifferentlyAreNotAChange() {
        pins.keyChanged(me, alice, oldKey, verified = false)
        val unpadded = oldKey.trimEnd('=')
        assertFalse(pins.keyChanged(me, alice, "$unpadded\n", verified = false))
    }

    @Test
    fun aMissingKeyIsNeitherPinnedNorFlagged() {
        assertFalse(pins.keyChanged(me, alice, "", verified = false))
        pins.accept(me, alice, "")
        assertTrue(storage.values.isEmpty())
        // The first real key is still treated as the first.
        assertFalse(pins.keyChanged(me, alice, oldKey, verified = false))
    }

    @Test
    fun checkingAnUnchangedKeyDoesntWriteEveryRefresh() {
        pins.keyChanged(me, alice, oldKey, verified = true)
        val writes = storage.writes
        repeat(5) {
            pins.keyChanged(me, alice, oldKey, verified = true)
            pins.keyChanged(me, alice, oldKey, verified = false)
        }
        assertEquals(writes, storage.writes)
    }
}
