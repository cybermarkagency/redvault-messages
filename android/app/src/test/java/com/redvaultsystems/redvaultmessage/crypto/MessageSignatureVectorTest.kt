package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * Message signatures (db/migrations/030) against vectors the web app's own
 * code produced (desktop/tests/vectors/make_message_signature_vectors.ts): the
 * same signing key from the same account key, the same signed bytes, and the
 * same Ed25519 signature (deterministic), so every app accepts the others'.
 */
class MessageSignatureVectorTest {
    private val crypto = E2eCrypto(LazySodiumJava(SodiumJava()))
    private val vectors = Json.parseToJsonElement(File("../../desktop/tests/vectors/message_signature_vectors.json").readText()).jsonObject

    @Test
    fun derivesTheWebSigningKeys() {
        for (account in vectors.getValue("accounts").jsonArray) {
            val fields = account.jsonObject
            val pair = crypto.deriveSigningKeyPair(Base64Codec.decode(fields.getValue("accountPrivateKey").jsonPrimitive.content))
            assertEquals(fields.getValue("signingPublicKey").jsonPrimitive.content, Base64Codec.encode(pair.publicKey))
        }
    }

    @Test
    fun signsTheSameBytesAndProducesTheSameSignatures() {
        val accounts = vectors.getValue("accounts").jsonArray.map {
            crypto.deriveSigningKeyPair(Base64Codec.decode(it.jsonObject.getValue("accountPrivateKey").jsonPrimitive.content))
        }
        for (case in vectors.getValue("cases").jsonArray) {
            val c = case.jsonObject
            val input = E2eCrypto.messageSignatureInput(
                c.getValue("conversationId").jsonPrimitive.content,
                c.getValue("senderId").jsonPrimitive.content,
                c.getValue("keyEpoch").jsonPrimitive.long,
                Base64Codec.decode(c.getValue("nonce").jsonPrimitive.content),
                Base64Codec.decode(c.getValue("ciphertext").jsonPrimitive.content),
            )
            assertEquals(c.getValue("inputHex").jsonPrimitive.content, input.joinToString("") { "%02x".format(it) })
            val pair = accounts[c.getValue("account").jsonPrimitive.int]
            val expected = Base64Codec.decode(c.getValue("signature").jsonPrimitive.content)
            assertArrayEquals(expected, crypto.signMessage(input, pair.secretKey))
            assertTrue(crypto.verifyMessageSignature(expected, input, pair.publicKey))
            // Any other key, or any other bytes, fails.
            assertFalse(crypto.verifyMessageSignature(expected, input, accounts[1 - c.getValue("account").jsonPrimitive.int].publicKey))
            assertFalse(crypto.verifyMessageSignature(expected, input + byteArrayOf(0), pair.publicKey))
        }
    }

    @Test(expected = CryptoException::class)
    fun refusesIdsThatAreNotUuids() {
        E2eCrypto.messageSignatureInput("conv-1", "11111111-2222-4333-8444-555555555555", 0, ByteArray(24), ByteArray(1))
    }
}
