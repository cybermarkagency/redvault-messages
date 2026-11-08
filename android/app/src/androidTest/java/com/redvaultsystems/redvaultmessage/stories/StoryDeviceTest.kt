// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.stories

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.goterl.lazysodium.LazySodiumAndroid
import com.goterl.lazysodium.SodiumAndroid
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileDecryptException
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.crypto.StoryCryptoException
import com.redvaultsystems.redvaultmessage.data.StoryDecoder
import com.redvaultsystems.redvaultmessage.data.StoryFriend
import com.redvaultsystems.redvaultmessage.data.api.StoryFeedParser
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.security.MessageDigest

/**
 * The Stories crypto and feed reading through lazysodium-android (the libsodium the app ships), against the same vectors
 * the web app's code made that the JVM tests in src/test/.../stories use (the vector files are this test's assets).
 * Runs on a device or emulator: `./gradlew connectedDebugAndroidTest`.
 */
@RunWith(AndroidJUnit4::class)
class StoryDeviceTest {
    private val sodium = LazySodiumAndroid(SodiumAndroid())
    private val crypto = StoryCrypto(sodium)

    private fun vectors(name: String): JsonObject {
        val assets = InstrumentationRegistry.getInstrumentation().context.assets
        return Json.parseToJsonElement(assets.open(name).use { String(it.readBytes(), Charsets.UTF_8) }).jsonObject
    }

    private fun JsonObject.str(key: String) = getValue(key).jsonPrimitive.content
    private fun JsonObject.obj(key: String) = getValue(key).jsonObject
    private fun JsonObject.b64(key: String) = Base64Codec.decode(str(key))

    @Test
    fun openTheKeyAndDetailsTheWebSealed() {
        val v = vectors("story_vectors.json")
        val alice = v.obj("alice")
        val bob = v.obj("bob")
        val key = crypto.unwrapStoryKey(v.str("storyId"), v.b64("wrappedForBob"), alice.b64("publicKey"), bob.b64("privateKey"))
        assertArrayEquals(v.b64("key"), key)
        assertEquals(v.str("metaJson"), crypto.openStoryMeta(v.str("storyId"), key, v.b64("metaCiphertext"), v.b64("metaNonce")))
        // Sealing the same details with the same nonce gives the web's own bytes.
        assertArrayEquals(v.b64("metaCiphertext"), crypto.sealStoryMeta(v.str("storyId"), key, v.str("metaJson"), v.b64("metaNonce")).ciphertext)
        assertThrows(StoryCryptoException::class.java) {
            crypto.unwrapStoryKey("0e9d8c7b-6a5f-4e3d-8c2b-1a0f9e8d7c6b", v.b64("wrappedForBob"), alice.b64("publicKey"), bob.b64("privateKey"))
        }
        // The settings sealed by the web open with the account key.
        assertEquals(true, crypto.openStoryPrefs(v.b64("prefsCiphertext"), v.b64("prefsNonce"), alice.b64("privateKey")).contains("hideFrom"))
    }

    @Test
    fun readAFeedTheWebSealedAndOpenItsMedia() {
        val v = vectors("story_wire_vectors.json")
        val ids = v.obj("ids")
        val bob = v.obj("keys").obj("bob")
        val friends = v.getValue("friendsOfBob").jsonArray.map { it.jsonObject }.associate {
            it.str("userId") to StoryFriend(it.str("userId"), it.str("username"), null, it.b64("publicKey"))
        }
        val feed = StoryFeedParser.parse(v.obj("feedForBob"))
        val shown = StoryDecoder(crypto).decodeFeed(feed, bob.b64("publicKey"), bob.b64("privateKey"), friends, emptySet()).authors.flatMap { it.stories }
        val audio = v.getValue("skippedByAndroid").jsonArray.single().jsonObject.str("id")
        assertEquals(v.getValue("webVisible").jsonArray.map { it.jsonObject.str("id") }.filter { it != audio }, shown.map { it.id })
        assertEquals(setOf(ids.str("alice"), ids.str("carol")), shown.map { it.authorId }.toSet())

        val file = FileCrypto(sodium)
        for (story in shown.filter { it.hasMedia }) {
            val media = story.meta.media!!
            val vector = v.obj("media").obj(story.id)
            val ciphertext = Base64Codec.decode(vector.str("ciphertext"))
            val out = ByteArrayOutputStream()
            file.decrypt(ByteArrayInputStream(ciphertext), out, Base64Codec.decode(media.key), Base64Codec.decode(media.header), media.encryptedSize, media.sha256)
            val plain = out.toByteArray()
            assertEquals(vector.str("plaintextSha256"), MessageDigest.getInstance("SHA-256").digest(plain).joinToString("") { "%02x".format(it) })
            // A changed byte is refused.
            assertThrows(FileDecryptException::class.java) {
                file.decrypt(ByteArrayInputStream(ciphertext.copyOf().also { it[100] = (it[100].toInt() xor 1).toByte() }), ByteArrayOutputStream(), Base64Codec.decode(media.key), Base64Codec.decode(media.header), media.encryptedSize, media.sha256)
            }
        }
    }
}
