// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

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
    fun install(builder: OkHttpClient.Builder, tor: TorGateSource): OkHttpClient.Builder {
        SocksCredentials.install(tor)
        return builder
            .addInterceptor(TorGate(tor))
            .proxySelector(GateSelector(tor))
            .dns(NoDns)
            // A circuit can take a while; an onion round trip is several hops each way.
            .connectTimeout(90, TimeUnit.SECONDS)
            .readTimeout(60, TimeUnit.SECONDS)
            .writeTimeout(60, TimeUnit.SECONDS)
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
}
