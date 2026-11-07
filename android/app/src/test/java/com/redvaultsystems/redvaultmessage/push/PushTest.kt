package com.redvaultsystems.redvaultmessage.push

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.PushWaitDto
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** UnifiedPush payloads, the route after sign-in, and the background connection's loop (docs/unifiedpush-plan.md). */
class PushTest {
    // A real-looking VAPID key: 65-byte uncompressed P-256 point, base64url without padding (87 characters).
    private val vapid = "BEl62iUYgUivxIkv69yViEuiBIa-Ib9-SkvMeAtA3LFgDzkrxZJjSgSnfckjBJuBkr3qBUYIHBQFLXYp5Nksh8U"

    // --- Payloads ---

    @Test
    fun parsesEveryKnownKind() {
        for (kind in listOf("message", "friend_request", "friend_accepted", "call", "share_upload")) {
            assertEquals(kind, PushKinds.parsePayload("""{"kind":"$kind"}""".toByteArray()))
        }
    }

    @Test
    fun ignoresExtraFieldsButNotUnknownKinds() {
        assertEquals("message", PushKinds.parsePayload("""{"kind":"message","v":2}""".toByteArray()))
        assertNull(PushKinds.parsePayload("""{"kind":"payment"}""".toByteArray()))
    }

    @Test
    fun rejectsAnythingElse() {
        for (payload in listOf("", "message", "[]", "null", """{"kind":1}""", """{"kind":null}""", """{"type":"message"}""", "{not json")) {
            assertNull(payload, PushKinds.parsePayload(payload.toByteArray()))
        }
        assertNull(PushKinds.parsePayload(ByteArray(5000) { 'a'.code.toByte() }))
        assertNull(PushKinds.parsePayload(byteArrayOf(0xff.toByte(), 0xfe.toByte())))
    }

    @Test
    fun notificationTextSaysOnlyTheKind() {
        assertEquals("You have a new message.", PushKinds.textFor("message"))
        assertEquals("You have a new friend request.", PushKinds.textFor("friend_request"))
        assertEquals("Your friend request was accepted.", PushKinds.textFor("friend_accepted"))
        assertEquals("New files arrived through one of your sharing links.", PushKinds.textFor("share_upload"))
        // A call rings instead of showing text.
        assertNull(PushKinds.textFor("call"))
        assertNull(PushKinds.textFor("anything"))
    }

    @Test
    fun severalKindsComeDownToACallAndTheNewestOther() {
        assertEquals(listOf("call", "friend_request"), PushKinds.toShow(listOf("message", "call", "friend_request", "bogus")))
        assertEquals(listOf("message"), PushKinds.toShow(listOf("message", "message")))
        assertEquals(emptyList<String>(), PushKinds.toShow(listOf("bogus")))
        assertEquals(emptyList<String>(), PushKinds.toShow(emptyList()))
    }

    // --- The route after sign-in ---

    private fun route(
        enabled: Boolean = true,
        strict: Boolean = false,
        vapidKey: String? = vapid,
        distributor: Boolean = true,
        background: Boolean = true,
        notifications: Boolean = true,
        tor: Boolean = false,
    ) = choosePushRoute(enabled, strict, vapidKey, distributor, background, notifications, tor)

    @Test
    fun unifiedPushWhenTheServerHasAKeyAndADistributorIsInstalled() {
        assertEquals(PushRoute.UnifiedPush, route())
        // Android's notification permission doesn't stop a registration (the user may allow it later).
        assertEquals(PushRoute.UnifiedPush, route(notifications = false))
    }

    @Test
    fun noVapidKeyMeansTheBackgroundConnection() {
        assertEquals(PushRoute.BackgroundConnection, route(vapidKey = null))
        // A key the connector would refuse (it throws) counts as none.
        assertEquals(PushRoute.BackgroundConnection, route(vapidKey = "not-a-key"))
        assertEquals(PushRoute.BackgroundConnection, route(vapidKey = "$vapid="))
    }

    @Test
    fun noDistributorMeansTheBackgroundConnection() {
        assertEquals(PushRoute.BackgroundConnection, route(distributor = false))
    }

    @Test
    fun theBackgroundConnectionOnlyWhenAllowedAndItCanNotify() {
        assertEquals(PushRoute.Off, route(distributor = false, background = false))
        assertEquals(PushRoute.Off, route(vapidKey = null, notifications = false))
        // With a working push route the switch doesn't matter.
        assertEquals(PushRoute.UnifiedPush, route(background = false))
    }

    @Test
    fun strictNoPushWithTorRoutingKeepsOnlyTheBackgroundConnection() {
        // Never a push service run by someone else: UnifiedPush is out even with a key and a distributor...
        assertEquals(PushRoute.BackgroundConnection, route(strict = true, tor = true))
        assertEquals(PushRoute.BackgroundConnection, route(strict = true, tor = true, distributor = false, vapidKey = null))
        // ...and the background connection (onion service only, through the Tor gate) still obeys its switches.
        assertEquals(PushRoute.Off, route(strict = true, tor = true, background = false))
        assertEquals(PushRoute.Off, route(strict = true, tor = true, notifications = false))
        assertEquals(PushRoute.Off, route(strict = true, tor = true, enabled = false))
        assertEquals(PushRoute.BackgroundConnection, fallbackRoute(enabled = true, strictNoPush = true, backgroundConnection = true, notificationsAllowed = true, torRouted = true))
        // Tor routing alone changes nothing when strict mode is off.
        assertEquals(PushRoute.UnifiedPush, route(tor = true))
    }

    @Test
    fun strictNoPushRegistersNothingAndStartsNoConnection() {
        assertEquals(PushRoute.Off, route(strict = true))
        assertEquals(PushRoute.Off, route(strict = true, distributor = false))
        assertEquals(PushRoute.Off, fallbackRoute(enabled = true, strictNoPush = true, backgroundConnection = true, notificationsAllowed = true))
    }

    @Test
    fun notificationsOffRegistersNothingAndStartsNoConnection() {
        assertEquals(PushRoute.Off, route(enabled = false))
        assertEquals(PushRoute.Off, route(enabled = false, distributor = false))
        assertEquals(PushRoute.Off, fallbackRoute(enabled = false, strictNoPush = false, backgroundConnection = true, notificationsAllowed = true))
    }

    @Test
    fun vapidKeyFormat() {
        assertTrue(isValidVapidKey(vapid))
        assertFalse(isValidVapidKey(null))
        assertFalse(isValidVapidKey(vapid.dropLast(1)))
        assertFalse(isValidVapidKey(vapid.replace('-', '+')))
    }

    // --- The background connection ---

    @Test
    fun aNullKindsAnswerOnlySetsTheCursor() {
        assertEquals(WaitStep(7, emptyList()), applyWait(PushWaitDto(cursor = 7, kinds = null)))
        assertEquals(WaitStep(9, listOf("message", "call")), applyWait(PushWaitDto(cursor = 9, kinds = listOf("message", "call"))))
    }

    @Test
    fun loopStartsFromTheFirstCursorShowsWhatArrivesAndFollowsARestart() = runBlocking {
        val answers = ArrayDeque(
            listOf(
                PushWaitDto(10, null), // first call: where to start
                PushWaitDto(11, listOf("message")),
                PushWaitDto(11, emptyList()), // timed out, nothing new
                PushWaitDto(2, null), // the server restarted: start again from 2, show nothing
                PushWaitDto(3, listOf("friend_request", "call")),
            ),
        )
        val asked = mutableListOf<Long?>()
        val shown = mutableListOf<List<String>>()
        val end = runWaitLoop(
            fetch = { after -> asked += after; answers.removeFirst() },
            show = { shown += it },
            sleep = { error("no errors, so no back-off") },
            keepGoing = { answers.isNotEmpty() },
        )
        assertEquals(WaitEnd.Stopped, end)
        assertEquals(listOf(null, 10L, 11L, 11L, 2L), asked)
        assertEquals(listOf(listOf("message"), listOf("call", "friend_request")), shown)
    }

    @Test
    fun loopBacksOffOnErrorsAndResetsAfterASuccess() = runBlocking {
        var calls = 0
        val slept = mutableListOf<Long>()
        runWaitLoop(
            fetch = {
                calls++
                when (calls) {
                    1, 2, 3 -> throw ApiException(0, "network_error", "Can't reach the server.")
                    4 -> PushWaitDto(1, null)
                    5 -> throw ApiException(503, "unavailable", "busy")
                    else -> PushWaitDto(1, emptyList())
                }
            },
            show = {},
            sleep = { slept += it },
            keepGoing = { calls < 6 },
            backoff = Backoff(baseMs = 1_000, capMs = 300_000, jitter = 0.0),
        )
        assertEquals(listOf(1_000L, 2_000L, 4_000L, 1_000L), slept)
    }

    @Test
    fun loopEndsWhenTheSessionIsGone() = runBlocking {
        var calls = 0
        val end = runWaitLoop(
            fetch = {
                calls++
                if (calls == 1) PushWaitDto(5, null) else throw ApiException(401, "unauthorized", "Sign in again.")
            },
            show = { error("nothing to show") },
            sleep = {},
            keepGoing = { true },
        )
        assertEquals(WaitEnd.SignedOut, end)
        assertEquals(2, calls)
    }

    @Test
    fun backoffDoublesUpToAboutFiveMinutes() {
        val backoff = Backoff(baseMs = 2_000, capMs = 300_000, jitter = 0.0)
        val delays = List(12) { backoff.next() }
        assertEquals(listOf(2_000L, 4_000L, 8_000L, 16_000L, 32_000L, 64_000L, 128_000L, 256_000L, 300_000L, 300_000L, 300_000L, 300_000L), delays)
        backoff.reset()
        assertEquals(2_000L, backoff.next())
    }

    @Test
    fun backoffJitterOnlyShortensTheWait() {
        val low = Backoff(baseMs = 10_000, capMs = 300_000, jitter = 0.2) { 1.0 }
        assertEquals(8_000L, low.next())
        val none = Backoff(baseMs = 10_000, capMs = 300_000, jitter = 0.2) { 0.0 }
        assertEquals(10_000L, none.next())
        // Many failures in a row never overflow past the cap.
        val long = Backoff(jitter = 0.0)
        repeat(100) { long.next() }
        assertEquals(300_000L, long.next())
    }
}
