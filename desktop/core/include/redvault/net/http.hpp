// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace redvault::net {

/// Header names are stored lowercased, so lookups are case-insensitive by construction.
using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    std::string method;
    std::string url;  // absolute
    Headers headers;
    std::string body;
};

struct HttpResponse {
    int status = 0;
    Headers headers;
    std::string body;

    /// First header with this (lowercase) name.
    std::optional<std::string> header(std::string_view name) const;
    /// Every header with this (lowercase) name, e.g. several set-cookie lines.
    std::vector<std::string> headersNamed(std::string_view name) const;
};

/// The request never got an HTTP response: DNS, connect, TLS or timeout failure.
class NetworkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Tor routing is on but the gateway isn't running: nothing was sent, and nothing may go any other way.
class TorUnavailableError : public NetworkError {
public:
    using NetworkError::NetworkError;
};

/**
 * Optional streaming for large bodies (encrypted files). `upload`, when set,
 * is sent as the request body (exactly `uploadSize` bytes) instead of
 * HttpRequest::body. `download`, when set, receives a 2xx response's body
 * instead of HttpResponse::body; a non-2xx body still lands in
 * HttpResponse::body so errors can be read.
 */
struct StreamOptions {
    std::istream* upload = nullptr;
    std::uint64_t uploadSize = 0;
    std::ostream* download = nullptr;
};

/**
 * One blocking HTTP exchange. The core is transport-agnostic so it can be
 * tested with a fake; CurlTransport is the real one. Implementations must
 * not add identifying headers (User-Agent and friends): the server strips
 * them on arrival (server/src/middleware/noMetadata.ts), and clients
 * shouldn't send them in the first place.
 */
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    /// Throws NetworkError when there's no HTTP response at all. Any status code is a normal return.
    virtual HttpResponse send(const HttpRequest& request) = 0;

    /**
     * send() with streamed bodies. The default buffers through send(), which
     * is fine for fakes; CurlTransport streams for real.
     */
    virtual HttpResponse sendStreaming(const HttpRequest& request, const StreamOptions& streams);
};

struct Url {
    std::string scheme;  // lowercase
    std::string host;    // lowercase, without port
    std::string path;    // starts with '/', without query
};

/// Parses an absolute http(s) URL. Empty on anything else.
std::optional<Url> parseUrl(std::string_view url);

#ifdef NDEBUG
inline constexpr bool kDebugBuild = false;
#else
inline constexpr bool kDebugBuild = true;
#endif

/// This computer, by a name or address that can't mean anything else: localhost, 127.x.x.x or [::1].
bool isLoopbackHost(std::string_view host);

/**
 * Where this client may send anything: https anywhere; plain http only to this
 * computer or a v3 onion service (which encrypts and authenticates on its own).
 * Debug builds also allow plain http elsewhere, for test servers; an address
 * given at run time (--api, REDVAULT_API_BASE) never gets that, so pass
 * allowPlainHttp = false for those.
 */
bool isAllowedServerUrl(std::string_view url, bool allowPlainHttp = kDebugBuild);

/// An address to send someone's browser to: https, nothing else. A page that takes payment details never loads over http.
bool isHttpsUrl(std::string_view url);

/// A Stripe-hosted payment page: https, on Stripe's own checkout hosts. Anything else from the server isn't opened.
bool isCheckoutUrl(std::string_view url);

/// Percent-encodes one path segment or query value (RFC 3986 unreserved characters pass through).
std::string percentEncode(std::string_view text);

std::string toLower(std::string_view text);

}  // namespace redvault::net
