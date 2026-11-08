// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.api

import com.redvaultsystems.redvaultmessage.devicekeys.V3Service

import com.redvaultsystems.redvaultmessage.tor.TorController
import com.redvaultsystems.redvaultmessage.tor.TorRouting
import kotlinx.serialization.json.Json
import okhttp3.Cookie
import okhttp3.CookieJar
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.Interceptor
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Response
import retrofit2.Retrofit
import retrofit2.converter.kotlinx.serialization.asConverterFactory
import java.io.IOException
import java.util.concurrent.TimeUnit

object ApiClient {
    val json = Json {
        ignoreUnknownKeys = true
        explicitNulls = false
    }

    /**
     * Shared by the API, the avatar loader and file transfers, so all of
     * them carry the session cookie and nothing else identifying, and none
     * of them can reach any host but [apiBase]'s.
     */
    fun httpClient(cookieJar: SessionCookieJar, apiBase: String, tor: TorController? = null): OkHttpClient =
        OkHttpClient.Builder()
            .cookieJar(cookieJar)
            // Before any connection is opened, so a refused host never even sees a TCP handshake.
            .addInterceptor(OwnServerOnly(apiBase.toHttpUrl().host))
            .addNetworkInterceptor(StripIdentifyingHeaders)
            // A redirect would skip OwnServerOnly; the API never sends one.
            .followRedirects(false)
            .followSslRedirects(false)
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(30, TimeUnit.SECONDS)
            // An onion API base: everything through the Tor gate, nothing direct (tor/TorRouting.kt).
            .apply { if (tor != null) TorRouting.install(this, tor) }
            .build()

    /**
     * The client for LiveKit's signaling (group calls): the call server's host only (the API's, or with
     * Tor routing the clearnet call server the API names, since calls are WebRTC + TURN and never Tor),
     * no session cookie and none of the headers [StripIdentifyingHeaders] drops.
     */
    fun signalingClient(signalingHost: String): OkHttpClient =
        OkHttpClient.Builder()
            .addInterceptor(OwnServerOnly(signalingHost))
            .addInterceptor(StripCallClientInfo)
            .addNetworkInterceptor(StripIdentifyingHeaders)
            .followRedirects(false)
            .followSslRedirects(false)
            .connectTimeout(15, TimeUnit.SECONDS)
            .build()

    /**
     * For public reads (channels): [apiBase]'s host only and no identifying headers, like [httpClient],
     * but with no cookie jar, so the session cookie never goes with them and the server can't tell
     * which account is reading. With an onion API base it goes through the Tor gate too: a direct
     * request would ask the phone's resolver for the onion name.
     */
    fun publicClient(apiBase: String, tor: TorController? = null): OkHttpClient =
        OkHttpClient.Builder()
            .addInterceptor(OwnServerOnly(apiBase.toHttpUrl().host))
            .addNetworkInterceptor(StripIdentifyingHeaders)
            .followRedirects(false)
            .followSslRedirects(false)
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(30, TimeUnit.SECONDS)
            .apply { if (tor != null) TorRouting.install(this, tor) }
            .build()

    fun create(baseUrl: String, client: OkHttpClient): ApiService =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(ApiService::class.java)

    /** The live channel, on a copy of [client] that waits out the 25-second long poll. */
    fun createLive(baseUrl: String, client: OkHttpClient): LiveApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client.newBuilder().readTimeout(45, TimeUnit.SECONDS).build())
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(LiveApi::class.java)

    /** Push registration and the background connection's long poll (data/api/PushApi.kt), with [createLive]'s longer wait. */
    fun createPush(baseUrl: String, client: OkHttpClient): PushApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client.newBuilder().readTimeout(45, TimeUnit.SECONDS).build())
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(PushApi::class.java)

    /** Linked devices and the transparency log (data/api/SecurityApi.kt). */
    fun createSecurity(baseUrl: String, client: OkHttpClient): SecurityApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(SecurityApi::class.java)

    /** Per-device keys, protocol 3 (devicekeys/V3Api.kt). Development only. */
    fun createV3(baseUrl: String, client: OkHttpClient): V3Service =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(V3Service::class.java)

    /** One-to-one calls and call links (data/api/CallApi.kt). */
    fun createCalls(baseUrl: String, client: OkHttpClient): CallApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(CallApi::class.java)

    /** Group invite links (data/api/GroupInviteApi.kt). */
    fun createGroupInvites(baseUrl: String, client: OkHttpClient): GroupInviteApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(GroupInviteApi::class.java)

    /** Channels you run (data/api/ChannelApi.kt), on the signed-in client. */
    fun createChannels(baseUrl: String, client: OkHttpClient): ChannelApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(ChannelApi::class.java)

    /** Reading channels, on [publicClient]: pass that client, never the signed-in one. */
    fun createPublicChannels(baseUrl: String, client: OkHttpClient): PublicChannelApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(PublicChannelApi::class.java)

    /** Paid and free communities and their key epochs (data/api/CommunityApi.kt). */
    fun createCommunities(baseUrl: String, client: OkHttpClient): CommunityApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(CommunityApi::class.java)

    /** Stories (data/api/StoriesApi.kt). */
    fun createStories(baseUrl: String, client: OkHttpClient): StoriesApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(StoriesApi::class.java)

    /** Answers to stickers and their totals (data/api/StoryInteractionsApi.kt). */
    fun createStoryInteractions(baseUrl: String, client: OkHttpClient): StoryInteractionsApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(StoryInteractionsApi::class.java)

    /** Files and the password vault (a separate interface: data/api/DriveApi.kt). */
    fun createShares(baseUrl: String, client: OkHttpClient): ShareApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(ShareApi::class.java)

    fun createDrive(baseUrl: String, client: OkHttpClient): DriveApi =
        Retrofit.Builder()
            .baseUrl(baseUrl)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(DriveApi::class.java)
}

/**
 * Refuses every request to a host other than our own API's, so no third
 * party (a storage provider, a CDN, a tracker) ever learns this phone's IP
 * address or when it's in use.
 */
class OwnServerOnly(private val allowedHost: String) : Interceptor {
    override fun intercept(chain: Interceptor.Chain): Response {
        val host = chain.request().url.host
        if (!host.equals(allowedHost, ignoreCase = true)) throw IOException("Refusing to contact $host; this app only talks to $allowedHost")
        return chain.proceed(chain.request())
    }
}

/**
 * LiveKit's SDK puts the phone's model, Android version and network type in the
 * signaling URL's query. The call server has no use for them (it needs only
 * "sdk", "version" and the protocol numbers), so they never leave the phone.
 */
object StripCallClientInfo : Interceptor {
    val PARAMS = listOf("device_model", "os", "os_version", "network")

    override fun intercept(chain: Interceptor.Chain): Response {
        val request = chain.request()
        val url = request.url.newBuilder().apply { PARAMS.forEach { removeAllQueryParameters(it) } }.build()
        return chain.proceed(request.newBuilder().url(url).build())
    }
}

/**
 * The server strips fingerprinting headers on arrival
 * (server/src/middleware/noMetadata.ts); don't send them in the first place.
 * Runs as a network interceptor because OkHttp adds its own User-Agent after
 * application interceptors.
 */
object StripIdentifyingHeaders : Interceptor {
    val HEADERS = listOf("User-Agent", "Accept-Language", "Referer", "X-Requested-With")

    override fun intercept(chain: Interceptor.Chain): Response =
        chain.proceed(chain.request().newBuilder().apply { HEADERS.forEach(::removeHeader) }.build())
}

/**
 * Holds the server's session cookie (and the half-session cookie used
 * between login and MFA verify) in memory only. That matches the private
 * key's lifetime: when the process dies, both are gone together and the
 * user signs in again.
 */
class SessionCookieJar : CookieJar {
    private val cookies = mutableListOf<Cookie>()

    @Synchronized
    override fun saveFromResponse(url: HttpUrl, cookies: List<Cookie>) {
        for (cookie in cookies) {
            this.cookies.removeAll { it.name == cookie.name && it.domain == cookie.domain && it.path == cookie.path }
            if (cookie.expiresAt > System.currentTimeMillis()) this.cookies.add(cookie)
        }
    }

    @Synchronized
    override fun loadForRequest(url: HttpUrl): List<Cookie> {
        val now = System.currentTimeMillis()
        cookies.removeAll { it.expiresAt <= now }
        return cookies.filter { it.matches(url) }
    }

    @Synchronized
    fun clear() {
        cookies.clear()
    }
}
