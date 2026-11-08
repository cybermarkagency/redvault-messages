// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.goterl.lazysodium.LazySodiumAndroid
import com.goterl.lazysodium.SodiumAndroid
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Pins the Android crypto to the web client byte-for-byte. Every expected
 * value below was produced by the web client's own libsodium build
 * (libsodium-wrappers-sumo, the same calls as ui/src/lib/e2eCrypto.ts and
 * commitment.ts). If any of these fail, Android and web users can't read
 * each other's messages or log in to each other's accounts.
 *
 * Runs on a device or emulator (libsodium is a native library).
 */
@RunWith(AndroidJUnit4::class)
class E2eCryptoCompatTest {
    private val crypto = E2eCrypto(LazySodiumAndroid(SodiumAndroid()))
    private fun b64(s: String) = Base64Codec.decode(s)

    private val password = "correct horse battery"
    private val salt = b64("AwoRGB8mLTQ7QklQV15lbA==")
    private val wrapKey = b64("QcWwX65OHrfRNkusFLIAnSPV7csyDZixx/MIyO1XIgo=")
    private val publicKey = b64("sduaLGHdSyI5zX1H/c4hGwIcCzYEnx4pI6JTs1qCTw0=")
    private val privateKey = b64("aXcRUXy911/1q83RboxQwY+QdYZyyE6fJW4Kc6oPNAo=")
    private val wrapNonce = b64("BQwTGiEoLzY9REtSWWBnbnV8g4qRmJ+m")
    private val encryptedPrivateKey = b64("EIHl9Qlybjc5eUytEwggq2nbod3oMc/2JiwRII5xw5ZthVA/Vyy1zibp2FfL2sNz")
    private val conversationKey = b64("Y2pxeH+GjZSboqmwt77FzNPa4ejv9v0ECxIZICcuNTw=")
    private val sealedByWeb =
        b64("hut7Ysgml9JRk5VCtoXsXNeV+wtiaCLJebDC3LPziUN5jkmDeyVEVfJFZMQqcmU3YwP+O0J+WAvQgG0HFXFhYEkMuxPuogpGPZUSl5NuYn4=")
    private val messagePlaintext = "hello · wörld 🔐"
    private val messageNonce = b64("yM/W3eTr8vkABw4VHCMqMTg/Rk1UW2Jp")
    private val messageCiphertext = b64("R2dDsSBiOfCHRBSeUmIkkMVenC6xUIY6i48LAawBivGbSLTG")

    @Test
    fun argon2idWrapKeyMatchesWeb() {
        assertArrayEquals(wrapKey, crypto.deriveWrapKey(password, salt))
    }

    @Test
    fun unwrapsPrivateKeyWrappedByWeb() {
        val unwrapped = crypto.unwrapPrivateKey(WrappedPrivateKey(encryptedPrivateKey, wrapNonce, salt), password)
        assertArrayEquals(privateKey, unwrapped)
    }

    @Test
    fun wrongPasswordFailsToUnwrap() {
        assertThrows(CryptoException::class.java) {
            crypto.unwrapPrivateKey(WrappedPrivateKey(encryptedPrivateKey, wrapNonce, salt), "wrong password")
        }
    }

    @Test
    fun aeadEncryptMatchesWebForSameNonce() {
        val result = crypto.aeadEncrypt(messagePlaintext.toByteArray(Charsets.UTF_8), conversationKey, messageNonce)
        assertArrayEquals(messageCiphertext, result.ciphertext)
    }

    @Test
    fun decryptsMessageEncryptedByWeb() {
        assertEquals(messagePlaintext, crypto.decryptWithConversationKey(messageCiphertext, messageNonce, conversationKey))
    }

    @Test
    fun unsealsConversationKeySealedByWeb() {
        assertArrayEquals(conversationKey, crypto.unsealConversationKey(sealedByWeb, publicKey, privateKey))
    }

    @Test
    fun sealRoundTrips() {
        val keyPair = crypto.generateKeyPair()
        val key = crypto.generateConversationKey()
        val sealed = crypto.sealConversationKey(key, keyPair.publicKey)
        assertArrayEquals(key, crypto.unsealConversationKey(sealed, keyPair.publicKey, keyPair.privateKey))
    }

    @Test
    fun wrapRoundTrips() {
        val keyPair = crypto.generateKeyPair()
        val wrapped = crypto.wrapPrivateKey(keyPair.privateKey, "a new password")
        assertArrayEquals(keyPair.privateKey, crypto.unwrapPrivateKey(wrapped, "a new password"))
    }

    @Test
    fun safetyNumberMatchesWebInEitherOrder() {
        val other = b64("VAv7yr7CW/DLzzzkKkInMLSSxPlmrFnjlFIYi4CJSxM=")
        val expected = "793202196896733342307744792420430796626930648824555351264617"
        assertEquals(expected, crypto.safetyNumberDigits(publicKey, other).joinToString(""))
        assertEquals(expected, crypto.safetyNumberDigits(other, publicKey).joinToString(""))
    }

    @Test
    fun commitmentMatchesWeb() {
        assertEquals(
            "13fc08a55f02eef3fa748752614b96051b583027b4bc88495682f09dc51f63e3",
            Commitment.computeHex("123e4567-e89b-12d3-a456-426614174000", messageCiphertext, messageNonce),
        )
    }
}
