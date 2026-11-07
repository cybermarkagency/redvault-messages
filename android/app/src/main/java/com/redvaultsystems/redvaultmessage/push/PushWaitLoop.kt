package com.redvaultsystems.redvaultmessage.push

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.PushWaitDto
import kotlinx.coroutines.CancellationException

/** Exponential back-off after failed polls: [baseMs], doubling, at most [capMs], with up to [jitter] of it random. */
class Backoff(
    private val baseMs: Long = 2_000,
    private val capMs: Long = 5 * 60_000,
    private val jitter: Double = 0.2,
    private val random: () -> Double = Math::random,
) {
    var failures = 0
        private set

    /** How long to wait after one more failure in a row. */
    fun next(): Long {
        val exponent = failures.coerceAtMost(30)
        failures++
        val plain = (baseMs shl exponent.coerceAtMost(20)).coerceAtMost(capMs)
        // Some randomness, so phones that lost the server together don't all come back in the same second.
        val spread = (plain * jitter * random()).toLong()
        return (plain - spread).coerceAtLeast(0)
    }

    fun reset() {
        failures = 0
    }
}

/** One answer of `GET /api/push/wait`, applied to where the loop was. */
data class WaitStep(val cursor: Long, val kinds: List<String>)

/**
 * `kinds: null` means "start from this cursor" (the first call, or the server restarted and its counter
 * with it): nothing to show, only a new place to wait from. Otherwise the kinds that happened, to show.
 */
fun applyWait(response: PushWaitDto): WaitStep = WaitStep(response.cursor, response.kinds.orEmpty())

/** Why the loop ended. */
enum class WaitEnd { Stopped, SignedOut }

/**
 * The background connection's loop: long-poll, show what arrived, back off on errors. Everything it needs
 * comes in, so it runs the same in [PushFallbackService] and in unit tests.
 *
 * Ends when [keepGoing] turns false, or with [WaitEnd.SignedOut] when the server no longer takes the
 * session (401/403): polling on would only fail forever.
 */
suspend fun runWaitLoop(
    fetch: suspend (after: Long?) -> PushWaitDto,
    show: (List<String>) -> Unit,
    sleep: suspend (Long) -> Unit,
    keepGoing: () -> Boolean,
    backoff: Backoff = Backoff(),
): WaitEnd {
    var cursor: Long? = null
    while (keepGoing()) {
        val response = try {
            fetch(cursor)
        } catch (e: CancellationException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401 || e.status == 403) return WaitEnd.SignedOut
            sleep(backoff.next())
            continue
        } catch (_: Exception) {
            sleep(backoff.next())
            continue
        }
        backoff.reset()
        val step = applyWait(response)
        val shown = PushKinds.toShow(step.kinds)
        // The first answer only sets where to start; a later null means the server restarted.
        if (cursor != null && shown.isNotEmpty()) show(shown)
        cursor = step.cursor
    }
    return WaitEnd.Stopped
}
