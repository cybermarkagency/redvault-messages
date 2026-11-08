// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.goterl.lazysodium.LazySodiumAndroid
import com.goterl.lazysodium.SodiumAndroid
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AppLockStoreTest {
    private val context = InstrumentationRegistry.getInstrumentation().targetContext
    private val store = AppLockStore(context, E2eCrypto(LazySodiumAndroid(SodiumAndroid())))

    @After
    fun tearDown() = store.clear()

    @Test
    fun verifiesTheRightPinAndCountsWrongOnes() {
        store.setPin("482916")
        assertTrue(store.enabled)
        assertEquals(6, store.pinLength)
        assertFalse(store.verify("000000"))
        assertFalse(store.verify("482917"))
        assertEquals(2, store.failedAttempts)
        assertTrue(store.verify("482916"))
        assertEquals(0, store.failedAttempts)
    }

    @Test
    fun neverStoresThePinItself() {
        store.setPin("13579")
        val stored = context.getSharedPreferences("appLock", android.content.Context.MODE_PRIVATE).all.values.map { it.toString() }
        assertFalse(stored.any { it.contains("13579") })
    }

    @Test
    fun pinsMustBeFourToTwelveDigits() {
        assertFalse(AppLockStore.isValidPin("123"))
        assertTrue(AppLockStore.isValidPin("1234"))
        assertTrue(AppLockStore.isValidPin("123456789012"))
        assertFalse(AppLockStore.isValidPin("1234567890123"))
        assertFalse(AppLockStore.isValidPin("12a4"))
    }

    @Test
    fun clearingRemovesTheLock() {
        store.setPin("2468")
        store.clear()
        assertFalse(store.enabled)
        assertFalse(store.verify("2468"))
    }
}
