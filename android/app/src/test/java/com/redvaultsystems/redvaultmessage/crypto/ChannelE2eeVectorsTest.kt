// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.data.model.ChannelSealedPosts
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File

/**
 * Part C1 (docs/signal-integration/10-part-c1-private-channels.md): this app seals and opens private channel posts
 * byte for byte as libsodium does in desktop/tests/vectors/channel_e2ee_vectors.json, the file the desktop app
 * is tested against too. If these fail, a post written on one app won't open on the other.
 */
class ChannelE2eeVectorsTest {
    private val sodium = LazySodiumJava(SodiumJava())
    private val crypto = ChannelCrypto(sodium, E2eCrypto(sodium))
    private val v: JsonObject = Json.parseToJsonElement(
        listOf("../../desktop/tests/vectors/channel_e2ee_vectors.json", "desktop/tests/vectors/channel_e2ee_vectors.json")
            .map(::File).first { it.exists() }.readText(),
    ).jsonObject

    private fun str(o: JsonObject, key: String) = o[key]!!.jsonPrimitive.content
    private val channelId get() = str(v, "channelId")
    private val secret get() = str(v, "secretText")
    private val epoch get() = v["epoch"]!!.jsonPrimitive.int
    private val post get() = v["post"]!!.jsonObject

    @Test
    fun theWrapKeyAndAccessTokenComeFromTheLinksSecret() {
        assertEquals(str(v, "wrapKey"), Base64Codec.encode(crypto.wrapKey(secret)))
        assertEquals(str(v, "accessToken"), crypto.accessToken(secret))
        assertEquals(str(v, "keyAd"), String(ChannelCrypto.keyAd(channelId, epoch), Charsets.UTF_8))
        assertEquals(str(v, "postAd"), String(ChannelCrypto.postAd(channelId, epoch), Charsets.UTF_8))
        // The channel id goes in lowercase whatever case it arrives in.
        assertEquals(str(v, "keyAd"), String(ChannelCrypto.keyAd(channelId.uppercase(), epoch), Charsets.UTF_8))
    }

    @Test
    fun theWrappedKeyOpensAndIsMadeTheSameWay() {
        val wrapKey = crypto.wrapKey(secret)
        val key = crypto.unwrapContentKey(str(v, "wrapped"), channelId, epoch, wrapKey)
        assertNotNull(key)
        assertEquals(str(v, "contentKey"), Base64Codec.encode(key!!))
        val nonce = Base64Codec.decode(str(v, "wrapped")).copyOfRange(0, ChannelCrypto.NONCE_BYTES)
        assertEquals(str(v, "wrapped"), crypto.wrapContentKey(key, channelId, epoch, wrapKey, nonce))
        assertEquals(ChannelCrypto.WRAPPED_BYTES, Base64Codec.decode(str(v, "wrapped")).size)
    }

    @Test
    fun thePostOpensAndIsSealedTheSameWay() {
        val key = Base64Codec.decode(str(v, "contentKey"))
        assertEquals(epoch, post["keyEpoch"]!!.jsonPrimitive.int)
        val opened = crypto.openPost(str(post, "nonce"), str(post, "ciphertext"), key, channelId, epoch)
        assertEquals(str(v, "plaintext"), opened)
        val sealed = crypto.sealPost(str(v, "plaintext"), key, channelId, epoch, Base64Codec.decode(str(post, "nonce")))
        assertEquals(str(post, "nonce"), sealed.nonce)
        assertEquals(str(post, "ciphertext"), sealed.ciphertext)
        assertEquals(epoch, sealed.keyEpoch)
    }

    @Test
    fun thePlaintextIsTheSpecsShapeAndWritesBackTheSame() {
        val plain = ChannelSealedPosts.decode(str(v, "plaintext"))
        assertNotNull(plain)
        assertEquals(1, plain!!.v)
        assertEquals("Hello, **followers** 👋", plain.body)
        assertEquals("Next topic?", plain.content!!.poll!!.question)
        assertEquals(listOf("A", "B"), plain.content.poll!!.options)
        assertEquals(str(v, "plaintext"), ChannelSealedPosts.encode(plain))
        // No content is written as null, as the spec spells it.
        assertEquals("""{"v":1,"body":"Hi","content":null}""", ChannelSealedPosts.encode(plain.copy(body = "Hi", content = null)))
        assertNull(ChannelSealedPosts.decode("""{"v":2,"body":"Hi","content":null}"""))
        assertNull(ChannelSealedPosts.decode("not json"))
    }

    @Test
    fun everyFailureCaseIsRefused() {
        val failures = v["failures"]!!.jsonArray
        assertEquals(6, failures.size)
        val wrapKey = crypto.wrapKey(secret)
        val key = Base64Codec.decode(str(v, "contentKey"))
        for (f in failures) {
            val o = f.jsonObject
            val name = str(o, "name")
            when (str(o, "kind")) {
                "unwrap" -> assertNull(name, crypto.unwrapContentKey(str(o, "wrapped"), str(o, "channelId"), o["epoch"]!!.jsonPrimitive.int, wrapKey))
                "post" -> assertNull(name, crypto.openPost(str(o, "nonce"), str(o, "ciphertext"), key, str(o, "channelId"), o["keyEpoch"]!!.jsonPrimitive.int))
                else -> error("unknown failure kind in $name")
            }
        }
        // And a different link's wrap key opens nothing.
        val other = crypto.wrapKey(crypto.newSecret())
        assertNull(crypto.unwrapContentKey(str(v, "wrapped"), channelId, epoch, other))
        assertArrayEquals(key, crypto.unwrapContentKey(str(v, "wrapped"), channelId, epoch, wrapKey))
    }
}
