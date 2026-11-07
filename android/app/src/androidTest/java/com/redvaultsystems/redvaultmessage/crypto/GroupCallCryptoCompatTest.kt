package com.redvaultsystems.redvaultmessage.crypto

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.goterl.lazysodium.LazySodiumAndroid
import com.goterl.lazysodium.SodiumAndroid
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Pins the group-call room key to the web client byte-for-byte. The vectors below were produced by the web's
 * own groupCallCrypto.ts and sas.ts (libsodium-wrappers-sumo): a room key the web sealed from "alice" to
 * "bob", and the safety words it shows for three people. If these fail, a call between this app and the web
 * can't share a room key. Runs on a device or emulator (libsodium is a native library).
 */
@RunWith(AndroidJUnit4::class)
class GroupCallCryptoCompatTest {
    private val sodium = LazySodiumAndroid(SodiumAndroid())
    private val e2e = E2eCrypto(sodium)
    private val crypto = GroupCallCrypto(sodium, e2e)
    private fun b64(s: String) = Base64Codec.decode(s)

    private val alicePublic = b64("0//6OwgAtFW9G+466J7L8gTNuhQ3jlWiYCERJRIFtlg=")
    private val alicePrivate = b64("gjzSWnqP6U7KgKs9kDj5ETD03aeYK9+VNpm1fOafYys=")
    private val bobPublic = b64("DBqRY27UkdYVLK3P06GHSxbIipS6e361q6hS+a8HS0U=")
    private val bobPrivate = b64("CeM6aU2Q4a89ewrKh6oVvPizmq5nxglX6QSXfT90GKM=")
    private val carolPublic = b64("hG529CcpQyJfTUaop7HdhH2Kr4ptHx8wzq6jCIys9DI=")
    private val roomKey = b64("AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dw=")
    private val sentAt = 1792150906279L
    private val webSealed = SealedSignal(
        b64(
            "PQPSsUeEbadCC3NW2p+1KVjhraInP+xAaiI9XUTNV0kEJDLsQ5Fa8wkGP0mYe9WIXrNDSoSGqX8ZYt8SxnrMLsHIYAeujIZ0tj+xB66gaFAHepxmbS/WiE/UpRitkBdHbp44fSJeGpzZcqh3UfDdYqeitxHPd6OZAqATR1xJGSWeNmDx59sTekIBOd/gyeKXqofZDs4ladSDZ5aIiWEwZw6sqgGH6UGPg0K2V6XuedFWzFqEkxIhHMUYsUmmTQdUYjwa3wwXbbiW04hQA5fjhg2HmyQACu6SL+Pdy63JQj7bywTrfLcf0vEcIsBwWoqBpDGNSWIIaZVPf3zo4sXEMG/LSITvXzv73GZ2ktlvUH4XcVlXd2DadtbkIAzsUGNirAy6ImeJHgiADciJxIE2EzhT9UkmMTt0xza9UTTXJKTjs6oudHa2cw==",
        ),
        b64("pHDfzlDh4dtRTiYbZzz2XtMFLInwWhjt"),
    )
    private val expected = ExpectedRoomKey(
        callId = "6f1d2c3e-4b5a-4c6d-8e7f-0a1b2c3d4e5f",
        conversationId = "9a8b7c6d-5e4f-4a3b-8c2d-1e0f9a8b7c6d",
        epoch = 19,
        from = "11111111-1111-4111-8111-111111111111",
        to = "22222222-2222-4222-8222-222222222222",
    )

    @Test
    fun opensTheRoomKeyTheWebSealed() {
        assertArrayEquals(roomKey, crypto.openRoomKey(webSealed, alicePublic, bobPrivate, expected, nowMs = sentAt))
    }

    @Test
    fun refusesTheWebsKeyFromAnyoneButTheKeyholdersIdentityKey() {
        val error = assertThrows(GroupKeyException::class.java) {
            crypto.openRoomKey(webSealed, carolPublic, bobPrivate, expected, nowMs = sentAt)
        }
        assertTrue(error.message.orEmpty().contains("couldn't be verified"))
        // Nor can someone else's private key open it.
        assertThrows(GroupKeyException::class.java) { crypto.openRoomKey(webSealed, alicePublic, alicePrivate, expected, nowMs = sentAt) }
    }

    @Test
    fun refusesTheWebsKeyForAnotherEpochOrAStaleOne() {
        assertThrows(GroupKeyException::class.java) {
            crypto.openRoomKey(webSealed, alicePublic, bobPrivate, ExpectedRoomKey(expected.callId, expected.conversationId, 20, expected.from, expected.to), nowMs = sentAt)
        }
        assertThrows(GroupKeyException::class.java) {
            crypto.openRoomKey(webSealed, alicePublic, bobPrivate, expected, nowMs = sentAt + 10 * 60 * 1000)
        }
    }

    @Test
    fun aKeySealedHereOpensHereAndTheSealedMessageHasTheWebsLayout() {
        val key = crypto.newRoomKey()
        assertEquals(32, key.size)
        val sealed = crypto.sealRoomKey(expected, key, bobPublic, alicePrivate, nowMs = sentAt)
        assertEquals(24, sealed.nonce.size)
        assertArrayEquals(key, crypto.openRoomKey(sealed, alicePublic, bobPrivate, expected, nowMs = sentAt))
        // Opened with the raw crypto_box, it's the web's JSON, field for field.
        val plain = ByteArray(sealed.ciphertext.size - 16)
        assertTrue(sodium.cryptoBoxOpenEasy(plain, sealed.ciphertext, sealed.ciphertext.size.toLong(), sealed.nonce, alicePublic, bobPrivate))
        assertEquals(
            "{\"v\":1,\"type\":\"group-call-key\",\"callId\":\"${expected.callId}\",\"conversationId\":\"${expected.conversationId}\"," +
                "\"epoch\":19,\"from\":\"${expected.from}\",\"to\":\"${expected.to}\",\"key\":\"${Base64Codec.encode(key)}\",\"sentAt\":$sentAt}",
            plain.toString(Charsets.UTF_8),
        )
    }

    @Test
    fun everyoneGetsTheSameSafetyWordsAsTheWeb() {
        val callId = expected.callId
        val people = listOf(
            "11111111-1111-4111-8111-111111111111" to alicePublic,
            "22222222-2222-4222-8222-222222222222" to bobPublic,
            "33333333-3333-4333-8333-333333333333" to carolPublic,
        )
        val words = crypto.groupSasWords(callId, people)
        assertEquals(listOf("bridge", "indigo", "crater", "sunset"), words)
        // Order doesn't matter, and a swapped identity key changes them.
        assertEquals(words, crypto.groupSasWords(callId, people.reversed()))
        assertNotEquals(words, crypto.groupSasWords(callId, listOf(people[0], people[1].first to carolPublic, people[2])))
        assertNotEquals(words, crypto.groupSasWords("another-call", people))
    }
}
