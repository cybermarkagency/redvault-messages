// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.KdfSettings
import com.redvaultsystems.redvaultmessage.crypto.UnsafeKdfException
import com.redvaultsystems.redvaultmessage.crypto.WrappedPrivateKey
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ChangePasswordRequest
import com.redvaultsystems.redvaultmessage.data.api.LoginResponse
import com.redvaultsystems.redvaultmessage.data.api.PreLoginResult
import com.redvaultsystems.redvaultmessage.data.api.RecoveryCompleteRequest
import com.redvaultsystems.redvaultmessage.data.api.RegisterRequest
import com.redvaultsystems.redvaultmessage.data.api.toLoginResult
import kotlinx.coroutines.runBlocking
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/** Key-stretching settings (M-1): the same floor and ceiling as ui/src/lib/e2eCrypto.ts, before anything is derived. */
class KdfSettingsTest {
    private val crypto = E2eCrypto(LazySodiumJava(SodiumJava()))
    private val salt = ByteArray(16) { 7 }
    private val mib = 1024L * 1024
    private lateinit var server: MockWebServer
    private val seen = ArrayList<String>()
    private var prelogin = ""

    @Before
    fun setUp() {
        server = MockWebServer()
        server.dispatcher = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest): MockResponse {
                synchronized(seen) { seen += "${request.path} ${request.body.readUtf8()}" }
                return MockResponse().setResponseCode(200).setHeader("Content-Type", "application/json").setBody(prelogin)
            }
        }
        server.start()
    }

    @After
    fun tearDown() = server.shutdown()

    private fun signInKeys(pins: SignInPins = MemorySignInPins()) =
        SignInKeys(ApiClient.create(server.url("/api/").toString(), OkHttpClient()), crypto, pins)

    private fun answer(ops: Long?, mem: Long?, authVersion: Int = 2) {
        val settings = if (ops != null && mem != null) ""","kdfOps":$ops,"kdfMem":$mem""" else ""
        prelogin = """{"kdfSalt":"${Base64Codec.encode(salt)}","authVersion":$authVersion,"upgrade":false$settings}"""
    }

    @Test
    fun aPinHoldsForAnySpellingOfTheUsernameAndFollowsARename() = runBlocking {
        val pins = MemorySignInPins()
        val keys = SignInKeys(ApiClient.create(server.url("/api/").toString(), OkHttpClient()), crypto, pins)
        keys.pin(" Alice ")
        keys.movePin("ALICE", "Carol")
        prelogin = """{"kdfSalt":"${Base64Codec.encode(ByteArray(16) { 1 })}","authVersion":1,"upgrade":false}"""
        for (handle in listOf("alice", "ALICE", "carol", "Carol")) {
            val refused = org.junit.Assert.assertThrows(UserFacingException::class.java) { runBlocking { keys.credentialFor(handle, "password") } }
            assertEquals(E2eCrypto.SIGN_IN_DETAILS_WRONG, refused.message)
        }
        assertEquals("password", keys.credentialFor("someone-else", "password").password)
    }

    @Test
    fun theDefaultStaysAtTheInteractiveValues() {
        assertEquals(KdfSettings(2, 67_108_864), E2eCrypto.KDF_INTERACTIVE)
        assertEquals(E2eCrypto.KDF_INTERACTIVE, E2eCrypto.KDF_CURRENT)
        assertEquals(KdfSettings(10, 1_073_741_824), E2eCrypto.KDF_MAX)
    }

    @Test
    fun theFloorAndCeilingAreInclusive() {
        E2eCrypto.requireSafeKdf(KdfSettings(2, 64 * mib))
        E2eCrypto.requireSafeKdf(KdfSettings(10, 1024 * mib))
        E2eCrypto.requireSafeKdf(KdfSettings(3, 128 * mib))
        for (bad in listOf(
            KdfSettings(1, 64 * mib), KdfSettings(0, 64 * mib), KdfSettings(-1, 64 * mib),
            KdfSettings(2, 64 * mib - 1), KdfSettings(2, 8 * mib), KdfSettings(2, 0),
            KdfSettings(11, 64 * mib), KdfSettings(2, 1024 * mib + 1), KdfSettings(2, Long.MAX_VALUE), KdfSettings(Long.MAX_VALUE, 64 * mib),
        )) {
            val refused = assertThrows(bad.toString(), UnsafeKdfException::class.java) { E2eCrypto.requireSafeKdf(bad) }
            assertEquals("This account's sign-in details look wrong. Your password wasn't sent.", refused.message)
        }
    }

    @Test
    fun aWeakSettingFromTheServerIsRefusedBeforeAnythingIsDerivedOrSent() {
        for ((ops, mem) in listOf(1L to 64 * mib, 2L to 8 * mib, 11L to 64 * mib, 2L to 2048 * mib)) {
            answer(ops, mem)
            synchronized(seen) { seen.clear() }
            assertThrows(UnsafeKdfException::class.java) { runBlocking { signInKeys().credentialFor("alice", "the password") } }
            val requests = synchronized(seen) { seen.toList() }
            assertEquals("only the prelogin went out", 1, requests.size)
            assertTrue(requests.single(), "prelogin" in requests.single())
            assertTrue(requests.none { "the password" in it })
        }
    }

    @Test
    fun theSameRefusalAppliesWhenTheAccountWouldSendThePassword() {
        answer(1, 64 * mib, authVersion = 1)
        val refused = assertThrows(UnsafeKdfException::class.java) { runBlocking { signInKeys().credentialFor("alice", "the password") } }
        assertEquals(E2eCrypto.SIGN_IN_DETAILS_WRONG, refused.message)
    }

    @Test
    fun aDirectDerivationIsRefusedToo() {
        assertThrows(UnsafeKdfException::class.java) { crypto.deriveWrapKey("pw", salt, KdfSettings(1, 64 * mib)) }
        assertThrows(UnsafeKdfException::class.java) { crypto.deriveSignInKey("pw", salt, KdfSettings(2, 1024)) }
    }

    @Test
    fun controlAStrongerSettingFromTheServerIsUsed() = runBlocking {
        answer(3, 128 * mib)
        val stronger = signInKeys().credentialFor("alice", "the password")
        assertEquals(crypto.deriveSignInKey("the password", salt, KdfSettings(3, 128 * mib)), stronger.password)
        assertEquals(2, stronger.authVersion)
        assertNotEquals(crypto.deriveSignInKey("the password", salt), stronger.password)
    }

    @Test
    fun controlAnOlderServerNamingNoSettingsMeansInteractive() = runBlocking {
        answer(null, null)
        assertEquals(crypto.deriveSignInKey("the password", salt, E2eCrypto.KDF_INTERACTIVE), signInKeys().credentialFor("alice", "the password").password)
    }

    @Test
    fun onlyOneOfTheTwoSettingsCountsAsNamingNone() {
        assertEquals(E2eCrypto.KDF_INTERACTIVE, kdfSettings(3, null))
        assertEquals(E2eCrypto.KDF_INTERACTIVE, kdfSettings(null, 128 * mib))
        assertEquals(KdfSettings(3, 128 * mib), kdfSettings(3, 128 * mib))
    }

    @Test
    fun aNewCredentialUsesTheCurrentSettingsWhateverTheServerSays() = runBlocking {
        answer(5, 256 * mib)
        assertEquals(crypto.deriveSignInKey("new password", salt, E2eCrypto.KDF_CURRENT), signInKeys().newCredential("alice", "new password", salt).password)
    }

    @Test
    fun aNewWrapRecordsItsSettingsAndOpensWithThem() {
        val pair = crypto.generateKeyPair()
        val wrapped = crypto.wrapPrivateKey(pair.privateKey, "pw")
        assertEquals(E2eCrypto.KDF_CURRENT, wrapped.kdf)
        assertTrue(pair.privateKey.contentEquals(crypto.unwrapPrivateKey(wrapped, "pw")))
    }

    @Test
    fun aWrappedKeyWithNoSettingsOpensAtTheInteractiveValues() {
        val pair = crypto.generateKeyPair()
        val wrapped = crypto.wrapPrivateKey(pair.privateKey, "pw")
        val older = WrappedPrivateKey(wrapped.encryptedPrivateKey, wrapped.nonce, wrapped.kdfSalt, kdf = null)
        assertTrue(pair.privateKey.contentEquals(crypto.unwrapPrivateKey(older, "pw")))
    }

    private fun loginBody(ops: Long?, mem: Long?, wrapped: WrappedPrivateKey) = ApiClient.json.decodeFromString(
        LoginResponse.serializer(),
        """{"userId":"u","role":"user","subscriptionTier":"free","publicKey":"AA==","encryptedPrivateKey":"${Base64Codec.encode(wrapped.encryptedPrivateKey)}",
           "privateKeyNonce":"${Base64Codec.encode(wrapped.nonce)}","privateKeyKdfSalt":"${Base64Codec.encode(wrapped.kdfSalt)}"${if (ops != null) ""","privateKeyKdfOps":$ops,"privateKeyKdfMem":$mem""" else ""}}""",
    ).toLoginResult()

    @Test
    fun theLoginResultsSettingsAreReadAndCheckedBeforeUnwrapping() {
        val pair = crypto.generateKeyPair()
        val wrapped = crypto.wrapPrivateKey(pair.privateKey, "pw")

        assertEquals(KdfSettings(2, 64 * mib), wrappedKeyOf(loginBody(2, 64 * mib, wrapped)).kdf)
        assertTrue(pair.privateKey.contentEquals(crypto.unwrapPrivateKey(wrappedKeyOf(loginBody(2, 64 * mib, wrapped)), "pw")))
        assertTrue(pair.privateKey.contentEquals(crypto.unwrapPrivateKey(wrappedKeyOf(loginBody(null, null, wrapped)), "pw")))

        // A weak setting isn't a wrong password: nothing is tried with it.
        assertThrows(UnsafeKdfException::class.java) { crypto.unwrapPrivateKey(wrappedKeyOf(loginBody(1, 64 * mib, wrapped)), "pw") }
        assertThrows(UnsafeKdfException::class.java) { crypto.unwrapPrivateKey(wrappedKeyOf(loginBody(2, 8 * mib, wrapped)), "pw") }
        assertThrows(UnsafeKdfException::class.java) { crypto.unwrapPrivateKey(wrappedKeyOf(loginBody(11, 64 * mib, wrapped)), "pw") }
    }

    @Test
    fun theSettingsAreOnTheWireInEveryRequestThatSetsAPassword() {
        fun mentionsThem(json: String) = "\"kdfOps\":2" in json && "\"kdfMem\":67108864" in json
        val ops = E2eCrypto.KDF_CURRENT.ops
        val mem = E2eCrypto.KDF_CURRENT.mem
        assertTrue(mentionsThem(ApiClient.json.encodeToString(RegisterRequest.serializer(), RegisterRequest("u", "p", 2, ops, mem, "k", "e", "n", "s", null, true, true, true))))
        assertTrue(mentionsThem(ApiClient.json.encodeToString(ChangePasswordRequest.serializer(), ChangePasswordRequest("c", "n", 2, ops, mem, "e", "n", "s"))))
        assertTrue(mentionsThem(ApiClient.json.encodeToString(RecoveryCompleteRequest.serializer(), RecoveryCompleteRequest("u", "a", null, "n", 2, ops, mem, "e", "n", "s"))))
    }

    @Test
    fun prologinAnswersParseWithAndWithoutSettings() {
        val with = ApiClient.json.decodeFromString(PreLoginResult.serializer(), """{"kdfSalt":"AA==","kdfOps":2,"kdfMem":67108864}""")
        assertEquals(2L, with.kdfOps)
        assertEquals(67108864L, with.kdfMem)
        val without = ApiClient.json.decodeFromString(PreLoginResult.serializer(), """{"kdfSalt":"AA=="}""")
        assertEquals(null, without.kdfOps)
    }
}
