// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "redvault/net/http.hpp"

namespace redvault::net {

/// The embedded Tor gateway's loopback SOCKS5 port (tor-core's rv_tor_endpoint).
struct SocksEndpoint {
    std::string host;  // always 127.0.0.1
    int port = 0;
    std::string username;
    std::string password;
};

/**
 * HttpTransport over libcurl. Sends no User-Agent, never follows redirects,
 * only speaks http/https, and uses the OS certificate store for TLS. curl's
 * own cookie engine stays off: SessionCookieJar owns cookies.
 *
 * Thread-safe: every send() uses an easy handle of its own, taken from a
 * small pool so connections are reused (over Tor each new connection is a
 * new stream through the onion service, which costs seconds).
 */
class CurlTransport final : public HttpTransport {
public:
    struct Options {
        std::chrono::seconds connectTimeout{15};
        /// Abort if the transfer stalls (no bytes) for this long, like OkHttp's read timeout.
        std::chrono::seconds stallTimeout{30};
        /// When set, requests to any other host throw NetworkError before a
        /// connection is opened, so no third party ever sees this computer's IP.
        std::string allowedHost;
        /**
         * Tor-only routing (docs/tor-routing-plan.md). When set, every request:
         * - must go to `allowedHost`, which must be a .onion address, over plain
         *   http (the onion service itself authenticates and encrypts);
         * - goes through the SOCKS5 gate this returns, as socks5h, so the host
         *   name is never resolved on this computer;
         * - may only open a socket to that gate (an opensocket guard refuses
         *   anything else, so a lost proxy setting fails instead of leaking);
         * - ignores proxy environment variables.
         * Returning nullopt (Tor not running) fails every request with
         * TorUnavailableError before any socket is opened. There is no
         * fallback to a direct connection.
         */
        std::function<std::optional<SocksEndpoint>()> torGate;
    };

    CurlTransport();
    explicit CurlTransport(Options options);
    ~CurlTransport() override;

    HttpResponse send(const HttpRequest& request) override;
    HttpResponse sendStreaming(const HttpRequest& request, const StreamOptions& streams) override;

    /// For the leak tests: sockets libcurl asked for in Tor mode ("ip:port"), and those the guard refused.
    struct SocketLog {
        std::size_t allowed = 0;
        std::vector<std::string> refused;
    };
    SocketLog socketLog() const;

private:
    struct Pool;
    Options options_;
    std::shared_ptr<Pool> pool_;
};

/// True when `host` is a v3 onion address (56 base32 characters + ".onion").
bool isOnionHost(std::string_view host);

}  // namespace redvault::net
