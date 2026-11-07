package com.redvaultsystems.redvaultmessage.data

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.GroupInviteCrypto
import com.redvaultsystems.redvaultmessage.crypto.SigningKeyPair
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.FriendDto
import com.redvaultsystems.redvaultmessage.data.api.KeyNoticeDto
import com.redvaultsystems.redvaultmessage.data.model.ConversationMeta
import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
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
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.util.concurrent.CopyOnWriteArrayList

/**
 * Key trust end to end through the real repositories, against a stand-in for the server that keeps notices the way
 * server/src/services/conversationService.ts does: in order, and only from the person they name. The server here is
 * the attacker wherever a test says so: it chooses what every call answers.
 */
class KeyTrustFlowTest {
    private val sodium = LazySodiumJava(SodiumJava())
    private val crypto = E2eCrypto(sodium)
    private lateinit var server: MockWebServer
    private val fake = FakeServer()

    private val dmId = "d0000000-0000-4000-8000-000000000001"
    private val groupId = "c0000000-0000-4000-8000-000000000001"

    private inner class Person(val userId: String) {
        val keys = crypto.generateKeyPair()
        val identity = SelfIdentity("h", userId, keys.publicKey, keys.privateKey, null, "user", "free", false)
        var signingKey: String? = null
        var since: String? = null
        val signer: SigningKeyPair get() = crypto.deriveSigningKeyPair(keys.privateKey)
    }

    /** One conversation as the server stores it; [wrapped] is each person's sealed copies, by epoch. */
    private inner class Chat(val id: String, val isGroup: Boolean) {
        val members = ArrayList<String>()
        var keyEpoch = 0
        var rekeyRequired = false
        var community: JsonObject? = null
        val wrapped = HashMap<String, MutableMap<Int, String>>()
        var keyNotice: JsonElement? = null
        val messages = ArrayList<JsonObject>()

        fun seal(person: Person, epoch: Int, key: ByteArray) {
            wrapped.getOrPut(person.userId) { HashMap() }[epoch] = Base64Codec.encode(crypto.sealConversationKey(key, person.keys.publicKey))
        }

        fun dto(userId: String) = buildJsonObject {
            val mine = wrapped.getValue(userId)
            put("id", id)
            put("isGroup", isGroup)
            put("role", "owner")
            put("wrappedKey", mine.getValue(keyEpoch))
            put("keyEpoch", keyEpoch)
            put("keys", buildJsonArray { mine.forEach { (e, k) -> add(buildJsonObject { put("epoch", e); put("wrappedKey", k) }) } })
            put("rekeyRequired", rekeyRequired)
            put("keyNotice", keyNotice ?: JsonNull)
            community?.let { put("community", it) }
            put("createdBy", members.first())
            put("createdAt", "2026-10-01T00:00:00.000Z")
        }
    }

    private inner class FakeServer : Dispatcher() {
        var caller = ""
        val people = LinkedHashMap<String, Person>()
        val chats = LinkedHashMap<String, Chat>()
        val requests = CopyOnWriteArrayList<String>()

        /** Answers for a path suffix, to make one kind of request fail. */
        var failNotice: Int? = null
        var staleNotice = false
        var failAdd = false
        val forgedPublicKeys = HashMap<String, String>()
        var saysNothingAboutSigning = false
        var answersFailForUsers = false
        var inviteConversation = ""

        private fun json(element: Any) = MockResponse().setResponseCode(200).setHeader("Content-Type", "application/json").setBody(element.toString())
        private fun error(status: Int, code: String) = MockResponse().setResponseCode(status).setBody("""{"error":"$code","message":"$code"}""")
        private val noContent get() = MockResponse().setResponseCode(204)

        override fun dispatch(request: RecordedRequest): MockResponse {
            val path = request.path!!.removePrefix("/api/").substringBefore("?")
            val text = request.body.readUtf8()
            requests += "${request.method} $path $text"
            val body = text.takeIf { it.isNotEmpty() }?.let { ApiClient.json.parseToJsonElement(it).jsonObject }
            val chatId = path.removePrefix("conversations/").substringBefore("/")
            val chat = chats[chatId]
            return when {
                request.method == "GET" && path == "conversations" ->
                    json(JsonArray(chats.values.filter { caller in it.members }.map { it.dto(caller) }))
                request.method == "GET" && path == "conversations/$chatId/participants" ->
                    json(JsonArray(chat!!.members.map { buildJsonObject { put("userId", it); put("role", "member"); put("joinedAt", "2026-10-01T00:00:00.000Z") } }))
                request.method == "GET" && path.startsWith("users/") -> {
                    val id = path.removePrefix("users/")
                    if (answersFailForUsers) return error(503, "unavailable")
                    val p = people.getValue(id.lowercase())
                    json(buildJsonObject {
                        put("userId", p.userId); put("username", "u"); put("publicKey", forgedPublicKeys[p.userId] ?: Base64Codec.encode(p.keys.publicKey))
                        put("createdAt", "2026-10-01T00:00:00.000Z")
                        // Asked in capitals, the server answers with whatever key suits it.
                        put("signingPublicKey", if (saysNothingAboutSigning) null else if (id != id.lowercase()) forgedSigningKey else p.signingKey)
                        put("signingKeySince", if (saysNothingAboutSigning) null else p.since)
                    })
                }
                request.method == "PUT" && path == "auth/signing-key" -> {
                    val p = people.getValue(caller)
                    p.signingKey = body!!.getValue("signingPublicKey").jsonPrimitive.content
                    if (p.since == null) p.since = "2026-10-01"
                    noContent
                }
                request.method == "PUT" && path == "conversations/$chatId/key-notice" -> {
                    failNotice?.let { return error(it, "unavailable") }
                    if (staleNotice) return error(409, "stale_key_notice")
                    chat!!.keyNotice = body
                    noContent
                }
                request.method == "POST" && path == "conversations/$chatId/keys" -> {
                    if (body!!.getValue("fromEpoch").jsonPrimitive.int != chat!!.keyEpoch) return error(409, "stale_key_epoch")
                    val given = body.getValue("keys").jsonArray.map { it.jsonObject }
                    if (given.map { it.getValue("userId").jsonPrimitive.content }.sorted() != chat.members.sorted()) return error(409, "membership_changed")
                    if (chat.keyNotice != null && body["keyNotice"] == null) return error(426, "client_update_required")
                    chat.keyEpoch++
                    chat.rekeyRequired = false
                    for (k in given) chat.wrapped.getValue(k.getValue("userId").jsonPrimitive.content)[chat.keyEpoch] = k.getValue("wrappedKey").jsonPrimitive.content
                    body["keyNotice"]?.let { chat.keyNotice = it }
                    json(buildJsonObject { put("keyEpoch", chat.keyEpoch) })
                }
                request.method == "POST" && path == "conversations/$chatId/participants" -> {
                    if (failAdd) return error(403, "not_addable")
                    val userId = body!!.getValue("userId").jsonPrimitive.content
                    chat!!.members += userId
                    chat.wrapped.getOrPut(userId) { HashMap() }[body.getValue("keyEpoch").jsonPrimitive.int] = body.getValue("wrappedKey").jsonPrimitive.content
                    body["keyNotice"]?.let { chat.keyNotice = it }
                    noContent
                }
                request.method == "POST" && path.startsWith("group-invites/groups/") && path.endsWith("/approve") -> {
                    if (failAdd) return error(403, "not_addable")
                    val parts = path.split("/")
                    val approving = chats.getValue(parts[2])
                    val userId = parts[4]
                    approving.members += userId
                    approving.wrapped.getOrPut(userId) { HashMap() }[body!!.getValue("keyEpoch").jsonPrimitive.int] = body.getValue("wrappedKey").jsonPrimitive.content
                    body["keyNotice"]?.let { approving.keyNotice = it }
                    noContent
                }
                request.method == "POST" && path.endsWith("/join") -> {
                    val joining = chats.getValue(inviteConversation)
                    joining.members += caller
                    joining.wrapped.getOrPut(caller) { HashMap() }[joining.keyEpoch] = body!!.getValue("wrappedKey").jsonPrimitive.content
                    json(buildJsonObject { put("status", "joined"); put("conversationId", inviteConversation) })
                }
                request.method == "POST" && path == "conversations/$chatId/messages" -> {
                    if (chat!!.rekeyRequired) return error(409, "group_rekey_required")
                    val message = message(chat, caller, body!!.getValue("ciphertext").jsonPrimitive.content, body.getValue("nonce").jsonPrimitive.content, body["signature"]?.jsonPrimitive?.content)
                    chat.messages += message
                    json(message)
                }
                request.method == "GET" && path == "conversations/$chatId/messages" -> json(JsonArray(chat!!.messages))
                request.method == "GET" && path == "conversations/$chatId/receipts" -> json(JsonArray(emptyList()))
                else -> error(404, "not_found")
            }
        }

        var seq = 0

        fun message(chat: Chat, senderId: String, ciphertext: String, nonce: String, signature: String?, epoch: Int = chat.keyEpoch, createdAt: String? = null) = buildJsonObject {
            put("id", "e0000000-0000-4000-8000-%012d".format(++seq))
            put("conversationId", chat.id)
            put("senderId", senderId)
            put("ciphertext", ciphertext)
            put("nonce", nonce)
            put("createdAt", createdAt ?: "2026-10-02T00:00:%02d.000Z".format(seq))
            put("keyEpoch", epoch)
            put("signature", signature)
        }
    }

    private var forgedSigningKey: String? = null

    private lateinit var alice: Person
    private lateinit var bob: Person
    private lateinit var carol: Person
    private lateinit var eve: Person
    private lateinit var dmKey: ByteArray
    private lateinit var groupKey: ByteArray
    private lateinit var dm: Chat
    private lateinit var group: Chat

    @Before
    fun setUp() {
        server = MockWebServer()
        server.dispatcher = fake
        server.start()
        alice = Person("a0000000-0000-4000-8000-000000000001")
        bob = Person("b0000000-0000-4000-8000-000000000002")
        carol = Person("c0000000-0000-4000-8000-000000000003")
        eve = Person("e0000000-0000-4000-8000-000000000004")
        for (p in listOf(alice, bob, carol, eve)) fake.people[p.userId] = p
        dmKey = crypto.generateConversationKey()
        groupKey = crypto.generateConversationKey()
        dm = Chat(dmId, false).also { fake.chats[dmId] = it }
        group = Chat(groupId, true).also { fake.chats[groupId] = it }
        for (p in listOf(alice, bob)) {
            dm.members += p.userId
            dm.seal(p, 0, dmKey)
        }
        for (p in listOf(alice, bob, carol)) {
            group.members += p.userId
            group.seal(p, 0, groupKey)
        }
    }

    @After
    fun tearDown() = server.shutdown()

    /** One phone of [who]: its own pins, like the app after sign-in. */
    private inner class Device(val who: Person, val keyPins: MemoryKeyPins = MemoryKeyPins(), signingPins: MemoryPins = MemoryPins()) {
        private val base = server.url("/api/").toString()
        private val api = ApiClient.create(base, OkHttpClient())
        val contactKeys = ContactKeyPins(object : ContactKeyPins.Storage {
            private val stored = HashMap<String, String>()
            override fun get(name: String) = stored[name]
            override fun put(name: String, value: String) {
                stored[name] = value
            }
        })
        val keys = MessageKeys(api, crypto, signingPins, KeyTrust(crypto, keyPins, contactKeys)).also { it.begin(who.identity) }
        val conversations = ConversationRepository(api, crypto, null, keys)
        val groups = GroupRepository(api, crypto, keys)
        val invites = GroupInviteRepository(ApiClient.createGroupInvites(base, OkHttpClient()), api, GroupInviteCrypto(sodium, crypto), crypto, "https://example.test", keys)

        suspend fun meta(id: String): ConversationMeta {
            fake.caller = who.userId
            val dto = conversations.listConversations().single { it.id == id }
            return conversations.hydrateMeta(dto, who.identity)!!
        }

        suspend fun read(id: String): List<Message> {
            val meta = meta(id)
            fake.caller = who.userId
            return conversations.fetchMessages(meta, who.userId).messages
        }

        suspend fun send(id: String, text: String): Message {
            val meta = meta(id)
            fake.caller = who.userId
            return conversations.send(meta, text)
        }

        fun pin(id: String) = keyPins.load(who.userId, id)
    }

    private class MemoryPins : SigningKeyPins {
        val pinned = HashMap<String, String>()
        override fun pin(selfUserId: String, userId: String, keyB64: String) = pinned.getOrPut("$selfUserId:$userId") { keyB64 }
        override fun pinned(selfUserId: String, userId: String) = pinned["$selfUserId:$userId"]
    }

    private fun sent(fragment: String) = fake.requests.filter { fragment in it }
    private fun storedNotice() = ApiClient.json.decodeFromJsonElement(KeyNoticeDto.serializer(), group.keyNotice!!)

    private fun signedNotice(signer: SigningKeyPair, issuer: String, chat: Chat, epoch: Long, seq: Long, members: List<String>, key: ByteArray): JsonElement {
        val input = E2eCrypto.keyNoticeInput(chat.id, issuer, epoch, seq, members, crypto.keyCommitment(key))
        val notice = KeyNoticeDto(epoch, seq, issuer, members, Base64Codec.encode(crypto.signMessage(input, signer.secretKey)))
        return ApiClient.json.encodeToJsonElement(KeyNoticeDto.serializer(), notice)
    }

    private fun friend(person: Person) = FriendDto(person.userId, "friend", Base64Codec.encode(person.keys.publicKey))

    // --- Direct chats ---

    @Test
    fun controlADirectChatPinsItsKeyAndKeepsWorking() = runBlocking {
        val device = Device(alice)
        device.send(dmId, "hello")
        assertNotNull(device.pin(dmId))
        assertNull(device.conversations.keyProblem(dmId))
        assertEquals(listOf("hello"), device.read(dmId).map { it.text })
    }

    @Test
    fun aServerChosenKeyAtANewEpochIsRefusedAndNothingCanBeSent() = runBlocking {
        val device = Device(alice)
        device.send(dmId, "real")
        val attackerKey = crypto.generateConversationKey()
        dm.keyEpoch = 1
        dm.seal(alice, 1, attackerKey)

        val meta = device.meta(dmId)
        assertEquals(KeyTrust.DIRECT_KEY_CHANGED, device.conversations.keyProblem(dmId))
        assertFalse(meta.conversationKey.contentEquals(attackerKey))
        assertFalse(device.keys.ringFor(dmId)!!.keys.values.any { it.contentEquals(attackerKey) })

        val stored = dm.messages.size
        val attempts = sent("POST conversations/$dmId/messages").size
        fake.caller = alice.userId
        val refused = assertThrows(UserFacingException::class.java) { runBlocking { device.conversations.send(meta, "secret") } }
        assertEquals(KeyTrust.DIRECT_KEY_CHANGED, refused.message)
        assertEquals("nothing reached the server", attempts, sent("POST conversations/$dmId/messages").size)
        assertEquals(stored, dm.messages.size)
    }

    @Test
    fun aRefusedDirectKeyStillReadsHistoryOnlyUnderThePinnedKey() = runBlocking {
        val device = Device(alice)
        device.send(dmId, "real")
        dm.keyEpoch = 1
        dm.seal(alice, 1, crypto.generateConversationKey())
        assertEquals(listOf("real"), device.read(dmId).map { it.text })
        // The server also swaps the key it lists at epoch 0: that one isn't the pinned key, so it isn't used.
        dm.seal(alice, 0, crypto.generateConversationKey())
        val shown = device.read(dmId)
        assertEquals(listOf("Couldn't decrypt this message."), shown.map { it.text })
        assertTrue(device.keys.ringFor(dmId)!!.keys.isEmpty())
    }

    @Test
    fun aDifferentKeyAtEpochZeroIsDroppedSoItCantReadOrForge() = runBlocking {
        val device = Device(alice)
        device.send(dmId, "real")
        val attackerKey = crypto.generateConversationKey()
        dm.seal(alice, 0, attackerKey)
        val forged = crypto.encryptWithConversationKey("forged", attackerKey)
        dm.messages += fake.message(dm, bob.userId, Base64Codec.encode(forged.ciphertext), Base64Codec.encode(forged.nonce), null)

        val shown = device.read(dmId)
        assertEquals(KeyTrust.DIRECT_KEY_CHANGED, device.conversations.keyProblem(dmId))
        assertFalse(shown.any { it.text == "forged" })
        assertTrue(device.keys.ringFor(dmId)!!.keys.isEmpty())
    }

    @Test
    fun aKeyListedUnderAnExtraEpochOfADirectChatIsNeverUsed() = runBlocking {
        val device = Device(alice)
        device.send(dmId, "real")
        val attackerKey = crypto.generateConversationKey()
        // keyEpoch stays 0, so nothing looks wrong: the extra key just sits in the list, for a forged message to name.
        dm.seal(alice, 7, attackerKey)
        val forged = crypto.encryptWithConversationKey("forged", attackerKey)
        dm.messages += fake.message(dm, bob.userId, Base64Codec.encode(forged.ciphertext), Base64Codec.encode(forged.nonce), null, epoch = 7)
        assertEquals(listOf("real", "Couldn't decrypt this message."), device.read(dmId).map { it.text })
        assertNull(device.conversations.keyProblem(dmId))
    }

    // --- Publishing notices ---

    @Test
    fun aGroupWithNoNoticeGetsItsFirstOneAndOtherMembersAcceptIt() = runBlocking {
        val aliceDevice = Device(alice)
        aliceDevice.meta(groupId)
        assertEquals(1, sent("PUT conversations/$groupId/key-notice").size)
        assertEquals(1L, aliceDevice.pin(groupId)?.seq)
        assertEquals(listOf(alice.userId, bob.userId, carol.userId).sorted(), storedNotice().members)

        val bobDevice = Device(bob)
        bobDevice.meta(groupId)
        assertNull(bobDevice.conversations.keyProblem(groupId))
        assertEquals(1, sent("PUT conversations/$groupId/key-notice").size)
        assertEquals(storedNotice().seq, bobDevice.pin(groupId)?.seq)
    }

    @Test
    fun aFailedPublishPinsNothingSoItCantLockTheGroup() = runBlocking {
        fake.failNotice = 503
        val device = Device(alice)
        device.send(groupId, "still works")
        assertNull(device.pin(groupId))
        assertNull(device.conversations.keyProblem(groupId))
        assertEquals(1, group.messages.size)
    }

    @Test
    fun losingThePublishRacePinsNothingAndTheWinnerIsPickedUp() = runBlocking {
        fake.staleNotice = true
        val device = Device(alice)
        device.meta(groupId)
        assertNull(device.pin(groupId))
        // Bob's notice, the one that won, is there on the next load.
        fake.staleNotice = false
        val bobSigner = bob.signer
        bob.signingKey = Base64Codec.encode(bobSigner.publicKey)
        group.keyNotice = signedNotice(bobSigner, bob.userId, group, 0, 1, group.members.toList(), groupKey)
        device.meta(groupId)
        assertNull(device.conversations.keyProblem(groupId))
        assertEquals(1L, device.pin(groupId)?.seq)
    }

    @Test
    fun aRekeyCarriesItsNoticeAndTheOthersVerifyIt() = runBlocking {
        val aliceDevice = Device(alice)
        val bobDevice = Device(bob)
        aliceDevice.meta(groupId)
        bobDevice.meta(groupId)
        group.members -= carol.userId
        group.wrapped.remove(carol.userId)
        group.rekeyRequired = true

        fake.caller = alice.userId
        aliceDevice.keys.rotate(groupId)
        assertEquals(1, group.keyEpoch)
        val notice = storedNotice()
        assertEquals(1L, notice.epoch)
        assertFalse(carol.userId in notice.members)
        assertEquals(1L, aliceDevice.pin(groupId)?.epoch)

        bobDevice.meta(groupId)
        assertNull(bobDevice.conversations.keyProblem(groupId))
        assertEquals(1L, bobDevice.pin(groupId)?.epoch)
        assertEquals(listOf(alice.userId, bob.userId).sorted(), bobDevice.pin(groupId)?.members)
    }

    @Test
    fun aRekeyRefusesWhenTheServerListsSomeoneNoNoticeNamed() = runBlocking {
        val device = Device(alice)
        device.meta(groupId)
        val before = sent("POST conversations/$groupId/keys").size
        // The server slips Eve in, then asks for a new key so she gets a copy of it.
        group.members += eve.userId
        group.seal(eve, 0, groupKey)
        group.rekeyRequired = true

        fake.caller = alice.userId
        val refused = assertThrows(UserFacingException::class.java) { runBlocking { device.keys.rotate(groupId) } }
        assertEquals(KeyTrust.UNCONFIRMED_MEMBER, refused.message)
        assertEquals(before, sent("POST conversations/$groupId/keys").size)
        assertEquals(0, group.keyEpoch)
    }

    @Test
    fun aRekeyRefusesToSealTheNewKeyToASwappedMemberKey() = runBlocking {
        val device = Device(alice)
        device.meta(groupId)
        device.contactKeys.accept(alice.userId, bob.userId, Base64Codec.encode(bob.keys.publicKey))
        fake.forgedPublicKeys[bob.userId] = Base64Codec.encode(crypto.generateKeyPair().publicKey)
        group.rekeyRequired = true
        fake.caller = alice.userId
        val refused = assertThrows(UserFacingException::class.java) { runBlocking { device.keys.rotate(groupId) } }
        assertEquals(KeyTrust.CONTACT_KEY_CHANGED, refused.message)
        assertTrue(sent("POST conversations/$groupId/keys").isEmpty())
    }

    @Test
    fun anAddOrApprovalRefusesToSealToASwappedKey() = runBlocking {
        val device = Device(alice)
        val meta = device.meta(groupId)
        device.contactKeys.accept(alice.userId, eve.userId, Base64Codec.encode(eve.keys.publicKey))
        fake.forgedPublicKeys[eve.userId] = Base64Codec.encode(crypto.generateKeyPair().publicKey)
        fake.caller = alice.userId
        assertThrows(UserFacingException::class.java) { runBlocking { device.invites.approve(meta, eve.userId) } }
        assertTrue(sent("/approve").isEmpty())
        val swapped = FriendDto(eve.userId, "friend", fake.forgedPublicKeys.getValue(eve.userId))
        assertThrows(UserFacingException::class.java) { runBlocking { device.groups.add(meta, swapped) } }
        assertTrue(sent("POST conversations/$groupId/participants").isEmpty())
    }

    @Test
    fun aKeyListedUnderAFutureEpochIsNeverUsed() = runBlocking {
        val device = Device(alice)
        group.seal(alice, 999, crypto.generateConversationKey())
        device.meta(groupId)
        assertFalse(999 in device.keys.ringFor(groupId)!!.keys)
    }

    @Test
    fun aPhoneWithNoPinDoesntVouchForALaterKey() = runBlocking {
        group.keyEpoch = 1
        group.seal(alice, 1, crypto.generateConversationKey())
        group.keyNotice = null
        Device(alice).meta(groupId)
        assertTrue(sent("conversations/$groupId/key-notice").isEmpty())
    }

    @Test
    fun aLinkJoinerWontVouchForANoticeNoMemberSigned() = runBlocking {
        Device(alice).meta(groupId)
        val genuine = storedNotice()
        val published = sent("conversations/$groupId/key-notice").size
        val attacker = crypto.deriveSigningKeyPair(crypto.generateConversationKey())
        val ghost = "99999999-9999-4999-8999-999999999999"
        // Signed with a key the attacker made up, for a group key it doesn't have.
        group.keyNotice = signedNotice(attacker, bob.userId, group, genuine.epoch, genuine.seq, genuine.members + ghost, crypto.generateConversationKey()).jsonObject
        fake.inviteConversation = groupId
        fake.caller = eve.userId
        val result = Device(eve).invites.join(eve.identity, preview(groupKey))
        assertEquals("joined", result.status)
        assertEquals(published, sent("conversations/$groupId/key-notice").size)
    }

    @Test
    fun aPinnedChatCantBeTurnedIntoACommunity() = runBlocking {
        val device = Device(alice)
        device.meta(dmId)
        dm.keyEpoch = 1
        dm.seal(alice, 1, crypto.generateConversationKey())
        dm.community = buildJsonObject { put("id", "x"); put("name", "x"); put("kind", "group"); put("status", "active"); put("role", "member"); put("canPost", true); put("keyEpoch", 0); put("rekeyRequired", false) }
        val meta = device.meta(dmId)
        assertNull(meta.community)
        assertEquals(KeyTrust.DIRECT_KEY_CHANGED, device.keys.ringFor(dmId)?.keyProblem)
    }

    @Test
    fun aRoutineAddDoesntConfirmSomeoneTheServerSlippedIn() = runBlocking {
        val device = Device(alice)
        val meta = device.meta(groupId)
        val ghost = "99999999-9999-4999-8999-999999999999"
        group.members.add(ghost)
        fake.caller = alice.userId
        device.groups.add(meta, friend(eve))
        val named = storedNotice().members
        assertTrue(eve.userId in named)
        assertFalse(ghost in named)
        assertFalse(ghost in device.pin(groupId)!!.members)
    }

    @Test
    fun aMemberAddedInTheOpenIsConfirmedByTheNoticeSoTheNextRekeyGoesThrough() = runBlocking {
        val device = Device(alice)
        val meta = device.meta(groupId)
        fake.caller = alice.userId
        device.groups.add(meta, friend(eve))
        assertTrue(sent("POST conversations/$groupId/participants").single().contains("keyNotice"))
        assertTrue(eve.userId in device.pin(groupId)!!.members)
        assertEquals(2L, storedNotice().seq)

        group.rekeyRequired = true
        fake.caller = alice.userId
        device.keys.rotate(groupId)
        assertEquals(1, group.keyEpoch)
    }

    @Test
    fun aFailedAddPinsNothingNew() = runBlocking {
        val device = Device(alice)
        val meta = device.meta(groupId)
        val before = device.pin(groupId)
        fake.failAdd = true
        fake.caller = alice.userId
        assertTrue(runCatching { device.groups.add(meta, friend(eve)) }.isFailure)
        assertEquals(before, device.pin(groupId))
    }

    @Test
    fun anApprovalCarriesTheEpochAndTheNotice() = runBlocking {
        val device = Device(alice)
        val meta = device.meta(groupId)
        fake.caller = alice.userId
        device.invites.approve(meta, eve.userId)
        val request = sent("/approve").single()
        assertTrue(request, request.contains("keyNotice") && request.contains("keyEpoch"))
        assertTrue(eve.userId in device.pin(groupId)!!.members)
    }

    @Test
    fun aFailedApprovalPinsNothingNew() = runBlocking {
        val device = Device(alice)
        val meta = device.meta(groupId)
        val before = device.pin(groupId)
        fake.failAdd = true
        fake.caller = alice.userId
        assertTrue(runCatching { device.invites.approve(meta, eve.userId) }.isFailure)
        assertEquals(before, device.pin(groupId))
    }

    private fun preview(key: ByteArray?) = InvitePreview("link1", groupId, "Group", 3, false, key, ByteArray(32) { 3 }, member = false, requested = false)

    @Test
    fun someoneJoiningByLinkPublishesTheirOwnNotice() = runBlocking {
        Device(alice).meta(groupId)
        val before = storedNotice()
        fake.inviteConversation = groupId
        fake.caller = eve.userId
        val device = Device(eve)
        val result = device.invites.join(eve.identity, preview(groupKey))
        assertEquals("joined", result.status)
        val notice = storedNotice()
        assertEquals(eve.userId, notice.issuerId)
        assertEquals(before.seq + 1, notice.seq)
        assertTrue(eve.userId in notice.members)
        assertTrue(eve.userId in device.pin(groupId)!!.members)
    }

    @Test
    fun aLinkJoinerWhoLosesThePublishRaceIsStillIn() = runBlocking {
        Device(alice).meta(groupId)
        fake.inviteConversation = groupId
        fake.staleNotice = true
        fake.caller = eve.userId
        val device = Device(eve)
        assertEquals("joined", device.invites.join(eve.identity, preview(groupKey)).status)
        assertNull(device.pin(groupId))
    }

    @Test
    fun aFailedNoticeNeverBlocksJoining() = runBlocking {
        fake.inviteConversation = groupId
        fake.failNotice = 500
        fake.caller = eve.userId
        val device = Device(eve)
        assertEquals("joined", device.invites.join(eve.identity, preview(groupKey)).status)
        assertTrue(eve.userId in group.members)
        assertNull(device.pin(groupId))
    }

    // --- Checking notices through the ring ---

    @Test
    fun aSubstitutedGroupKeyIsDroppedSendingStopsAndOlderMessagesStillRead() = runBlocking {
        val device = Device(alice)
        device.send(groupId, "before")
        assertEquals(listOf("before"), device.read(groupId).map { it.text })
        // No member made this: the server has no signing key of theirs, so it can only leave the notice as it was.
        val attackerKey = crypto.generateConversationKey()
        group.keyEpoch = 1
        group.seal(alice, 1, attackerKey)

        val meta = device.meta(groupId)
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, device.conversations.keyProblem(groupId))
        assertFalse(device.keys.ringFor(groupId)!!.keys.containsKey(1))
        assertFalse(meta.conversationKey.contentEquals(attackerKey))
        assertEquals(listOf("before"), device.read(groupId).map { it.text })

        val stored = group.messages.size
        fake.caller = alice.userId
        val refused = assertThrows(UserFacingException::class.java) { runBlocking { device.conversations.send(meta, "secret") } }
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, refused.message)
        assertEquals(stored, group.messages.size)
    }

    @Test
    fun aRemovedNoticeStopsTheGroupOnceOneHasBeenSeen() = runBlocking {
        val device = Device(alice)
        device.meta(groupId)
        group.keyNotice = null
        device.meta(groupId)
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, device.conversations.keyProblem(groupId))
        // It isn't quietly replaced with a fresh one either.
        assertEquals(1, sent("PUT conversations/$groupId/key-notice").size)
    }

    @Test
    fun theRefusalLiftsOnceTheRealNoticeIsBack() = runBlocking {
        val device = Device(alice)
        device.meta(groupId)
        val real = group.keyNotice
        group.keyNotice = null
        device.meta(groupId)
        group.keyNotice = real
        device.meta(groupId)
        assertNull(device.conversations.keyProblem(groupId))
    }

    @Test
    fun anInviteLinkCantHandOutAKeyThatWasRefused() = runBlocking {
        val device = Device(alice)
        val meta = device.meta(groupId)
        group.keyEpoch = 1
        group.seal(alice, 1, crypto.generateConversationKey())
        device.meta(groupId)
        val refused = assertThrows(UserFacingException::class.java) { runBlocking { device.invites.approve(meta, eve.userId) } }
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, refused.message)
        assertTrue(sent("/approve").isEmpty())
    }

    @Test
    fun aCachedNoSigningKeyYetDoesNotStick() = runBlocking {
        // Bob published a notice, but his key isn't on the server when Alice first looks...
        val bobSigner = bob.signer
        group.keyNotice = signedNotice(bobSigner, bob.userId, group, 0, 1, group.members.toList(), groupKey)
        val device = Device(alice)
        device.meta(groupId)
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, device.conversations.keyProblem(groupId))
        // ...then it is: the same session looks again and the group opens.
        bob.signingKey = Base64Codec.encode(bobSigner.publicKey)
        device.meta(groupId)
        assertNull(device.conversations.keyProblem(groupId))
        assertEquals(1L, device.pin(groupId)?.seq)
    }

    @Test
    fun anIssuerIdInCapitalsCantDodgeTheSigningKeyPin() = runBlocking {
        val bobSigner = bob.signer
        bob.signingKey = Base64Codec.encode(bobSigner.publicKey)
        group.keyNotice = signedNotice(bobSigner, bob.userId, group, 0, 1, group.members.toList(), groupKey)
        val device = Device(alice)
        device.meta(groupId)
        assertNull(device.conversations.keyProblem(groupId))

        // A new epoch "from Bob", spelled in capitals so it looks like someone with nothing pinned, under a signing key
        // the server made up: everything lowercases to Bob, who is in the verified list, except the key that signed it.
        val attacker = crypto.deriveSigningKeyPair(crypto.generateConversationKey())
        forgedSigningKey = Base64Codec.encode(attacker.publicKey)
        val attackerKey = crypto.generateConversationKey()
        group.keyEpoch = 1
        group.seal(alice, 1, attackerKey)
        group.keyNotice = signedNotice(attacker, bob.userId.uppercase(), group, 1, 1, group.members.toList(), attackerKey)

        device.meta(groupId)
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, device.conversations.keyProblem(groupId))
        assertFalse(device.keys.ringFor(groupId)!!.keys.containsKey(1))
    }

    @Test
    fun whatTheServerSaysAboutOurOwnSigningKeyDoesNotMatter() = runBlocking {
        val device = Device(alice)
        device.meta(groupId)
        alice.signingKey = null
        val second = Device(alice)
        second.meta(groupId)
        assertNull(second.conversations.keyProblem(groupId))
    }

    // --- M-3 and M-2 ---

    private fun encrypted(text: String, key: ByteArray) = crypto.encryptWithConversationKey(text, key)

    @Test
    fun anUnsignedMessageFromSomeoneKnownToSignIsUnverifiedInADirectChatToo() = runBlocking {
        Device(bob).send(dmId, "signed")
        val stripped = encrypted("stripped", dmKey)
        dm.messages += fake.message(dm, bob.userId, Base64Codec.encode(stripped.ciphertext), Base64Codec.encode(stripped.nonce), null)
        val shown = Device(alice).read(dmId)
        assertEquals(listOf("signed" to false, "stripped" to true), shown.map { it.text to it.unverifiedSender })
    }

    @Test
    fun aPinnedSignerStaysASignerWhenTheServerSaysNothingOrFails() = runBlocking {
        Device(bob).send(dmId, "signed")
        val pins = MemoryPins()
        Device(alice, signingPins = pins).read(dmId)
        val stripped = encrypted("stripped", dmKey)
        dm.messages += fake.message(dm, bob.userId, Base64Codec.encode(stripped.ciphertext), Base64Codec.encode(stripped.nonce), null)
        fake.saysNothingAboutSigning = true
        assertEquals(listOf("signed" to false, "stripped" to true), Device(alice, signingPins = pins).read(dmId).map { it.text to it.unverifiedSender })
        fake.saysNothingAboutSigning = false
        fake.answersFailForUsers = true
        val device = Device(alice, signingPins = pins)
        val dto = device.conversations.listConversations().single { it.id == dmId }
        device.keys.resolveSenderKeys(alice.userId, listOf(bob.userId))
        assertNotNull(dto)
        assertNotNull(device.keys.senderSigningKey(bob.userId))
    }

    @Test
    fun aSignatureForAnotherChatDoesntCountHere() = runBlocking {
        Device(bob).send(dmId, "signed")
        val moved = encrypted("moved", dmKey)
        val input = E2eCrypto.messageSignatureInput(groupId, bob.userId, 0, moved.nonce, moved.ciphertext)
        val signature = Base64Codec.encode(crypto.signMessage(input, bob.signer.secretKey))
        // The server says it belongs to the group it was signed for, and shows it in the direct chat.
        dm.messages += fake.message(group, bob.userId, Base64Codec.encode(moved.ciphertext), Base64Codec.encode(moved.nonce), signature, 0)
        assertFalse(Device(alice).read(dmId).any { it.text == "moved" })
    }

    @Test
    fun controlAMessageFromBeforeTheyStartedSigningStaysLegacyInADirectChat() = runBlocking {
        Device(bob).send(dmId, "signed")
        val old = encrypted("from before", dmKey)
        dm.messages.add(0, fake.message(dm, bob.userId, Base64Codec.encode(old.ciphertext), Base64Codec.encode(old.nonce), null, createdAt = "2026-09-01T00:00:00.000Z"))
        val shown = Device(alice).read(dmId)
        assertEquals(listOf("from before" to false, "signed" to false), shown.map { it.text to it.unverifiedSender })
    }

    @Test
    fun anUnsignedReactionFromASignerInADirectChatIsDropped() = runBlocking {
        Device(bob).send(dmId, "signed")
        val target = dm.messages.single().getValue("id").jsonPrimitive.content
        val reaction = encrypted(com.redvaultsystems.redvaultmessage.data.model.RichMessage.serializeReaction(target, "👍"), dmKey)
        dm.messages += fake.message(dm, bob.userId, Base64Codec.encode(reaction.ciphertext), Base64Codec.encode(reaction.nonce), null)
        assertTrue(Device(alice).read(dmId).single().reactions.isEmpty())
    }

    @Test
    fun aReplayUnderAnotherIdShowsOnce() = runBlocking {
        Device(bob).send(dmId, "pay me once")
        val original = dm.messages.single()
        dm.messages += fake.message(
            dm, bob.userId, original.getValue("ciphertext").jsonPrimitive.content, original.getValue("nonce").jsonPrimitive.content,
            original.getValue("signature").jsonPrimitive.content,
        )
        assertEquals(2, dm.messages.size)
        val reader = Device(alice)
        val meta = reader.meta(dmId)
        fake.caller = alice.userId
        val result = reader.conversations.fetchMessages(meta, alice.userId)
        assertEquals(listOf("pay me once"), result.messages.map { it.text })
        assertEquals("a replay doesn't count as unread either", 1, result.unread)
    }

    @Test
    fun aReplayClaimingAnotherEpochNeverShowsItsContentTwice() = runBlocking {
        val device = Device(alice)
        device.send(groupId, "only once")
        group.members -= carol.userId
        group.wrapped.remove(carol.userId)
        group.rekeyRequired = true
        fake.caller = alice.userId
        device.keys.rotate(groupId)
        val original = group.messages.single()
        // The same bytes, said to be from the new epoch: its key doesn't open them.
        group.messages += fake.message(
            group, alice.userId, original.getValue("ciphertext").jsonPrimitive.content, original.getValue("nonce").jsonPrimitive.content, null, epoch = 1,
        )
        assertEquals(1, device.read(groupId).count { it.text == "only once" })
    }

    @Test
    fun damagedMessagesThatShareANonceEachStillShowTheirError() = runBlocking {
        val nonce = Base64Codec.encode(ByteArray(24))
        dm.messages += fake.message(dm, bob.userId, Base64Codec.encode(ByteArray(40) { 1 }), nonce, null)
        dm.messages += fake.message(dm, bob.userId, Base64Codec.encode(ByteArray(40) { 2 }), nonce, null)
        assertEquals(listOf("Couldn't decrypt this message.", "Couldn't decrypt this message."), Device(alice).read(dmId).map { it.text })
    }

    @Test
    fun controlSameCiphertextUnderDifferentNoncesIsNotAReplay() = runBlocking {
        val ciphertext = Base64Codec.encode(ByteArray(40) { 1 })
        dm.messages += fake.message(dm, bob.userId, ciphertext, Base64Codec.encode(ByteArray(24) { 1 }), null)
        dm.messages += fake.message(dm, bob.userId, ciphertext, Base64Codec.encode(ByteArray(24) { 2 }), null)
        assertEquals(2, Device(alice).read(dmId).size)
    }

    @Test
    fun anotherBase64SpellingOfTheSameBytesIsStillTheSameMessage() = runBlocking {
        Device(bob).send(dmId, "once")
        val original = dm.messages.single()
        val ciphertext = original.getValue("ciphertext").jsonPrimitive.content
        // "once" makes 20 bytes of ciphertext, so its last base64 character has two bits that decode to nothing.
        val alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
        val last = ciphertext.trimEnd('=').length - 1
        val respelled = ciphertext.substring(0, last) + alphabet[alphabet.indexOf(ciphertext[last]) or 1] + ciphertext.substring(last + 1)
        assertNotEquals(ciphertext, respelled)
        assertTrue(Base64Codec.decode(ciphertext).contentEquals(Base64Codec.decode(respelled)))
        dm.messages += fake.message(dm, bob.userId, respelled, original.getValue("nonce").jsonPrimitive.content, original.getValue("signature").jsonPrimitive.content)
        assertEquals(listOf("once"), Device(alice).read(dmId).map { it.text })
    }
}
