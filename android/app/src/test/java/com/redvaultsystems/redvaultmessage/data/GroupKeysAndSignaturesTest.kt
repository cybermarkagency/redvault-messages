// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.model.ConversationMeta
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.data.model.RichMessage
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * Group key epochs and message signatures (db/migrations/030) against a small
 * stand-in for the server that makes the same checks: no message while the
 * group needs a new key, only the current epoch accepted, one sealed copy per
 * remaining member on rotation.
 */
class GroupKeysAndSignaturesTest {
    private val crypto = E2eCrypto(LazySodiumJava(SodiumJava()))
    private lateinit var server: MockWebServer
    private val fake = FakeServer()

    private inner class Person(val userId: String) {
        val keys = crypto.generateKeyPair()
        val identity = SelfIdentity("h", userId, keys.publicKey, keys.privateKey, null, "user", "free", false)
        var signingKey: String? = null
        var deleted = false
    }

    private inner class FakeServer : Dispatcher() {
        var caller = ""
        val people = LinkedHashMap<String, Person>()
        val conversationId = "c0000000-0000-4000-8000-000000000001"
        val members = ArrayList<String>()
        var keyEpoch = 0
        var rekeyRequired = false
        val keys = HashMap<String, MutableList<Pair<Int, String>>>()
        var nameCiphertext: String? = null
        var nameNonce: String? = null
        val messages = ArrayList<JsonObject>()
        var seq = 0

        private fun json(element: Any) = MockResponse().setResponseCode(200).setHeader("Content-Type", "application/json").setBody(element.toString())
        private fun error(status: Int, code: String) = MockResponse().setResponseCode(status).setBody("""{"error":"$code","message":"$code"}""")

        fun conversation(userId: String) = buildJsonObject {
            val mine = keys.getValue(userId)
            put("id", conversationId)
            put("isGroup", true)
            put("role", "member")
            put("nameCiphertext", nameCiphertext)
            put("nameNonce", nameNonce)
            put("wrappedKey", mine.first { it.first == keyEpoch }.second)
            put("keyEpoch", keyEpoch)
            put("keys", buildJsonArray { mine.forEach { (e, k) -> add(buildJsonObject { put("epoch", e); put("wrappedKey", k) }) } })
            put("rekeyRequired", rekeyRequired)
            put("createdBy", members.first())
            put("createdAt", "2026-10-01T00:00:00.000Z")
        }

        override fun dispatch(request: RecordedRequest): MockResponse {
            val path = request.path!!.removePrefix("/api/").substringBefore("?")
            val body = request.body.readUtf8().takeIf { it.isNotEmpty() }?.let { ApiClient.json.parseToJsonElement(it).jsonObject }
            return when {
                request.method == "GET" && path == "conversations" ->
                    json(JsonArray(if (caller in members) listOf(conversation(caller)) else emptyList()))
                request.method == "GET" && path == "conversations/$conversationId/participants" ->
                    json(JsonArray(members.map { buildJsonObject { put("userId", it); put("role", "member"); put("joinedAt", "2026-10-01T00:00:00.000Z") } }))
                request.method == "GET" && path.startsWith("users/") -> {
                    val p = people.getValue(path.removePrefix("users/"))
                    json(buildJsonObject {
                        put("userId", p.userId); put("username", "u"); put("publicKey", Base64Codec.encode(p.keys.publicKey))
                        put("createdAt", "2026-10-01T00:00:00.000Z"); put("deleted", p.deleted)
                        put("signingPublicKey", p.signingKey); put("signingKeySince", p.signingKey?.let { "2026-10-01" })
                    })
                }
                request.method == "PUT" && path == "auth/signing-key" -> {
                    people.getValue(caller).signingKey = body!!.getValue("signingPublicKey").jsonPrimitive.content
                    MockResponse().setResponseCode(204)
                }
                request.method == "POST" && path == "conversations/$conversationId/keys" -> {
                    if (body!!.getValue("fromEpoch").jsonPrimitive.int != keyEpoch) return error(409, "stale_key_epoch")
                    val given = body.getValue("keys").jsonArray.map { it.jsonObject }
                    val active = members.filter { !people.getValue(it).deleted }.sorted()
                    if (given.map { it.getValue("userId").jsonPrimitive.content }.sorted() != active) return error(409, "membership_changed")
                    keyEpoch++
                    rekeyRequired = false
                    for (k in given) keys.getValue(k.getValue("userId").jsonPrimitive.content) += keyEpoch to k.getValue("wrappedKey").jsonPrimitive.content
                    nameCiphertext = body["nameCiphertext"]?.jsonPrimitive?.content
                    nameNonce = body["nameNonce"]?.jsonPrimitive?.content
                    json(buildJsonObject { put("keyEpoch", keyEpoch) })
                }
                request.method == "POST" && path == "conversations/$conversationId/messages" -> {
                    if (rekeyRequired) return error(409, "group_rekey_required")
                    if ((body!!["keyEpoch"]?.jsonPrimitive?.int ?: 0) != keyEpoch) return error(409, "stale_key_epoch")
                    val message = message(caller, body.getValue("ciphertext").jsonPrimitive.content, body.getValue("nonce").jsonPrimitive.content, keyEpoch, body["signature"]?.jsonPrimitive?.content)
                    messages += message
                    json(message)
                }
                request.method == "GET" && path == "conversations/$conversationId/messages" -> json(JsonArray(messages))
                request.method == "GET" && path == "conversations/$conversationId/receipts" -> json(JsonArray(emptyList()))
                else -> error(404, "not_found")
            }
        }

        fun message(senderId: String, ciphertext: String, nonce: String, epoch: Int, signature: String?) = buildJsonObject {
            put("id", "e0000000-0000-4000-8000-%012d".format(++seq))
            put("conversationId", conversationId)
            put("senderId", senderId)
            put("ciphertext", ciphertext)
            put("nonce", nonce)
            put("createdAt", "2026-10-02T00:00:%02d.000Z".format(seq))
            put("keyEpoch", epoch)
            put("signature", signature)
        }
    }

    private lateinit var alice: Person
    private lateinit var bob: Person
    private lateinit var carol: Person
    private lateinit var groupKey: ByteArray

    @Before
    fun setUp() {
        server = MockWebServer()
        server.dispatcher = fake
        server.start()
        alice = Person("a0000000-0000-4000-8000-000000000001")
        bob = Person("b0000000-0000-4000-8000-000000000002")
        carol = Person("c0000000-0000-4000-8000-000000000003")
        for (p in listOf(alice, bob, carol)) fake.people[p.userId] = p
        groupKey = crypto.generateConversationKey()
        for (p in listOf(alice, bob, carol)) {
            fake.members += p.userId
            fake.keys[p.userId] = mutableListOf(0 to Base64Codec.encode(crypto.sealConversationKey(groupKey, p.keys.publicKey)))
        }
        val name = crypto.encryptWithConversationKey("Weekend plans", groupKey)
        fake.nameCiphertext = Base64Codec.encode(name.ciphertext)
        fake.nameNonce = Base64Codec.encode(name.nonce)
    }

    @After
    fun tearDown() = server.shutdown()

    private class MemoryPins : SigningKeyPins {
        val pinned = HashMap<String, String>()
        override fun pin(selfUserId: String, userId: String, keyB64: String) = pinned.getOrPut("$selfUserId:$userId") { keyB64 }
        override fun pinned(selfUserId: String, userId: String) = pinned["$selfUserId:$userId"]
    }

    /** One device of [who]: its own key state and repository, like the app after sign-in. */
    private inner class Device(val who: Person, pins: MemoryPins = MemoryPins()) {
        private val api = ApiClient.create(server.url("/api/").toString(), OkHttpClient())
        val keys = MessageKeys(api, crypto, pins, KeyTrust(crypto, MemoryKeyPins())).also { it.begin(who.identity) }
        val conversations = ConversationRepository(api, crypto, null, keys)

        suspend fun meta(): ConversationMeta {
            fake.caller = who.userId
            val dto = conversations.listConversations().single()
            return conversations.hydrateMeta(dto, who.identity)!!
        }

        suspend fun read() = run {
            val meta = meta()
            fake.caller = who.userId
            conversations.fetchMessages(meta, who.userId).messages
        }

        suspend fun send(text: String) = run {
            val meta = meta()
            fake.caller = who.userId
            conversations.send(meta, text)
        }
    }

    private fun removeCarol() {
        fake.members.remove(carol.userId)
        fake.keys.remove(carol.userId)
        fake.rekeyRequired = true
    }

    @Test
    fun aRemovedMemberNeverGetsTheNewKey() = runBlocking {
        removeCarol()
        val device = Device(alice)
        fake.caller = alice.userId
        device.keys.rotate(fake.conversationId)
        assertEquals(1, fake.keyEpoch)
        assertFalse(fake.rekeyRequired)
        assertFalse(carol.userId in fake.keys)
        val bobsCopy = fake.keys.getValue(bob.userId).single { it.first == 1 }.second
        val newKey = crypto.unsealConversationKey(Base64Codec.decode(bobsCopy), bob.keys.publicKey, bob.keys.privateKey)
        assertFalse(newKey.contentEquals(groupKey))
        assertEquals("Weekend plans", crypto.decryptWithConversationKey(Base64Codec.decode(fake.nameCiphertext!!), Base64Codec.decode(fake.nameNonce!!), newKey))
    }

    @Test
    fun aDeletedAccountIsSkippedWhenSealing() = runBlocking {
        carol.deleted = true
        fake.rekeyRequired = true
        fake.caller = alice.userId
        Device(alice).keys.rotate(fake.conversationId)
        assertEquals(listOf(0), fake.keys.getValue(carol.userId).map { it.first })
    }

    @Test
    fun aSendThatFindsTheGroupWaitingRotatesThenSendsSigned() = runBlocking {
        val device = Device(alice)
        device.meta() // loaded before Carol is removed
        removeCarol()
        val sent = device.send("after the change")
        val stored = fake.messages.single()
        assertEquals(1, stored.getValue("keyEpoch").jsonPrimitive.int)
        val signature = stored.getValue("signature").jsonPrimitive.content
        val input = E2eCrypto.messageSignatureInput(
            fake.conversationId, alice.userId, 1, Base64Codec.decode(sent.nonceB64!!), Base64Codec.decode(sent.ciphertextB64!!),
        )
        assertTrue(crypto.verifyMessageSignature(Base64Codec.decode(signature), input, Base64Codec.decode(alice.signingKey!!)))
        assertEquals(listOf("after the change"), Device(bob).read().map { it.text })
    }

    @Test
    fun eachMessageOpensWithItsOwnEpochAndOneBrokenMessageDoesNotHideTheRest() = runBlocking {
        val device = Device(alice)
        device.send("before")
        removeCarol()
        device.send("after")
        fake.messages += fake.message(alice.userId, Base64Codec.encode(ByteArray(40)), Base64Codec.encode(ByteArray(24)), 0, null)
        val shown = Device(bob).read()
        assertEquals(listOf("before", "after", "Couldn't decrypt this message."), shown.map { it.text })
        assertEquals(MessageKind.Undecryptable, shown.last().kind)
    }

    @Test
    fun forgedSignaturesAreHiddenAndUnsignedMessagesFromSignersAreFlagged() = runBlocking {
        Device(alice).send("real one")
        val genuineId = fake.messages.single().getValue("id").jsonPrimitive.content
        // Carol (a member, so she has the key) signs one "from Alice" with her own key...
        val forged = crypto.encryptWithConversationKey("forged", groupKey)
        val carolKey = crypto.deriveSigningKeyPair(carol.keys.privateKey)
        val forgedSignature = crypto.signMessage(
            E2eCrypto.messageSignatureInput(fake.conversationId, alice.userId, 0, forged.nonce, forged.ciphertext), carolKey.secretKey,
        )
        fake.messages += fake.message(alice.userId, Base64Codec.encode(forged.ciphertext), Base64Codec.encode(forged.nonce), 0, Base64Codec.encode(forgedSignature))
        // ...one unsigned, and an unsigned reaction.
        val unsigned = crypto.encryptWithConversationKey("unsigned", groupKey)
        fake.messages += fake.message(alice.userId, Base64Codec.encode(unsigned.ciphertext), Base64Codec.encode(unsigned.nonce), 0, null)
        val reaction = crypto.encryptWithConversationKey(RichMessage.serializeReaction(genuineId.lowercase(), "👍"), groupKey)
        fake.messages += fake.message(alice.userId, Base64Codec.encode(reaction.ciphertext), Base64Codec.encode(reaction.nonce), 0, null)

        val shown = Device(bob).read()
        assertEquals(
            listOf("real one" to false, "Couldn't verify who sent this message." to false, "unsigned" to true),
            shown.map { it.text to it.unverifiedSender },
        )
        assertTrue(shown.first().reactions.isEmpty())
    }

    @Test
    fun theFirstSigningKeySeenIsPinned() = runBlocking {
        Device(alice).send("one")
        val pins = MemoryPins()
        Device(bob, pins).read()
        assertNotNull(pins.pinned["${bob.userId}:${alice.userId}"])
        // The server swaps in another key and serves a message signed with it.
        val other = crypto.deriveSigningKeyPair(crypto.generateKeyPair().privateKey)
        alice.signingKey = Base64Codec.encode(other.publicKey)
        val fakeMessage = crypto.encryptWithConversationKey("swapped", groupKey)
        val signature = crypto.signMessage(
            E2eCrypto.messageSignatureInput(fake.conversationId, alice.userId, 0, fakeMessage.nonce, fakeMessage.ciphertext), other.secretKey,
        )
        fake.messages += fake.message(alice.userId, Base64Codec.encode(fakeMessage.ciphertext), Base64Codec.encode(fakeMessage.nonce), 0, Base64Codec.encode(signature))
        assertEquals(listOf("one", "Couldn't verify who sent this message."), Device(bob, pins).read().map { it.text })
    }

    @Test
    fun someoneWhoRegistersASigningKeyMidSessionIsNotTreatedAsAForger() = runBlocking {
        Device(alice).send("one")
        val aliceKey = alice.signingKey
        // Her key isn't on the server yet when Bob first reads the chat...
        alice.signingKey = null
        val bobsDevice = Device(bob)
        bobsDevice.read()
        // ...then she registers it: the same session must look again, not call her message forged.
        alice.signingKey = aliceKey
        assertEquals(listOf("one"), bobsDevice.read().map { it.text })
    }

    @Test
    fun ownSignedMessagesAreNotForgedJustBecauseTheKeyWasRegisteredByThisVerySend() = runBlocking {
        Device(alice).send("one")
        alice.signingKey = null
        val device = Device(alice)
        device.read() // Our own key isn't on the server yet: cached as "no key"...
        device.send("two") // ...then this send registers it.
        assertEquals(listOf("one", "two"), device.read().map { it.text })
    }
}
