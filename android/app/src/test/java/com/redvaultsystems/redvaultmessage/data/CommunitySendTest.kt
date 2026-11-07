package com.redvaultsystems.redvaultmessage.data

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.CommunityCrypto
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.KeyPair
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ConversationDto
import com.redvaultsystems.redvaultmessage.data.api.MessageDto
import com.redvaultsystems.redvaultmessage.data.api.RotateRequest
import com.redvaultsystems.redvaultmessage.data.api.SendMessageRequest
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.runBlocking
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.util.Collections

/**
 * The community send path (ConversationRepository.sendToCommunity) against a stand-in server: someone
 * left, so the server answers rekey_required; this app rotates (a new key sealed to exactly the roster),
 * then sends under the new epoch. And when another app rotated first, stale_key_epoch reloads the keys.
 * Real libsodium on the JVM; the messages are what the server would actually receive.
 */
class CommunitySendTest {
    private val sodium = LazySodiumJava(SodiumJava())
    private val e2e = E2eCrypto(sodium)
    private val crypto = CommunityCrypto(e2e)
    private val json = ApiClient.json

    private val communityId = "3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f"
    private val me = e2e.generateKeyPair()
    private val stayer = e2e.generateKeyPair()
    private val identity = SelfIdentity("member", "u-me", me.publicKey, me.privateKey, null, "user", "free", false)

    private lateinit var server: MockWebServer
    private lateinit var repository: ConversationRepository
    private lateinit var keys: CommunityKeys

    /** The sealed copies the server holds for this member, by epoch. */
    private val myCopies = Collections.synchronizedMap(sortedMapOf<Long, String>())
    @Volatile private var epoch = 0L
    @Volatile private var rekeyRequired = false
    private val sent = Collections.synchronizedList(mutableListOf<SendMessageRequest>())
    private val rotations = Collections.synchronizedList(mutableListOf<RotateRequest>())

    @Before
    fun setUp() {
        val k0 = e2e.generateConversationKey()
        myCopies[0] = Base64Codec.encode(crypto.sealEpochKey(k0, 0, communityId, me.publicKey))
        server = MockWebServer()
        server.dispatcher = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest): MockResponse {
                val path = request.path!!.substringBefore('?')
                val body = request.body.readUtf8()
                return when {
                    path == "/api/communities/$communityId/keys" -> ok(
                        """{"conversationId":"c1","keyEpoch":$epoch,"rekeyRequired":$rekeyRequired,"keys":[${myCopies.entries.joinToString(",") { """{"epoch":${it.key},"wrappedKey":"${it.value}"}""" }}]}""",
                    )
                    path == "/api/communities/$communityId/roster" -> ok(
                        """{"keyEpoch":$epoch,"rekeyRequired":$rekeyRequired,"members":[
                            {"userId":"u-me","publicKey":"${Base64Codec.encode(me.publicKey)}"},
                            {"userId":"u-stayer","publicKey":"${Base64Codec.encode(stayer.publicKey)}"}]}""",
                    )
                    path == "/api/communities/$communityId/rotate" -> {
                        val rotation = json.decodeFromString(RotateRequest.serializer(), body)
                        rotations += rotation
                        epoch = rotation.fromEpoch + 1
                        rekeyRequired = false
                        myCopies[epoch] = rotation.keys.first { it.userId == "u-me" }.wrappedKey
                        ok("""{"keyEpoch":$epoch}""")
                    }
                    path == "/api/conversations/c1/messages" && request.method == "POST" -> {
                        val message = json.decodeFromString(SendMessageRequest.serializer(), body)
                        when {
                            rekeyRequired -> conflict("rekey_required")
                            message.keyEpoch?.toLong() != epoch -> conflict("stale_key_epoch")
                            else -> {
                                sent += message
                                ok("""{"id":"m${sent.size}","conversationId":"c1","senderId":"u-me","ciphertext":"${message.ciphertext}","nonce":"${message.nonce}","keyEpoch":${message.keyEpoch},"createdAt":"2026-10-19T12:00:00.000Z"}""", 201)
                            }
                        }
                    }
                    else -> MockResponse().setResponseCode(404).setBody("""{"error":"not_found","message":"Not found"}""")
                }
            }
        }
        server.start()
        val base = server.url("/api/").toString()
        val client = OkHttpClient()
        keys = CommunityKeys(ApiClient.createCommunities(base, client), crypto)
        keys.setIdentity(identity)
        repository = ConversationRepository(ApiClient.create(base, client), e2e, hidden = null, communities = keys)
    }

    @After
    fun tearDown() = server.shutdown()

    private fun ok(body: String, code: Int = 200) = MockResponse().setResponseCode(code).setHeader("Content-Type", "application/json").setBody(body)
    private fun conflict(code: String) = MockResponse().setResponseCode(409).setHeader("Content-Type", "application/json")
        .setBody("""{"error":"$code","message":"$code"}""")

    private fun conversation() = json.decodeFromString(
        ConversationDto.serializer(),
        """{"id":"c1","isGroup":true,"role":"member","wrappedKey":"AA==","createdBy":"u-owner","createdAt":"2026-10-18T12:00:00.000Z",
           "community":{"id":"$communityId","name":"Night Market Notes","kind":"group","status":"published","role":"member","canPost":true,"keyEpoch":$epoch,"rekeyRequired":$rekeyRequired}}""",
    )

    private fun epochKey(epoch: Long, pair: KeyPair, copy: String) = crypto.openEpochKey(Base64Codec.decode(copy), epoch, communityId, pair)

    @Test
    fun sendsUnderTheCurrentEpoch() = runBlocking {
        val meta = repository.hydrateCommunity(conversation(), identity, previous = null)
        assertEquals(communityId, meta.community?.id)
        repository.send(meta, "hello")
        val message = sent.single()
        assertEquals(0, message.keyEpoch)
        assertEquals("hello", e2e.decryptWithConversationKey(Base64Codec.decode(message.ciphertext), Base64Codec.decode(message.nonce), epochKey(0, me, myCopies[0]!!)))
        assertTrue(rotations.isEmpty())
    }

    @Test
    fun aRemovalForcesARotationToExactlyWhoRemainsBeforeTheMessageGoesOut() = runBlocking {
        val meta = repository.hydrateCommunity(conversation(), identity, previous = null)
        // Someone left after this phone loaded the keys.
        rekeyRequired = true
        repository.send(meta, "after the removal")

        val rotation = rotations.single()
        assertEquals(0L, rotation.fromEpoch)
        assertEquals(listOf("u-me", "u-stayer"), rotation.keys.map { it.userId })
        val newKey = epochKey(1, stayer, rotation.keys[1].wrappedKey)
        // Never a key anyone already held.
        assertTrue(!newKey.contentEquals(epochKey(0, me, myCopies[0]!!)))

        val message = sent.single()
        assertEquals(1, message.keyEpoch)
        assertEquals("after the removal", e2e.decryptWithConversationKey(Base64Codec.decode(message.ciphertext), Base64Codec.decode(message.nonce), newKey))
    }

    @Test
    fun anotherAppRotatedFirstSoItReloadsAndSendsUnderTheNewEpoch() = runBlocking {
        val meta = repository.hydrateCommunity(conversation(), identity, previous = null)
        // Another member's app rotated meanwhile: epoch 1 exists and this phone holds a copy of it.
        val k1 = e2e.generateConversationKey()
        myCopies[1] = Base64Codec.encode(crypto.sealEpochKey(k1, 1, communityId, me.publicKey))
        epoch = 1
        repository.send(meta, "late")
        assertTrue(rotations.isEmpty())
        val message = sent.single()
        assertEquals(1, message.keyEpoch)
        assertEquals("late", e2e.decryptWithConversationKey(Base64Codec.decode(message.ciphertext), Base64Codec.decode(message.nonce), k1))
    }

    @Test
    fun receivedMessagesOpenWithTheirOwnEpochsKey() = runBlocking {
        val meta = repository.hydrateCommunity(conversation(), identity, previous = null)
        val k0 = epochKey(0, me, myCopies[0]!!)
        val body = e2e.encryptWithConversationKey("from the creator", k0)
        val dto = MessageDto(
            id = "m9", conversationId = "c1", senderId = "u-owner",
            ciphertext = Base64Codec.encode(body.ciphertext), nonce = Base64Codec.encode(body.nonce),
            createdAt = "2026-10-19T12:00:00.000Z", keyEpoch = 0,
        )
        // An epoch this member was never given shows as unreadable, not as garbage.
        val unknownEpoch = dto.copy(id = "m10", keyEpoch = 5)
        server.dispatcher.let { inner ->
            server.dispatcher = object : Dispatcher() {
                override fun dispatch(request: RecordedRequest): MockResponse {
                    val path = request.path!!.substringBefore('?')
                    if (path == "/api/conversations/c1/messages" && request.method == "GET") {
                        return ok(json.encodeToString(kotlinx.serialization.builtins.ListSerializer(MessageDto.serializer()), listOf(dto, unknownEpoch)))
                    }
                    if (path == "/api/conversations/c1/receipts") return ok("[]")
                    return inner.dispatch(request)
                }
            }
        }
        val messages = repository.fetchMessages(meta, identity.userId).messages
        assertEquals("from the creator", messages.first { it.id == "m9" }.text)
        assertEquals(com.redvaultsystems.redvaultmessage.data.model.MessageKind.Undecryptable, messages.first { it.id == "m10" }.kind)
        assertNull(messages.first { it.id == "m9" }.ciphertextB64)
    }
}
