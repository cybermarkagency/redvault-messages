package com.redvaultsystems.redvaultmessage.stories

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.crypto.StoryCryptoException
import com.redvaultsystems.redvaultmessage.data.model.StoryPrefs
import com.redvaultsystems.redvaultmessage.data.model.StoryPrefsParser
import kotlinx.serialization.json.Json
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The story crypto against the vectors the web app's own code made (resources/vectors/story_vectors.json, from
 * desktop/tests/vectors/make_story_vectors.ts): the sealed key a web author made for a viewer opens here, and the
 * details this code seals are the very bytes the web sealed. Runs on the JVM with the desktop build of libsodium;
 * androidTest/.../StoryCryptoDeviceTest.kt runs the same checks through lazysodium-android on a device.
 */
class StoryCryptoVectorsTest {
    private val crypto = StoryCrypto(TestSodium.sodium)
    private val v = vectors("story_vectors.json")
    private val alice = v.obj("alice")
    private val bob = v.obj("bob")
    private val storyId = v.str("storyId")
    private val key = v.b64("key")
    private val wrapped = v.b64("wrappedForBob")

    private fun flipLast(bytes: ByteArray) = bytes.copyOf().also { it[it.size - 1] = (it[it.size - 1].toInt() xor 1).toByte() }

    @Test
    fun openAKeyTheWebSealedForTheViewer() {
        assertArrayEquals(key, crypto.unwrapStoryKey(storyId, wrapped, alice.b64("publicKey"), bob.b64("privateKey")))
        assertEquals(93, wrapped.size)
    }

    @Test
    fun theSealedKeyIsBoundToItsStoryItsAuthorAndItsViewer() {
        val otherStory = "0e9d8c7b-6a5f-4e3d-8c2b-1a0f9e8d7c6b"
        assertThrows(StoryCryptoException::class.java) { crypto.unwrapStoryKey(otherStory, wrapped, alice.b64("publicKey"), bob.b64("privateKey")) }
        // Not made by this author.
        assertThrows(StoryCryptoException::class.java) { crypto.unwrapStoryKey(storyId, wrapped, bob.b64("publicKey"), bob.b64("privateKey")) }
        // Not for this viewer.
        assertThrows(StoryCryptoException::class.java) { crypto.unwrapStoryKey(storyId, wrapped, alice.b64("publicKey"), alice.b64("privateKey")) }
        // A changed byte, a short copy, a long one.
        for (damaged in listOf(flipLast(wrapped), wrapped.copyOf(92), wrapped + byteArrayOf(0))) {
            assertThrows(StoryCryptoException::class.java) { crypto.unwrapStoryKey(storyId, damaged, alice.b64("publicKey"), bob.b64("privateKey")) }
        }
        assertThrows(StoryCryptoException::class.java) { crypto.unwrapStoryKey("not-a-story-id", wrapped, alice.b64("publicKey"), bob.b64("privateKey")) }
    }

    @Test
    fun aKeyBoxWithTheWrongMagicOrVersionIsRefusedEvenThoughItOpens() {
        val sodium = TestSodium.sodium
        fun boxed(payload: ByteArray): ByteArray {
            val nonce = ByteArray(24) { it.toByte() }
            val box = ByteArray(payload.size + 16)
            assertTrue(sodium.cryptoBoxEasy(box, payload, payload.size.toLong(), nonce, bob.b64("publicKey"), alice.b64("privateKey")))
            return nonce + box
        }
        val good = crypto.storyKeyPayload(storyId, key)
        // The intact payload, boxed this way, opens: so what follows is refused for its content, not by the box.
        assertArrayEquals(key, crypto.unwrapStoryKey(storyId, boxed(good), alice.b64("publicKey"), bob.b64("privateKey")))
        val wrongMagic = good.copyOf().also { it[0] = 'X'.code.toByte() }
        val wrongVersion = good.copyOf().also { it[4] = 2 }
        for (payload in listOf(wrongMagic, wrongVersion)) {
            assertThrows(StoryCryptoException::class.java) { crypto.unwrapStoryKey(storyId, boxed(payload), alice.b64("publicKey"), bob.b64("privateKey")) }
        }
    }

    @Test
    fun theKeyPayloadHasTheDocumentedLayout() {
        val payload = crypto.storyKeyPayload(storyId, key)
        assertEquals(53, payload.size)
        assertEquals("RVST", String(payload.copyOfRange(0, 4), Charsets.US_ASCII))
        assertEquals(1, payload[4].toInt())
        assertEquals("5c1d0f7e2b3a4c8d9e6fa1b2c3d4e5f6", payload.copyOfRange(5, 21).joinToString("") { "%02x".format(it) })
        assertArrayEquals(key, payload.copyOfRange(21, 53))
        assertThrows(StoryCryptoException::class.java) { crypto.storyKeyPayload(storyId, ByteArray(31)) }
    }

    @Test
    fun aKeyWrappedHereOpensHereAndIsTheSameSize() {
        val id = crypto.newStoryId()
        val k = crypto.newStoryKey()
        val sealed = crypto.wrapStoryKey(id, k, bob.b64("publicKey"), alice.b64("privateKey"))
        assertEquals(93, sealed.size)
        assertArrayEquals(k, crypto.unwrapStoryKey(id, sealed, alice.b64("publicKey"), bob.b64("privateKey")))
        // Two wraps of one key differ (a fresh nonce each).
        assertNotEquals(sealed.toList(), crypto.wrapStoryKey(id, k, bob.b64("publicKey"), alice.b64("privateKey")).toList())
    }

    @Test
    fun newIdsAreVersion4UuidsAndNewKeysAre32Bytes() {
        val shape = Regex("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")
        repeat(20) {
            val id = crypto.newStoryId()
            assertTrue(id, shape.matches(id))
            assertEquals(32, crypto.newStoryKey().size)
        }
    }

    @Test
    fun openTheDetailsTheWebSealed() {
        val json = crypto.openStoryMeta(storyId, key, v.b64("metaCiphertext"), v.b64("metaNonce"))
        assertEquals(v.str("metaJson"), json)
    }

    @Test
    fun sealTheSameDetailsWithTheSameNonceToTheWebsBytes() {
        // The additional data (label + story id) is part of the ciphertext: equal bytes means equal binding.
        val sealed = crypto.sealStoryMeta(storyId, key, v.str("metaJson"), v.b64("metaNonce"))
        assertArrayEquals(v.b64("metaCiphertext"), sealed.ciphertext)
        assertArrayEquals(v.b64("metaNonce"), sealed.nonce)
    }

    @Test
    fun detailsAreBoundToTheirStoryAndKey() {
        val ct = v.b64("metaCiphertext")
        val nonce = v.b64("metaNonce")
        assertThrows(StoryCryptoException::class.java) { crypto.openStoryMeta("0e9d8c7b-6a5f-4e3d-8c2b-1a0f9e8d7c6b", key, ct, nonce) }
        assertThrows(StoryCryptoException::class.java) { crypto.openStoryMeta(storyId, ByteArray(32) { 9 }, ct, nonce) }
        assertThrows(StoryCryptoException::class.java) { crypto.openStoryMeta(storyId, key, ct.copyOf().also { it[0] = (it[0].toInt() xor 1).toByte() }, nonce) }
        assertThrows(StoryCryptoException::class.java) { crypto.openStoryMeta(storyId, key, ct.copyOf(10), nonce) }
        assertThrows(StoryCryptoException::class.java) { crypto.openStoryMeta(storyId, key, ct, nonce.copyOf(23)) }
    }

    @Test
    fun openTheSettingsTheWebSealed() {
        val json = crypto.openStoryPrefs(v.b64("prefsCiphertext"), v.b64("prefsNonce"), alice.b64("privateKey"))
        val prefs = StoryPrefsParser.parse(Json.parseToJsonElement(json))
        assertEquals(setOf("00000000-0000-4000-8000-00000000000b"), prefs.closeFriends)
        assertEquals(setOf("00000000-0000-4000-8000-00000000000c"), prefs.hideFrom)
        assertTrue(prefs.muted.isEmpty())
        assertEquals(false, prefs.shareViews)
        assertEquals(true, prefs.allowReplies)
        // Another account's key opens nothing.
        assertThrows(StoryCryptoException::class.java) { crypto.openStoryPrefs(v.b64("prefsCiphertext"), v.b64("prefsNonce"), bob.b64("privateKey")) }
    }

    @Test
    fun settingsSealedHereAreTheWebsBytesForTheSameNonce() {
        val prefs = StoryPrefs(
            linkedSetOf("00000000-0000-4000-8000-00000000000b"), linkedSetOf("00000000-0000-4000-8000-00000000000c"), emptySet(),
            shareViews = false, allowReplies = true,
        )
        val json = StoryPrefsParser.toJson(prefs)
        assertEquals("{\"closeFriends\":[\"00000000-0000-4000-8000-00000000000b\"],\"hideFrom\":[\"00000000-0000-4000-8000-00000000000c\"],\"muted\":[],\"shareViews\":false,\"allowReplies\":true}", json)
        val sealed = crypto.sealStoryPrefs(json, alice.b64("privateKey"), v.b64("prefsNonce"))
        assertArrayEquals(v.b64("prefsCiphertext"), sealed.ciphertext)
        // A fresh seal uses a fresh nonce and still opens.
        val fresh = crypto.sealStoryPrefs(json, alice.b64("privateKey"))
        assertEquals(json, crypto.openStoryPrefs(fresh.ciphertext, fresh.nonce, alice.b64("privateKey")))
    }

    @Test
    fun aHighlightTitleOpensWithItsCoverStoryAndIsSealedToTheWebsBytes() {
        val coverId = v.str("coverId")
        val coverKey = v.b64("coverKey")
        assertEquals(v.str("title"), crypto.openHighlightTitle(v.b64("titleCiphertext"), v.b64("titleNonce"), coverId, coverKey))
        assertArrayEquals(v.b64("titleCiphertext"), crypto.sealHighlightTitle(v.str("title"), coverId, coverKey, v.b64("titleNonce")).ciphertext)
        // Another cover story or key, a changed byte: nothing.
        assertNull(crypto.openHighlightTitle(v.b64("titleCiphertext"), v.b64("titleNonce"), storyId, coverKey))
        assertNull(crypto.openHighlightTitle(v.b64("titleCiphertext"), v.b64("titleNonce"), coverId, key))
        assertNull(crypto.openHighlightTitle(flipLast(v.b64("titleCiphertext")), v.b64("titleNonce"), coverId, coverKey))
        // Trimmed and cut at 40 characters, as on the web.
        val long = crypto.sealHighlightTitle("  " + "x".repeat(60) + "  ", coverId, coverKey)
        assertEquals("x".repeat(40), crypto.openHighlightTitle(long.ciphertext, long.nonce, coverId, coverKey))
    }

    @Test
    fun base64IsTheStandardPaddedKind() {
        assertEquals(v.str("key"), Base64Codec.encode(key))
    }
}
