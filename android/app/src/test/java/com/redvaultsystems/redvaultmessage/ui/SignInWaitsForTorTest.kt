// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.TOR_UNAVAILABLE_MESSAGE
import com.redvaultsystems.redvaultmessage.tor.TorUnavailableException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.yield
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.CopyOnWriteArrayList

/**
 * Sign-in's wait for Tor (ui/AuthViewModel.kt, retryWhenTorConnects): Tor not connected is never an error on the
 * sign-in screen; the attempt waits and goes again once Tor connects. Anything else ends it as before.
 */
class SignInWaitsForTorTest {
    private val torDown = ApiException(status = 0, code = "tor_unavailable", message = TOR_UNAVAILABLE_MESSAGE)
    private val waiting = CopyOnWriteArrayList<Boolean>()

    @Test fun torUnavailableIsRecognised() {
        assertTrue(isTorUnavailable(torDown))
        assertTrue(isTorUnavailable(TorUnavailableException()))
        assertFalse(isTorUnavailable(ApiException(0, "network_error", "Can't reach the server.")))
        assertFalse(isTorUnavailable(ApiException(401, "invalid_credentials", "Wrong")))
    }

    @Test fun itWaitsForTorAndSignsInOnceItConnects() = runBlocking {
        var attempts = 0
        var waits = 0
        val result = retryWhenTorConnects(waitForTor = { waits++ }, waiting = { waiting.add(it) }, pauseMillis = 0) {
            attempts++
            if (attempts < 3) throw torDown
            "signed in"
        }
        assertEquals("signed in", result)
        assertEquals(3, attempts)
        assertEquals(2, waits)
        assertEquals(listOf(true, true, false), waiting.toList())
    }

    @Test fun anyOtherFailureEndsItAsBefore() = runBlocking {
        var attempts = 0
        val error = runCatching {
            retryWhenTorConnects(waitForTor = {}, waiting = { waiting.add(it) }, pauseMillis = 0) {
                attempts++
                if (attempts == 1) throw torDown
                throw ApiException(401, "invalid_credentials", "Wrong handle or password.")
            }
        }.exceptionOrNull()
        assertTrue(error is ApiException && error.code == "invalid_credentials")
        assertEquals(2, attempts)
        assertEquals(listOf(true, false), waiting.toList())
    }

    @Test fun nothingIsTriedAgainUntilTorConnects() = runBlocking {
        val connected = CompletableDeferred<Unit>()
        var attempts = 0
        val signIn = async(Dispatchers.Default) {
            retryWhenTorConnects(waitForTor = { connected.await() }, waiting = { waiting.add(it) }, pauseMillis = 0) {
                attempts++
                if (attempts == 1) throw TorUnavailableException()
                "signed in"
            }
        }
        withTimeout(5_000) { while (waiting.isEmpty()) yield() }
        assertEquals(1, attempts)
        assertEquals(listOf(true), waiting.toList())
        connected.complete(Unit)
        assertEquals("signed in", withTimeout(5_000) { signIn.await() })
        assertEquals(2, attempts)
    }

    @Test fun cancelStopsTheWait() = runBlocking {
        var attempts = 0
        val signIn = async(Dispatchers.Default) {
            retryWhenTorConnects(waitForTor = { CompletableDeferred<Unit>().await() }, waiting = { waiting.add(it) }, pauseMillis = 0) {
                attempts++
                throw torDown
            }
        }
        withTimeout(5_000) { while (waiting.isEmpty()) yield() }
        signIn.cancel()
        val error = runCatching { signIn.await() }.exceptionOrNull()
        assertTrue(error is CancellationException)
        assertEquals(1, attempts)
    }
}
