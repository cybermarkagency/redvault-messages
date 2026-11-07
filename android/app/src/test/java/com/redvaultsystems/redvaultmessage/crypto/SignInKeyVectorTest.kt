package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test

class SignInKeyVectorTest {
    private val crypto = E2eCrypto(LazySodiumJava(SodiumJava()))
    private val salt = ByteArray(16) { 7 }

    @Test
    fun matchesTheVectorEveryClientTestsAgainst() {
        assertEquals("FctalMlnA5L7J4K3YTVRGWqHwQRvCdnMWno0LvXQI2I=", crypto.deriveSignInKey("correct horse battery staple", salt))
    }

    @Test
    fun differsFromTheUnlockKey() {
        assertNotEquals(Base64Codec.encode(crypto.deriveWrapKey("a password", salt)), crypto.deriveSignInKey("a password", salt))
    }
}
