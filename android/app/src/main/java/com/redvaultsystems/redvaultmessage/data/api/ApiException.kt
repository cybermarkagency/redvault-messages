// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.api

import com.redvaultsystems.redvaultmessage.tor.TorUnavailableException
import kotlinx.serialization.json.Json
import retrofit2.HttpException
import java.io.IOException

/** A failed API call, with the server's machine-readable error code where there is one. */
class ApiException(val status: Int, val code: String, message: String) : Exception(message)

/** Runs [block], turning Retrofit's HTTP and network failures into [ApiException]. */
suspend fun <T> apiCall(json: Json, block: suspend () -> T): T {
    try {
        return block()
    } catch (e: HttpException) {
        val body = e.response()?.errorBody()?.string()
        val parsed = body?.let { runCatching { json.decodeFromString(ErrorBody.serializer(), it) }.getOrNull() }
        throw ApiException(
            status = e.code(),
            code = parsed?.error ?: "unknown_error",
            message = parsed?.message ?: e.message(),
        )
    } catch (e: TorUnavailableException) {
        // Nothing left the phone: Tor routing and Tor is down (tor/TorRouting.kt).
        throw ApiException(status = 0, code = "tor_unavailable", message = TOR_UNAVAILABLE_MESSAGE)
    } catch (e: IOException) {
        throw ApiException(status = 0, code = "network_error", message = "Can't reach the server. Check your connection.")
    }
}

/** What the apps say while Tor is down (docs/tor-routing-plan.md): the same words on every platform. */
const val TOR_UNAVAILABLE_MESSAGE =
    "Tor connection unavailable. Your private messages and media will remain queued until the secure connection is restored."
