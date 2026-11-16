// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

import okhttp3.Call
import okhttp3.Dns
import okhttp3.Interceptor
import okhttp3.OkHttpClient
import okhttp3.Response
import java.io.IOException
import java.net.Authenticator
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.PasswordAuthentication
import java.net.Proxy
import java.net.ProxySelector
import java.net.SocketAddress
import java.net.URI
import java.net.UnknownHostException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit

/** Where the Tor gate is right now; null while nothing may be sent (Tor stopped or unavailable). */
fun interface TorGateSource {
    fun gate(): SocksEndpoint?
}

/**
 * Tor is not connected, so nothing was sent. Not a server error: the outbox
 * keeps the item and sends it once Tor is back, never another way.
 */
class TorUnavailableException : IOException("Tor connection unavailable")

/**
 * An onion request that failed before any byte of it went out: the connection through the Tor gate never opened
 * (SOCKS refused it, Tor answered "host unreachable", the connect timed out). Sending it again, even a POST, can't
 * do anything twice. Thrown only when that is certain ([SendWatch]); any other failure stays a plain IOException.
 */
class NotSentException(cause: IOException) : IOException(cause.message ?: "The request wasn't sent", cause)

/**
 * Tells a failure before sending from one after it. OkHttp runs network interceptors only once a connection is open,
 * right before it writes the request's headers; a call that never reached [network] never wrote a byte. [application]
 * sits last among the application interceptors and turns such a call's failure into a [NotSentException]. A call that
 * reached the network even once (a retry included) counts as sent.
 */
class SendWatch {
    private val reached: MutableSet<Call> = ConcurrentHashMap.newKeySet()

    val network = Interceptor { chain ->
        reached.add(chain.call())
        chain.proceed(chain.request())
    }

    val application = Interceptor { chain ->
        val call = chain.call()
        try {
            chain.proceed(chain.request())
        } catch (e: IOException) {
            if (e is TorUnavailableException || e is NotSentException || call.isCanceled() || call in reached) throw e
            throw NotSentException(e)
        } finally {
            reached.remove(call)
        }
    }
}

/**
 * Routes an OkHttp client through the Tor gate and nowhere else
 * (docs/tor-routing-plan.md):
 *
 * - every connection goes to the loopback SOCKS5 gate, with the hostname
 *   unresolved (OkHttp passes SOCKS targets unresolved, so the gate gets
 *   the .onion name and the phone never looks it up);
 * - [NoDns] makes any other lookup fail instead of leaking to the resolver;
 * - [TorGate] refuses a request before any socket opens when Tor is stopped
 *   or unavailable, and the proxy selector never returns an empty list (an
 *   empty list would make OkHttp connect directly);
 * - the gate's per-start random credentials are answered only for that
 *   loopback port's SOCKS5 request ([SocksCredentials]).
 */
object TorRouting {
    /**
     * [connectTimeoutSeconds]: how long a connection to the onion may take. 90 s by default (a circuit can take a while);
     * shorter ([BACKUP_ROUTE_CONNECT_TIMEOUT_SECONDS]) where the sealed backup route can take over sooner.
     */
    fun install(builder: OkHttpClient.Builder, tor: TorGateSource, connectTimeoutSeconds: Long = DEFAULT_CONNECT_TIMEOUT_SECONDS): OkHttpClient.Builder {
        SocksCredentials.install(tor)
        val watch = SendWatch()
        return builder
            .addInterceptor(TorGate(tor))
            // Last, so it sees the failure exactly as the connection attempt left it.
            .addInterceptor(watch.application)
            .addNetworkInterceptor(watch.network)
            .proxySelector(GateSelector(tor))
            .dns(NoDns)
            // A circuit can take a while; an onion round trip is several hops each way.
            .connectTimeout(connectTimeoutSeconds, TimeUnit.SECONDS)
            .readTimeout(60, TimeUnit.SECONDS)
            .writeTimeout(60, TimeUnit.SECONDS)
    }

    /**
     * The sealed backup route's own client (docs/tor-sealed-fallback.md): the same gate, selector, no-DNS and
     * credentials, but it may only reach [host] over https (what it carries is already sealed to the gateway's key).
     */
    fun installForWebsite(builder: OkHttpClient.Builder, tor: TorGateSource, host: String): OkHttpClient.Builder {
        SocksCredentials.install(tor)
        return builder
            .addInterceptor(WebsiteGate(tor, host.lowercase()))
            .proxySelector(GateSelector(tor))
            .dns(NoDns)
            .connectTimeout(DEFAULT_CONNECT_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .readTimeout(60, TimeUnit.SECONDS)
            .writeTimeout(60, TimeUnit.SECONDS)
    }

    /** The website client's gate: Tor connected, https, and only the one host. */
    class WebsiteGate(private val tor: TorGateSource, private val host: String) : Interceptor {
        override fun intercept(chain: Interceptor.Chain): Response {
            if (tor.gate() == null) throw TorUnavailableException()
            val url = chain.request().url
            if (!url.isHttps || url.host != host || TorController.isOnionHost(url.host)) {
                throw IOException("Tor routing: the backup route only reaches $host over https")
            }
            return chain.proceed(chain.request())
        }
    }

    /** Before any connection: no gate, no request. */
    class TorGate(private val tor: TorGateSource) : Interceptor {
        override fun intercept(chain: Interceptor.Chain): Response {
            if (tor.gate() == null) throw TorUnavailableException()
            if (!TorController.isOnionHost(chain.request().url.host)) throw IOException("Tor routing: only the onion service")
            return chain.proceed(chain.request())
        }
    }

    /** Always exactly one SOCKS proxy: the gate, or a closed loopback port while there's none. */
    class GateSelector(private val tor: TorGateSource) : ProxySelector() {
        override fun select(uri: URI?): List<Proxy> {
            val gate = tor.gate()
            // A literal address, so neither OkHttp nor java.net's SOCKS client looks up "localhost".
            val address = InetSocketAddress(InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1)), gate?.port ?: CLOSED_PORT)
            return listOf(Proxy(Proxy.Type.SOCKS, address))
        }

        override fun connectFailed(uri: URI?, sa: SocketAddress?, ioe: IOException?) {}
    }

    /** The onion name goes to the gate unresolved; any lookup at all is a bug, and fails. */
    object NoDns : Dns {
        override fun lookup(hostname: String): List<InetAddress> =
            throw UnknownHostException("Tor routing: no DNS lookups")
    }

    /**
     * java.net's SOCKS5 client asks the process-wide Authenticator for the
     * username and password. This one answers only the gate's own SOCKS5
     * request on its current loopback port, and nothing else.
     */
    object SocksCredentials : Authenticator() {
        @Volatile private var tor: TorGateSource? = null

        fun install(controller: TorGateSource) {
            tor = controller
            setDefault(this)
        }

        override fun getPasswordAuthentication(): PasswordAuthentication? {
            val gate = tor?.gate() ?: return null
            if (requestorType != RequestorType.SERVER || requestingProtocol != "SOCKS5") return null
            if (requestingPort != gate.port) return null
            val host = requestingHost
            if (host != "127.0.0.1" && requestingSite?.isLoopbackAddress != true) return null
            return PasswordAuthentication(gate.username, gate.password.toCharArray())
        }
    }

    /** "discard": nothing listens there, so a request that somehow got past [TorGate] fails instead of going direct. */
    private const val CLOSED_PORT = 9

    const val DEFAULT_CONNECT_TIMEOUT_SECONDS = 90L
    /** The onion's connect timeout when the sealed backup route exists (the desktop's too). */
    const val BACKUP_ROUTE_CONNECT_TIMEOUT_SECONDS = 30L
}
