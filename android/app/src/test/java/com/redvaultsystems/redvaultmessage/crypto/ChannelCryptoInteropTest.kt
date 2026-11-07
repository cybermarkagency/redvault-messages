package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.data.model.ChannelPostRef
import com.redvaultsystems.redvaultmessage.data.model.Channels
import com.redvaultsystems.redvaultmessage.data.model.ParsedBody
import com.redvaultsystems.redvaultmessage.data.model.RichMessage
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * Pins private channel links and channel posts forwarded into chats to the
 * web byte for byte. The vectors came out of the web's own lib/channels.ts and
 * lib/richMessage.ts (desktop/tests/vectors/make_channel_vectors.ts), the same
 * file the desktop app is tested against. If these fail, a private link made
 * on the web won't open in this app, or a forwarded post won't show as one.
 */
class ChannelCryptoInteropTest {
    private val sodium = LazySodiumJava(SodiumJava())
    private val e2e = E2eCrypto(sodium)
    private val crypto = ChannelCrypto(sodium, e2e)
    private val vectors: JsonObject = Json.parseToJsonElement(
        listOf("../../desktop/tests/vectors/channel_vectors.json", "desktop/tests/vectors/channel_vectors.json")
            .map(::File).first { it.exists() }.readText(),
    ).jsonObject

    private fun str(o: JsonObject, key: String): String? = o[key]?.let { if (it is JsonNull) null else it.jsonPrimitive.content }

    @Test
    fun accessTokensMatchTheWeb() {
        val tokens = vectors["tokens"]!!.jsonArray
        assertEquals(4, tokens.size)
        for (t in tokens) {
            val o = t.jsonObject
            assertEquals(str(o, "access"), crypto.accessToken(str(o, "secret")!!))
        }
    }

    @Test
    fun onlyTheCanonicalSecretInAFragmentCounts() {
        for (c in vectors["fragmentCases"]!!.jsonArray) {
            val o = c.jsonObject
            assertEquals("fragment ${str(o, "fragment")}", str(o, "secret"), ChannelCrypto.secretFromFragment(str(o, "fragment")!!))
        }
    }

    @Test
    fun theOwnersSealedCopyOpensOnlyWithTheirKeys() {
        val owner = vectors["owner"]!!.jsonObject
        val other = vectors["other"]!!.jsonObject
        val sealed = vectors["sealed"]!!.jsonObject
        val ownerPublic = Base64Codec.decode(str(owner, "publicKey")!!)
        val ownerPrivate = Base64Codec.decode(str(owner, "privateKey")!!)
        assertEquals(str(sealed, "secret"), crypto.openSecret(str(sealed, "sealed"), ownerPublic, ownerPrivate))
        assertNull(crypto.openSecret(str(sealed, "sealed"), Base64Codec.decode(str(other, "publicKey")!!), Base64Codec.decode(str(other, "privateKey")!!)))
        assertNull(crypto.openSecret(null, ownerPublic, ownerPrivate))
        assertNull(crypto.openSecret("", ownerPublic, ownerPrivate))
        assertNull(crypto.openSecret("not base64!", ownerPublic, ownerPrivate))

        // What this app seals, the web (and this app) opens: a fresh secret round-trips, and its token works out the same.
        val fresh = crypto.newSecret()
        assertEquals(fresh, ChannelCrypto.secretFromFragment(fresh))
        assertEquals(fresh, crypto.openSecret(crypto.sealSecret(fresh, ownerPublic), ownerPublic, ownerPrivate))
        assertNotEquals(fresh, crypto.newSecret())
    }

    @Test
    fun linksAreTheWebsAndParseBack() {
        val id = str(vectors, "channelId")!!
        val base = str(vectors, "webBase")!!
        val secret = str(vectors["sealed"]!!.jsonObject, "secret")!!
        assertEquals(str(vectors, "publicUrl"), Channels.url(base, id))
        assertEquals(str(vectors, "privateUrl"), Channels.url(base, id, secret))
        assertEquals(id, Channels.parse(str(vectors, "publicUrl")!!)?.id)
        assertNull(Channels.parse(str(vectors, "publicUrl")!!)?.secret)
        assertEquals(secret, Channels.parse(str(vectors, "privateUrl")!!)?.secret)
        assertEquals(secret, Channels.parseOwn(str(vectors, "privateUrl")!!, base)?.secret)
    }

    @Test
    fun forwardedPostsAreTheWebsBodies() {
        for (f in vectors["forwards"]!!.jsonArray) {
            val o = f.jsonObject
            val ref = o["ref"]!!.jsonObject
            val body = RichMessage.serializeChannelForward(str(o, "text")!!, ChannelPostRef(str(ref, "channelId")!!, str(ref, "postId")!!, str(ref, "channelName")!!))
            assertEquals(str(o, "body"), body)
        }
    }

    @Test
    fun forwardedPostsParseAsTheWebParsesThem() {
        for (c in vectors["parseCases"]!!.jsonArray) {
            val o = c.jsonObject
            val expected = o["parsed"]!!.jsonObject
            val parsed = RichMessage.parse(str(o, "body")!!)
            if (str(expected, "kind") == "invalid") {
                assertEquals("body ${str(o, "body")}", ParsedBody.Invalid, parsed)
                continue
            }
            assertTrue("body ${str(o, "body")}", parsed is ParsedBody.Text)
            parsed as ParsedBody.Text
            assertEquals(str(expected, "text"), parsed.text)
            assertEquals(expected["forwarded"]?.jsonPrimitive?.booleanOrNull ?: false, parsed.forwarded)
            val post = expected["channelPost"]!!.jsonObject
            assertEquals(
                ChannelPostRef(str(post, "channelId")!!, str(post, "postId")!!, post["channelName"]!!.jsonPrimitive.contentOrNull!!),
                parsed.channelPost,
            )
        }
    }
}
