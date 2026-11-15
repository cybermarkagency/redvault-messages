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
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.ResponseBody
import okhttp3.ResponseBody.Companion.asResponseBody
import okhttp3.ResponseBody.Companion.toResponseBody
import okio.Buffer
import okio.Sink
import okio.Source
import okio.Timeout
import okio.buffer
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
 *
 * A chat file bigger than one sealed message goes up in `Content-Range` parts and comes down in `Range`s of
 * [PART_BYTES], each part its own sealed request (docs/tor-sealed-fallback.md, section 5).
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
    private val sealer: Sealer = NativeSealer,
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

    /** One sealed exchange's answer: the inner response's status, headers and body. */
    private class Answer(val status: Int, val headers: Headers, val body: ByteArray)

    /** Stops writing an upload's body when the server ends it early with this answer. */
    private class UploadEnded(val answer: Answer) : IOException("The server ended the upload")

    private fun sendSealed(request: Request): Response {
        val url = request.url.toString()
        val prefix = onionOrigin + pathPrefix
        if (!url.startsWith("$prefix/api/")) throw IOException("Only the API can use the backup route")
        val path = url.substring(prefix.length)

        val headers = mutableListOf<Pair<String, String>>()
        for ((name, value) in request.headers) {
            if (name.lowercase() !in SKIPPED_HEADERS) headers.add(name.lowercase() to value)
        }
        request.body?.contentType()?.let { type ->
            if (request.header("Content-Type") == null) headers.add("content-type" to type.toString())
        }

        val body = request.body
        if (body != null) {
            val length = body.contentLength()
            if (length > MAX_SEALED_BODY && request.method == "PUT" && UPLOAD_IN_PARTS.matches(path)) {
                return uploadInParts(request, path, headers, body, length)
            }
            if (length > MAX_SEALED_BODY) throw IOException(TOO_LARGE)
            val bytes = Buffer().also { body.writeTo(it) }.also { if (it.size > MAX_SEALED_BODY) throw IOException(TOO_LARGE) }.readByteArray()
            return toResponse(request, sealOnce(request, request.method, path, headers, bytes))
        }
        if (request.method == "GET" && request.header("Range") == null && DOWNLOAD_IN_RANGES.matches(path)) {
            return downloadInRanges(request, path, headers)
        }
        return toResponse(request, sealOnce(request, request.method, path, headers, ByteArray(0)))
    }

    private fun sealOnce(request: Request, method: String, path: String, headers: List<Pair<String, String>>, body: ByteArray): Answer {
        val headerList = buildJsonArray {
            for ((name, value) in headers) add(buildJsonArray { add(JsonPrimitive(name)); add(JsonPrimitive(value)) })
            val cookies = cookieJar.loadForRequest(request.url)
            if (cookies.isNotEmpty()) {
                add(buildJsonArray { add(JsonPrimitive("cookie")); add(JsonPrimitive(cookies.joinToString("; ") { "${it.name}=${it.value}" })) })
            }
        }
        val head = buildJsonObject {
            put("method", method)
            put("path", path)
            put("headers", headerList)
        }.toString()

        val sealed = sealer.seal(keyConfig, head, body)
        val outer = Request.Builder()
            .url(gatewayUrl)
            .post(sealed.message.toRequestBody(OHTTP_REQUEST))
            .build()
        val reply = try {
            website.newCall(outer).execute().use { response ->
                if (response.code != 200) throw IOException("The backup route refused the request (${response.code})")
                response.body?.bytes() ?: ByteArray(0)
            }
        } catch (e: IOException) {
            sealed.discard()
            throw e
        }
        val opened = sealed.open(reply)
        val headLength = ByteBuffer.wrap(opened, 0, 4).int
        val answer = kotlinx.serialization.json.Json.parseToJsonElement(String(opened, 4, headLength, Charsets.UTF_8)).jsonObject
        val responseHeaders = Headers.Builder().apply {
            for (pair in answer["headers"]?.jsonArray ?: JsonArray(emptyList())) {
                val parts = pair.jsonArray
                if (parts.size == 2) addUnsafeNonAscii(parts[0].jsonPrimitive.content, parts[1].jsonPrimitive.content)
            }
        }.build()
        cookieJar.saveFromResponse(request.url, Cookie.parseAll(request.url, responseHeaders))
        return Answer(answer["status"]!!.jsonPrimitive.int, responseHeaders, opened.copyOfRange(4 + headLength, opened.size))
    }

    /** One part of a file: sending it again is harmless (the server replaces a part), so a part that got no answer is tried again. */
    private fun sealPart(request: Request, method: String, path: String, headers: List<Pair<String, String>>, body: ByteArray): Answer {
        var attempt = 1
        while (true) {
            try {
                return sealOnce(request, method, path, headers, body)
            } catch (e: TorUnavailableException) {
                throw e
            } catch (e: IOException) {
                if (attempt >= PART_ATTEMPTS) throw e
                attempt++
            }
        }
    }

    private fun toResponse(request: Request, answer: Answer, body: ResponseBody? = null): Response =
        Response.Builder()
            .request(request)
            .protocol(Protocol.HTTP_1_1)
            .code(answer.status)
            .message("")
            .headers(answer.headers)
            .body(body ?: answer.body.toResponseBody(answer.headers["Content-Type"]?.toMediaTypeOrNull()))
            .build()

    /**
     * A chat file bigger than one sealed message (docs/tor-sealed-fallback.md, section 5): PARTs of exactly
     * [PART_BYTES] (the last one the rest) with `Content-Range`, each its own sealed request, read from the
     * body as it's written (never the whole file in memory). The server answers 202 to each part but the last.
     */
    private fun uploadInParts(request: Request, path: String, headers: List<Pair<String, String>>, body: RequestBody, total: Long): Response {
        val pending = Buffer()
        var start = 0L
        var ended = false
        fun send(last: Boolean): Answer {
            val bytes = pending.readByteArray()
            val end = start + bytes.size - 1
            val answer = sealPart(request, "PUT", path, headers + ("content-range" to "bytes $start-$end/$total"), bytes)
            start += bytes.size
            if (!last && answer.status != 202) {
                ended = true
                throw UploadEnded(answer)
            }
            return answer
        }
        val parts = object : Sink {
            override fun write(source: Buffer, byteCount: Long) {
                if (ended) {
                    source.skip(byteCount) // the server already answered; the rest of the body goes nowhere
                    return
                }
                var remaining = byteCount
                while (remaining > 0) {
                    val take = minOf(remaining, PART_BYTES - pending.size)
                    pending.write(source, take)
                    remaining -= take
                    if (pending.size == PART_BYTES && start + PART_BYTES < total) send(last = false)
                }
            }
            override fun flush() {}
            override fun timeout() = Timeout.NONE
            override fun close() {}
        }
        try {
            parts.buffer().use { body.writeTo(it) }
        } catch (e: UploadEnded) {
            return toResponse(request, e.answer)
        }
        if (start + pending.size != total) throw IOException("Upload stopped")
        return toResponse(request, send(last = true))
    }

    /**
     * A chat file download in `Range`s of [PART_BYTES], each its own sealed request. The first comes now (for the
     * status), the rest as the caller reads; to the caller it's one 200 with the whole file, as the onion sends it.
     */
    private fun downloadInRanges(request: Request, path: String, headers: List<Pair<String, String>>): Response {
        val first = sealOnce(request, "GET", path, headers + ("range" to "bytes=0-${PART_BYTES - 1}"), ByteArray(0))
        if (first.status == 416) return toResponse(request, sealOnce(request, "GET", path, headers, ByteArray(0)))
        if (first.status != 206) return toResponse(request, first) // a route without ranges: the whole (small) body
        val (_, firstEnd, total) = contentRange(first, 0, null)
        val source = object : Source {
            private val ready = Buffer().write(first.body)
            private var next = firstEnd + 1
            override fun read(sink: Buffer, byteCount: Long): Long {
                if (ready.exhausted()) {
                    if (next >= total) return -1
                    val answer = sealPart(request, "GET", path, headers + ("range" to "bytes=$next-${next + PART_BYTES - 1}"), ByteArray(0))
                    if (answer.status != 206) throw IOException("Download stopped (${answer.status})")
                    val (_, end, _) = contentRange(answer, next, total)
                    ready.write(answer.body)
                    next = end + 1
                }
                return ready.read(sink, byteCount)
            }
            override fun timeout() = Timeout.NONE
            override fun close() {}
        }
        val whole = first.headers.newBuilder().removeAll("Content-Range").set("Content-Length", total.toString()).build()
        return toResponse(
            request,
            Answer(200, whole, ByteArray(0)),
            source.buffer().asResponseBody(first.headers["Content-Type"]?.toMediaTypeOrNull(), total),
        )
    }

    /** A 206's `Content-Range` (start, end, total), checked against the offset asked for and the body. */
    private fun contentRange(answer: Answer, expectedStart: Long, expectedTotal: Long?): Triple<Long, Long, Long> {
        val match = CONTENT_RANGE.matchEntire(answer.headers["Content-Range"] ?: "") ?: throw IOException("The backup route's download is malformed")
        val (start, end, total) = match.destructured.toList().map { it.toLong() }
        if (start != expectedStart || end < start || end >= total || (expectedTotal != null && total != expectedTotal) ||
            answer.body.size.toLong() != end - start + 1
        ) {
            throw IOException("The backup route's download is malformed")
        }
        return Triple(start, end, total)
    }

    /** Seals a request and opens its answer: crypto-core's Oblivious HTTP, unless a test passes its own. */
    interface Sealer {
        /** One sealed request: [message] is the message/ohttp-req body; [open] or [discard] exactly once. */
        interface Sealed {
            val message: ByteArray
            /** The answer: a 4-byte head length, the head (JSON), then the body. */
            fun open(reply: ByteArray): ByteArray
            fun discard()
        }
        fun seal(keyConfig: ByteArray, headJson: String, body: ByteArray): Sealed
    }

    /** crypto-core through JNI. */
    object NativeSealer : Sealer {
        override fun seal(keyConfig: ByteArray, headJson: String, body: ByteArray): Sealer.Sealed {
            val sealed = NativeSealed.seal(keyConfig, headJson, body)
            val handle = ByteBuffer.wrap(sealed, 0, 8).long
            return object : Sealer.Sealed {
                override val message = sealed.copyOfRange(8, sealed.size)
                override fun open(reply: ByteArray) = NativeSealed.open(handle, reply)
                override fun discard() = NativeSealed.discard(handle)
            }
        }
    }

    companion object {
        const val SWITCH_AFTER_FAILURES = 3
        const val SWITCH_AFTER_SECONDS = 30L
        /** One sealed message (Files parts fit). */
        const val MAX_SEALED_BODY = 8L * 1024 * 1024 + 512 * 1024
        /**
         * A chat file bigger than one sealed message crosses the backup route in parts of this size: Content-Range
         * uploads and Range downloads (server/src/services/fileService.ts, FILE_UPLOAD_PART_BYTES).
         */
        const val PART_BYTES = 8L * 1024 * 1024
        /** How often one part is tried before the transfer fails. */
        const val PART_ATTEMPTS = 3
        private const val TOO_LARGE = "Large files wait for the onion address: the backup route carries up to 8 MB at a time"
        /** The chat file content the server takes in parts and serves in ranges. */
        private val UPLOAD_IN_PARTS = Regex("^/api/v1/files/[0-9A-Fa-f-]{36}/content$")
        private val DOWNLOAD_IN_RANGES = UPLOAD_IN_PARTS
        private val CONTENT_RANGE = Regex("^bytes ([0-9]{1,15})-([0-9]{1,15})/([0-9]{1,15})$")
        private val OHTTP_REQUEST = "message/ohttp-req".toMediaType()
        private val SKIPPED_HEADERS = setOf(
            "host", "connection", "keep-alive", "proxy-connection", "transfer-encoding", "te", "upgrade", "trailer",
            "content-length", "expect", "user-agent", "forwarded", "x-forwarded-for", "via", "accept-encoding", "cookie",
        )

        /** The gateway's public key configuration from its hex (BuildConfig.SEALED_KEY_CONFIG); empty: no backup route. */
        fun keyFromHex(hex: String): ByteArray = ByteArray(hex.length / 2) { i -> hex.substring(2 * i, 2 * i + 2).toInt(16).toByte() }
    }
}
