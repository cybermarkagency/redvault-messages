package com.redvaultsystems.redvaultmessage.crypto

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.goterl.lazysodium.LazySodiumAndroid
import com.goterl.lazysodium.SodiumAndroid
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/** Pinned to the same vector as ui/src/test/recoveryKey.test.ts, so a key made on one app works on the other. */
@RunWith(AndroidJUnit4::class)
class RecoveryKeyTest {
    private val sodium = LazySodiumAndroid(SodiumAndroid())
    private val crypto = E2eCrypto(sodium)
    private val recovery = RecoveryKey(sodium, crypto)
    private val fixedKey = ByteArray(32) { i -> ((i * 13 + 5) and 0xff).toByte() }

    @Test
    fun derivesTheWebClientsVector() {
        assertEquals("AUJB-6LBZ-IZJW-A3L2-Q6KK-DLV3-ZDK6-F374-BELC-GMB5-JJLW-I4L6-ROMA", RecoveryKey.format(fixedKey))
        assertEquals("hNkdtJ9WVWt9NeRT9e0PbW25zE/F7VmLxjWUqY12lwM=", Base64Codec.encode(recovery.wrapKey(fixedKey)))
        assertEquals("7N3QawoTizJf9BpyLEIA7jfSfleT9oISJi8CM8b1pSI=", Base64Codec.encode(recovery.authToken(fixedKey)))
    }

    @Test
    fun formatsAndParsesForgivingly() {
        val key = recovery.generate()
        val text = RecoveryKey.format(key)
        assertTrue(Regex("^([A-Z2-7]{4}-){12}[A-Z2-7]{4}$").matches(text))
        assertArrayEquals(key, RecoveryKey.parse(text))
        assertArrayEquals(key, RecoveryKey.parse(text.lowercase().replace("-", " ")))
        assertArrayEquals(fixedKey, RecoveryKey.parse("AUJB-6LBZ-IZJW-A3L2-Q6KK-DLV3-ZDK6-F374-BELC-GMB5-JJLW-I4L6-R0MA"))
        assertNull(RecoveryKey.parse("too-short"))
    }

    @Test
    fun unwrapsOnlyForTheRightKeyAndAccount() {
        val pair = crypto.generateKeyPair()
        val key = recovery.generate()
        val m = recovery.materialFor(key, pair.privateKey)
        val publicB64 = Base64Codec.encode(pair.publicKey)
        assertArrayEquals(pair.privateKey, recovery.unwrap(key, m.recoveryEncryptedPrivateKey, m.recoveryPrivateKeyNonce, publicB64))

        val other = Base64Codec.encode(crypto.generateKeyPair().publicKey)
        assertThrows(CryptoException::class.java) { recovery.unwrap(key, m.recoveryEncryptedPrivateKey, m.recoveryPrivateKeyNonce, other) }
        assertThrows(CryptoException::class.java) { recovery.unwrap(recovery.generate(), m.recoveryEncryptedPrivateKey, m.recoveryPrivateKeyNonce, publicB64) }
    }
}
