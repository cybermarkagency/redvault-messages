package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.tor.TorUnavailableException
import com.redvaultsystems.redvaultmessage.data.api.TOR_UNAVAILABLE_MESSAGE
import com.redvaultsystems.redvaultmessage.data.api.TransferTargetDto
import kotlinx.coroutines.delay
import okhttp3.Headers
import okhttp3.OkHttpClient
import okhttp3.Response
import java.io.IOException
import java.time.Instant

/**
 * Uploads and downloads ciphertext through our own API ("proxy" transfer
 * targets). A "direct" target (a presigned URL at the storage provider) is
 * refused outright: it would let the provider see this phone's IP address
 * and when it's in use. [allowedPrefix] pins which API paths a target may
 * point at ("/backup/" for Files).
 */
class ProxyTransfer(private val client: OkHttpClient, private val apiBase: String, private val allowedPrefix: String) {

    /** Runs one transfer with retries on network errors, 5xx and 429; returns the successful response, still open. */
    suspend fun run(target: TransferTargetDto, call: suspend (OkHttpClient, String, Headers) -> Response): Response {
        if (target.mode != "proxy" || !target.url.startsWith(allowedPrefix)) throw UserFacingException("Transfer failed. Try again.")
        val url = apiBase + target.url.removePrefix("/")
        val headers = Headers.Builder().apply { target.headers.forEach { (name, value) -> add(name, value) } }.build()
        var lastError: Exception = TransferInterruptedException()
        for (attempt in 0 until MAX_ATTEMPTS) {
            if (attempt > 0) delay(minOf(8_000L, 500L shl (attempt - 1)))
            if (runCatching { Instant.parse(target.expiresAt).toEpochMilli() < System.currentTimeMillis() }.getOrDefault(false)) {
                throw UserFacingException("The transfer link expired. Try again.")
            }
            val response = try {
                call(client, url, headers)
            } catch (e: TorUnavailableException) {
                // Tor routing and Tor is down: retrying now can't help, and nothing may go another way.
                throw TransferInterruptedException(TOR_UNAVAILABLE_MESSAGE)
            } catch (e: IOException) {
                lastError = TransferInterruptedException()
                continue
            }
            if (response.isSuccessful) return response
            val code = response.code
            response.close()
            val error = when (code) {
                410 -> UserFacingException("This file is no longer available.")
                404 -> UserFacingException("That file isn't there anymore.")
                else -> UserFacingException("Transfer failed ($code). Try again.")
            }
            if (code >= 500 || code == 429) {
                lastError = TransferInterruptedException(error.message ?: "Network interrupted. Try again.")
                continue
            }
            throw error
        }
        throw lastError
    }

    companion object {
        private const val MAX_ATTEMPTS = 4
    }
}
