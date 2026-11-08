// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File

/**
 * Public share links (docs/public-share-links.md) against vectors the web
 * app's own code produced (desktop/tests/vectors/make_share_vectors.ts): a
 * link made in any app opens on this phone, the tokens the server checks are
 * the same, and the owner's sealed copy, items and uploads open here.
 */
class ShareCryptoInteropTest {
    private val share = ShareCrypto(LazySodiumJava(SodiumJava()))
    private val vectors: JsonObject = Json.parseToJsonElement(
        listOf("../../desktop/tests/vectors/share_vectors.json", "desktop/tests/vectors/share_vectors.json").map(::File).first { it.exists() }.readText(),
    ).jsonObject

    private fun JsonObject.s(key: String): String = getValue(key).jsonPrimitive.content
    private fun b64(text: String): ByteArray = Base64Codec.decode(text)

    @Test
    fun derivesTheWebKeysWithAndWithoutPasswords() {
        for (c in vectors.getValue("keys").jsonArray) {
            val o = c.jsonObject
            val secret = share.fromFragment(o.s("secret"))!!
            val params = (o["params"] as? JsonObject)?.let {
                SharePasswordParams(b64(it.s("salt")), it.getValue("ops").jsonPrimitive.long, it.getValue("mem").jsonPrimitive.long)
            }
            // One password is stored decomposed on purpose: it must derive what its NFC form does.
            val password = if (o["password"] is JsonNull) null else o.s("password")
            val keys = share.deriveKeys(secret, password, params)
            assertEquals(o.s("lookup"), Base64Codec.encode(keys.lookup))
            assertEquals(o.s("access"), Base64Codec.encode(keys.access))
            assertEquals(o.s("wrap"), Base64Codec.encode(keys.wrap))
            assertEquals(o.s("lookupVerifier"), share.verifierOf(keys.lookup))
            assertEquals(o.s("accessVerifier"), share.verifierOf(keys.access))
        }
    }

    @Test
    fun opensWebEnvelopesAndNothingAltered() {
        for (c in vectors.getValue("envelopes").jsonArray) {
            val o = c.jsonObject
            val wrap = b64(o.s("wrap"))
            val ciphertext = b64(o.s("ciphertext"))
            val nonce = b64(o.s("nonce"))
            val env = share.openEnvelope(wrap, ciphertext, nonce)
            assertNotNull(env)
            assertEquals(o.s("key"), Base64Codec.encode(env!!.key))
            assertEquals(o.s("root"), env.root)
            if (o["upload"] is JsonNull) assertNull(env.upload) else assertEquals(o.s("upload"), Base64Codec.encode(env.upload!!))
            ciphertext[2] = (ciphertext[2].toInt() xor 1).toByte()
            assertNull(share.openEnvelope(wrap, ciphertext, nonce))
            val resealed = share.sealEnvelope(wrap, env)
            assertEquals(env.root, share.openEnvelope(wrap, resealed.ciphertext, resealed.nonce)!!.root)
        }
    }

    @Test
    fun opensWebItemsUnderTheirOwnIdOnly() {
        for (c in vectors.getValue("items").jsonArray) {
            val o = c.jsonObject
            val key = b64(o.s("shareKey"))
            val id = o.s("itemId")
            val meta = share.openItem(key, id, b64(o.s("ciphertext")), b64(o.s("nonce")))
            assertEquals(o.getValue("meta"), meta)
            assertNull(share.openItem(key, id + "x", b64(o.s("ciphertext")), b64(o.s("nonce"))))
            val here = share.sealItem(key, id, meta!!)
            assertEquals(o.getValue("meta"), share.openItem(key, id, here.ciphertext, here.nonce))
        }
    }

    @Test
    fun opensTheOwnersCopy() {
        val o = vectors.getValue("ownerBox").jsonObject
        val box = share.openOwnerBox(b64(o.s("driveKey")), b64(o.s("ciphertext")), b64(o.s("nonce")))
        val expected = o.getValue("box").jsonObject
        assertEquals(ShareOwnerBox(expected.s("secret"), expected.s("key"), expected.s("root"), expected.s("label"), expected.getValue("created").jsonPrimitive.long), box)
        assertNull(share.openOwnerBox(ByteArray(32) { 9 }, b64(o.s("ciphertext")), b64(o.s("nonce"))))
    }

    @Test
    fun derivesUploadKeysAndOpensWebUploads() {
        for (k in vectors.getValue("uploadKeys").jsonArray) {
            val o = k.jsonObject
            val pair = share.uploadKeyPair(b64(o.s("driveKey")), b64(o.s("salt")))
            assertEquals(o.s("publicKey"), Base64Codec.encode(pair.publicKey))
            assertEquals(o.s("privateKey"), Base64Codec.encode(pair.privateKey))
        }
        val c = vectors.getValue("contribution").jsonObject
        val pair = ShareKeyPair(b64(c.s("publicKey")), b64(c.s("privateKey")))
        assertEquals(c.getValue("meta"), share.openContribution(pair, b64(c.s("sealed"))))
        assertNull(share.openContribution(share.uploadKeyPair(ByteArray(32) { 1 }, ByteArray(16) { 2 }), b64(c.s("sealed"))))
    }

    @Test
    fun writesLinksLikeTheWeb() {
        val u = vectors.getValue("url").jsonObject
        assertEquals(u.s("url"), share.shareUrl(u.s("webBase"), u.s("publicId"), share.fromFragment(u.s("secret"))!!))
        assertArrayEquals(share.fromFragment(u.s("secret")), share.fromFragment(share.toFragment(share.fromFragment(u.s("secret"))!!)))
    }
}
