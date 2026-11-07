#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "redvault/net/http.hpp"

namespace redvault::net {

/// Where the embedded Tor gateway listens (rv_tor_endpoint).
struct TorSocksEndpoint {
    std::string host;  // always 127.0.0.1
    int port = 0;
    std::string username;
    std::string password;
};

/**
 * Proof of concept of the desktop transport for Tor-only routing: the same
 * exchange as CurlTransport, but every request goes through the embedded
 * gateway's SOCKS5 port, and nothing else is possible:
 *
 * - only the Redvault onion host is accepted (checked before libcurl runs);
 * - `socks5h`: libcurl hands the hostname to the gateway and never resolves
 *   it, so no DNS query leaves this computer;
 * - an opensocket guard refuses any socket that isn't 127.0.0.1:<socks port>,
 *   so a missing or overridden proxy setting fails instead of connecting
 *   directly (and is recorded as a violation for the leak tests);
 * - environment proxies (http_proxy, NO_PROXY...) can't redirect or bypass it;
 * - redirects are never followed.
 *
 * With no endpoint (Tor not started or stopped) every request throws
 * NetworkError without opening a socket: fail closed.
 */
class TorCurlTransport final : public HttpTransport {
public:
    struct Options {
        std::string onionHost;
        /// First contact with an onion service can take tens of seconds.
        std::chrono::seconds connectTimeout{90};
        /// Long polls hold for 25 s; over Tor allow for the extra latency.
        std::chrono::seconds stallTimeout{60};
    };

    explicit TorCurlTransport(Options options);

    /// Called when the gateway starts or stops; an empty port means "no Tor".
    void setEndpoint(const TorSocksEndpoint& endpoint);
    void clearEndpoint();

    HttpResponse send(const HttpRequest& request) override;
    HttpResponse sendStreaming(const HttpRequest& request, const StreamOptions& streams) override;

    /// For the leak tests: every socket libcurl asked for, and those refused.
    struct SocketLog {
        std::vector<std::string> allowed;
        std::vector<std::string> refused;
    };
    SocketLog socketLog() const;

    /// The opensocket guard's verdict (and log entry) for one socket libcurl wants to open.
    bool admitSocket(const std::string& address, int port);

private:
    Options options_;
    mutable std::mutex mutex_;
    TorSocksEndpoint endpoint_;
    SocketLog log_;
};

}  // namespace redvault::net
