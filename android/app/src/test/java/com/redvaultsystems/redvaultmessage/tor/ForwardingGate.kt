// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

import java.io.DataInputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread

/**
 * A stand-in for tor-core's loopback SOCKS5 gate that hands each connection on to a local test server
 * ([forwardToPort], e.g. a MockWebServer) as Tor would hand it to the onion service. It records the target
 * each connection asked for, so a test can check the app named the onion host and never looked anything up.
 * [up] false: the gate is "unavailable" and [source] gives no gate, as TorController does while Tor is down.
 */
class ForwardingGate(private val forwardToPort: Int) : AutoCloseable {
    class Target(val addressType: Int, val host: String, val port: Int)

    val username = "u" + "1".repeat(31)
    val password = "p" + "2".repeat(31)
    private val server = ServerSocket(0, 50, InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1)))
    val port: Int get() = server.localPort
    val targets = CopyOnWriteArrayList<Target>()
    val connections = AtomicInteger()

    @Volatile var up = true

    val source = TorGateSource { if (up) SocksEndpoint("127.0.0.1", port, username, password) else null }

    init {
        thread(isDaemon = true) {
            while (!server.isClosed) {
                val socket = runCatching { server.accept() }.getOrNull() ?: break
                connections.incrementAndGet()
                thread(isDaemon = true) { runCatching { handle(socket) } }
            }
        }
    }

    private fun handle(socket: Socket) {
        val input = DataInputStream(socket.getInputStream())
        val output = socket.getOutputStream()
        check(input.readUnsignedByte() == 5)
        ByteArray(input.readUnsignedByte()).also(input::readFully)
        output.write(byteArrayOf(5, 2))
        check(input.readUnsignedByte() == 1)
        val user = String(ByteArray(input.readUnsignedByte()).also(input::readFully))
        val pass = String(ByteArray(input.readUnsignedByte()).also(input::readFully))
        if (user != username || pass != password) {
            output.write(byteArrayOf(1, 1))
            socket.close()
            return
        }
        output.write(byteArrayOf(1, 0))
        check(input.readUnsignedByte() == 5)
        check(input.readUnsignedByte() == 1) // CONNECT
        input.readUnsignedByte()
        val type = input.readUnsignedByte()
        val host = when (type) {
            3 -> String(ByteArray(input.readUnsignedByte()).also(input::readFully))
            1 -> InetAddress.getByAddress(ByteArray(4).also(input::readFully)).hostAddress
            else -> InetAddress.getByAddress(ByteArray(16).also(input::readFully)).hostAddress
        }
        val port = input.readUnsignedShort()
        targets += Target(type, host, port)
        output.write(byteArrayOf(5, 0, 0, 1, 0, 0, 0, 0, 0, 0))
        output.flush()
        val upstream = Socket(InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1)), forwardToPort)
        thread(isDaemon = true) { runCatching { upstream.getInputStream().copyTo(output) }; runCatching { socket.close() } }
        runCatching { input.copyTo(upstream.getOutputStream()) }
        runCatching { upstream.close() }
    }

    override fun close() = server.close()
}
