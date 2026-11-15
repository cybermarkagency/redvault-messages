// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

import com.redvaultsystems.e2ee.NativeSealed
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import okhttp3.Cookie
import okhttp3.CookieJar
import okhttp3.Headers
import okhttp3.Interceptor
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.MediaType.Companion.toMediaTypeOrNull
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import okio.Buffer
import java.io.IOException
import java.nio.ByteBuffer
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

/**
 * The sealed backup route (docs/tor-sealed-fallback.md), Android's side; the desktop has the same rules
 * (desktop/e2ev3/src/sealed_route.cpp).
 *
 * An application interceptor in front of the Tor gate. Requests keep their onion URL (and the session cookie), and
 * normally go straight on through the onion. When the onion keeps failing while Tor itself works (30 s of failures,
 * or 3 in a row) and the user hasn't chosen "Only use the onion address", requests are sealed to the gateway's pinned
 * key (crypto-core, Oblivious HTTP) and POSTed through Tor to the website's gateway by [website], a client that only
 * reaches that one https host through the same gate. A check tries the onion every minute and moves back when it
 * answers. Never a request without Tor, never an unsealed request to the website. Only a GET is sent again
 * automatically at the switch; anything else fails as it would have and the outbox retries it.
 *
 * Cookies: application interceptors run before OkHttp adds them, so this loads and saves them through [cookieJar].
 */
class SealedRoute(
    private val onionOrigin: String,
    private val pathPrefix: String,
    private val gatewayUrl: String,
    private val keyConfig: ByteArray,
    private val website: OkHttpClient,
    private val probeClient: () -> OkHttpClient,
    private val probeUrl: String,
    private val cookieJar: CookieJar,
    private val allowed: () -> Boolean,
    private val onRouteChanged: (Boolean) -> Unit,
    private val clock: () -> Long = System::nanoTime,
    private val probeEverySeconds: Long = 60,
) : Interceptor {
    @Volatile var onBackup: Boolean = false
        private set

    private val lock = Any()
    private var failures = 0
    private var firstFailureAt = 0L
    private val prober: ScheduledExecutorService = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "sealed-route-probe").apply { isDaemon = true }
    }
    private var probing: ScheduledFuture<*>? = null

    override fun intercept(chain: Interceptor.Chain): Response {
        val request = chain.request()
        if (!allowed() && onBackup) setBackup(false)
        if (onBackup) return sendSealed(request)
        return try {
            chain.proceed(request).also { noteSuccess() }
        } catch (e: TorUnavailableException) {
            throw e // Tor itself is down: nothing goes anywhere, on either route.
        } catch (e: IOException) {
            if (chain.call().isCanceled()) throw e
            noteFailure()
            if (onBackup && (request.method == "GET" || request.method == "HEAD")) sendSealed(request) else throw e
        }
    }

    /** Back to the onion now (the user turned "Only use the onion address" on). */
    fun useOnion() {
        synchronized(lock) { failures = 0 }
        setBackup(false)
    }

    /** One onion check, as the background one does. */
    fun probeOnionNow(): Boolean = try {
        probeClient().newCall(Request.Builder().url(probeUrl).get().build()).execute().use { response ->
            if (response.code in 200..499) {
                synchronized(lock) { failures = 0 }
                setBackup(false)
                true
            } else {
                false
            }
        }
    } catch (_: IOException) {
        false
    }

    private fun noteSuccess() = synchronized(lock) { failures = 0 }

    private fun noteFailure() {
        val switchNow = synchronized(lock) {
            val now = clock()
            if (failures == 0) firstFailureAt = now
            failures++
            failures >= SWITCH_AFTER_FAILURES || now - firstFailureAt >= TimeUnit.SECONDS.toNanos(SWITCH_AFTER_SECONDS)
        }
        if (switchNow && keyConfig.isNotEmpty() && allowed()) setBackup(true)
    }

    private fun setBackup(backup: Boolean) {
        synchronized(lock) {
            if (onBackup == backup) return
            onBackup = backup
            probing?.cancel(false)
            probing = if (backup) {
                prober.scheduleWithFixedDelay({ probeOnionNow() }, probeEverySeconds, probeEverySeconds, TimeUnit.SECONDS)
            } else {
                null
            }
        }
        onRouteChanged(backup)
    }

    private fun sendSealed(request: Request): Response {
        val url = request.url.toString()
        val prefix = onionOrigin + pathPrefix
        if (!url.startsWith("$prefix/api/")) throw IOException("Only the API can use the backup route")
        val path = url.substring(prefix.length)

        val bodyBytes = request.body?.let { body ->
            val length = body.contentLength()
            if (length > MAX_SEALED_BODY) throw IOException("Large files wait for the onion address: the backup route carries up to 8 MB at a time")
            Buffer().also { body.writeTo(it) }.also {
                if (it.size > MAX_SEALED_BODY) throw IOException("Large files wait for the onion address: the backup route carries up to 8 MB at a time")
            }.readByteArray()
        } ?: ByteArray(0)

        val headers = buildJsonArray {
            for ((name, value) in request.headers) {
                if (name.lowercase() !in SKIPPED_HEADERS) add(buildJsonArray { add(JsonPrimitive(name.lowercase())); add(JsonPrimitive(value)) })
            }
            request.body?.contentType()?.let { type ->
                if (request.header("Content-Type") == null) add(buildJsonArray { add(JsonPrimitive("content-type")); add(JsonPrimitive(type.toString())) })
            }
            val cookies = cookieJar.loadForRequest(request.url)
            if (cookies.isNotEmpty()) {
                add(buildJsonArray { add(JsonPrimitive("cookie")); add(JsonPrimitive(cookies.joinToString("; ") { "${it.name}=${it.value}" })) })
            }
        }
        val head = buildJsonObject {
            put("method", request.method)
            put("path", path)
            put("headers", headers)
        }.toString()

        val sealed = NativeSealed.seal(keyConfig, head, bodyBytes)
        val handle = ByteBuffer.wrap(sealed, 0, 8).long
        val outer = Request.Builder()
            .url(gatewayUrl)
            .post(sealed.copyOfRange(8, sealed.size).toRequestBody(OHTTP_REQUEST))
            .build()
        val reply = try {
            website.newCall(outer).execute().use { response ->
                if (response.code != 200) throw IOException("The backup route refused the request (${response.code})")
                response.body?.bytes() ?: ByteArray(0)
            }
        } catch (e: IOException) {
            NativeSealed.discard(handle)
            throw e
        }
        val opened = NativeSealed.open(handle, reply)
        val headLength = ByteBuffer.wrap(opened, 0, 4).int
        val answer = kotlinx.serialization.json.Json.parseToJsonElement(String(opened, 4, headLength, Charsets.UTF_8)).jsonObject
        val responseHeaders = Headers.Builder().apply {
            for (pair in answer["headers"]?.jsonArray ?: JsonArray(emptyList())) {
                val parts = pair.jsonArray
                if (parts.size == 2) addUnsafeNonAscii(parts[0].jsonPrimitive.content, parts[1].jsonPrimitive.content)
            }
        }.build()
        cookieJar.saveFromResponse(request.url, Cookie.parseAll(request.url, responseHeaders))
        val body = opened.copyOfRange(4 + headLength, opened.size)
        return Response.Builder()
            .request(request)
            .protocol(Protocol.HTTP_1_1)
            .code(answer["status"]!!.jsonPrimitive.int)
            .message("")
            .headers(responseHeaders)
            .body(body.toResponseBody(responseHeaders["Content-Type"]?.toMediaTypeOrNull()))
            .build()
    }

    companion object {
        const val SWITCH_AFTER_FAILURES = 3
        const val SWITCH_AFTER_SECONDS = 30L
        /** One sealed message (Files parts fit). */
        const val MAX_SEALED_BODY = 8L * 1024 * 1024 + 512 * 1024
        private val OHTTP_REQUEST = "message/ohttp-req".toMediaType()
        private val SKIPPED_HEADERS = setOf(
            "host", "connection", "keep-alive", "proxy-connection", "transfer-encoding", "te", "upgrade", "trailer",
            "content-length", "expect", "user-agent", "forwarded", "x-forwarded-for", "via", "accept-encoding", "cookie",
        )

        /** The gateway's public key configuration from its hex (BuildConfig.SEALED_KEY_CONFIG); empty: no backup route. */
        fun keyFromHex(hex: String): ByteArray = ByteArray(hex.length / 2) { i -> hex.substring(2 * i, 2 * i + 2).toInt(16).toByte() }
    }
}
