// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.DriveCrypto
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.VaultCrypto
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.int
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
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/** M-24, mirroring ui/src/test/passwordsScreen.test.tsx: the server here is the attacker, and answers whatever a test says. */
class VaultBindingTest {
    private val sodium = LazySodiumJava(SodiumJava())
    private val e2e = E2eCrypto(sodium)
    private val crypto = VaultCrypto(sodium, e2e, DriveCrypto(sodium, e2e))
    private val pair = e2e.generateKeyPair()
    private val identity = SelfIdentity("h", "a0000000-0000-4000-8000-000000000001", pair.publicKey, pair.privateKey, null, "user", "free", false)
    private val first = "10000000-0000-4000-8000-000000000001"
    private val second = "20000000-0000-4000-8000-000000000002"

    private class Item(val id: String, val ciphertext: String, val nonce: String, val revision: Int)

    private val items = ArrayList<Item>()
    private var sharedWithMe = """{"items":[]}"""
    private val writes = ArrayList<String>()
    private lateinit var server: MockWebServer
    private val revs = object : VaultRevs {
        private val seen = HashMap<String, Long>()
        override fun seen(userId: String, itemId: String) = seen[itemId] ?: 0
        override fun record(userId: String, itemId: String, rev: Long) {
            seen[itemId] = rev
        }
    }

    private fun json(body: Any) = MockResponse().setResponseCode(200).setHeader("Content-Type", "application/json").setBody(body.toString())

    private fun dto(item: Item) = buildJsonObject {
        put("id", item.id); put("ciphertext", item.ciphertext); put("nonce", item.nonce); put("revision", item.revision)
    }

    private val dispatcher = object : Dispatcher() {
        override fun dispatch(request: RecordedRequest): MockResponse {
            val path = request.path!!.removePrefix("/api/")
            val body = request.body.readUtf8().takeIf { it.isNotEmpty() }?.let { ApiClient.json.parseToJsonElement(it).jsonObject }
            return when {
                request.method == "GET" && path == "vault/items" -> json(buildJsonObject { put("items", buildJsonArray { items.forEach { add(dto(it)) } }) })
                request.method == "GET" && path == "vault/shares" -> json("""{"shares":[]}""")
                request.method == "GET" && path == "vault/shared-with-me" -> json(sharedWithMe)
                request.method == "POST" && path == "vault/items" -> {
                    writes += "create"
                    val item = Item(second, body!!.string("ciphertext"), body.string("nonce"), 1)
                    items += item
                    json(dto(item))
                }
                request.method == "PUT" && path.startsWith("vault/items/") -> {
                    val id = path.removePrefix("vault/items/")
                    writes += "update"
                    val item = Item(id, body!!.string("ciphertext"), body.string("nonce"), body.getValue("revision").jsonPrimitive.int + 1)
                    items.removeAll { it.id == id }
                    items += item
                    json(dto(item))
                }
                else -> MockResponse().setResponseCode(404)
            }
        }
    }

    private fun JsonObject.string(name: String) = getValue(name).jsonPrimitive.content

    private fun repository(): VaultRepository {
        val base = server.url("/api/").toString()
        return VaultRepository(ApiClient.createDrive(base, OkHttpClient()), ApiClient.create(base, OkHttpClient()), crypto, identity, revs)
    }

    private fun seal(id: String, title: String, boundId: String? = null, rev: Long? = null): Item {
        val data = buildJsonObject {
            put("type", "login"); put("title", title)
            boundId?.let { put("boundId", it) }
            rev?.let { put("rev", it) }
        }
        val key = crypto.vaultKey(identity.privateKey)
        val sealed = crypto.sealItem(data.toString(), key)
        return Item(id, Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce), 1)
    }

    private fun serve(vararg served: Item) {
        items.clear()
        items.addAll(served)
    }

    @Before
    fun setUp() {
        server = MockWebServer()
        server.dispatcher = dispatcher
        server.start()
    }

    @After
    fun tearDown() = server.shutdown()

    @Test
    fun controlAnItemFromBeforeBindingStillShows() = runBlocking {
        val repository = repository()
        serve(seal(first, "Old login"))
        assertEquals(listOf("Old login"), repository.list().map { it.data.string("title") })
        assertEquals(0, repository.refused)
    }

    @Test
    fun anItemInAnotherRecordsPlaceIsLeftOut() = runBlocking {
        val repository = repository()
        serve(seal(first, "Moved here", second, 1), seal(second, "In place", second, 2))
        assertEquals(listOf("In place"), repository.list().map { it.data.string("title") })
        assertEquals(1, repository.refused)
    }

    @Test
    fun anOlderCopyIsLeftOutAndSoIsAnUnboundOne() = runBlocking {
        val repository = repository()
        serve(seal(first, "Current", first, 5))
        assertEquals(1, repository.list().size)
        serve(seal(first, "Older", first, 4))
        assertTrue(repository.list().isEmpty())
        assertEquals(1, repository.refused)
        serve(seal(first, "Unbound"))
        assertTrue(repository.list().isEmpty())
        assertEquals(1, repository.refused)
    }

    @Test
    fun aNewItemIsCreatedThenBoundToTheIdTheServerGaveIt() = runBlocking {
        val repository = repository()
        val data = buildJsonObject { put("type", "login"); put("title", "New") }
        val saved = repository.save(VaultEntry(data = data))
        assertEquals(listOf("create", "update"), writes)
        assertEquals(second, saved.data.string("boundId"))
        assertEquals(1, saved.data.getValue("rev").jsonPrimitive.int)
        assertEquals(2, saved.revision)
        assertEquals(listOf("New"), repository.list().map { it.data.string("title") })
    }

    private fun shareCopy(title: String, rev: Long): String {
        val data = buildJsonObject { put("type", "login"); put("title", title); put("boundId", first); put("rev", rev) }
        val sealed = Base64Codec.encode(crypto.sealForRecipient(data.toString(), identity.publicKey))
        return """{"items":[{"itemId":"$first","ownerId":"$second","sealed":"$sealed"}]}"""
    }

    @Test
    fun aSharedItemCantGoBackToAnOlderCopyEither() = runBlocking {
        val repository = repository()
        sharedWithMe = shareCopy("Shared now", 3)
        assertEquals(listOf("Shared now"), repository.list().map { it.data.string("title") })
        sharedWithMe = shareCopy("Shared before", 2)
        assertTrue(repository.list().isEmpty())
        assertEquals(1, repository.refused)
    }

    @Test
    fun savingAnOldItemBindsItAndRaisesItsRevision() = runBlocking {
        val repository = repository()
        serve(seal(first, "Old login"))
        val entry = repository.list().single()
        val saved = repository.save(entry.copy(data = JsonObject(entry.data + ("title" to JsonPrimitive("Changed")))))
        assertEquals(first, saved.data.string("boundId"))
        assertEquals(1, saved.data.getValue("rev").jsonPrimitive.int)
        // What it was before the save can't come back now.
        serve(seal(first, "Old login"))
        assertFalse(repository.list().any { it.data.string("title") == "Old login" })
    }
}
