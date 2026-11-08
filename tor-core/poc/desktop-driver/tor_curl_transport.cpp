// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "tor_curl_transport.hpp"

#include <curl/curl.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <memory>
#include <mutex>

namespace redvault::net {

struct TorCurlGuard {
    TorCurlTransport* transport;
    int port;
};

namespace {

void ensureCurl() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw NetworkError("libcurl failed to initialise");
    });
}

struct EasyDeleter {
    void operator()(CURL* handle) const { curl_easy_cleanup(handle); }
};

struct SlistDeleter {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

struct BodySink {
    CURL* handle;
    std::string* buffer;
    std::ostream* stream;
};

std::size_t onBody(char* data, std::size_t size, std::size_t count, void* userdata) {
    auto& sink = *static_cast<BodySink*>(userdata);
    long status = 0;
    curl_easy_getinfo(sink.handle, CURLINFO_RESPONSE_CODE, &status);
    if (sink.stream != nullptr && status >= 200 && status <= 299) {
        sink.stream->write(data, static_cast<std::streamsize>(size * count));
        return sink.stream->good() ? size * count : 0;
    }
    sink.buffer->append(data, size * count);
    return size * count;
}

std::size_t onUpload(char* buffer, std::size_t size, std::size_t count, void* userdata) {
    auto& input = *static_cast<std::istream*>(userdata);
    input.read(buffer, static_cast<std::streamsize>(size * count));
    if (input.bad()) return CURL_READFUNC_ABORT;
    return static_cast<std::size_t>(input.gcount());
}

std::size_t onHeader(char* data, std::size_t size, std::size_t count, void* userdata) {
    auto& headers = *static_cast<Headers*>(userdata);
    std::string_view line(data, size * count);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (line.starts_with("HTTP/")) {
        headers.clear();
        return size * count;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return size * count;
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    headers.emplace_back(toLower(line.substr(0, colon)), std::string(value));
    return size * count;
}

/// libcurl asks here for every socket it wants. Only the gateway's loopback port is allowed.
curl_socket_t onOpenSocket(void* userdata, curlsocktype purpose, curl_sockaddr* address) {
    auto& guard = *static_cast<TorCurlGuard*>(userdata);
    std::string text = "unknown";
    int port = -1;
    bool loopbackGateway = false;
    if (purpose == CURLSOCKTYPE_IPCXN && address->family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&address->addr);
        char buffer[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &in->sin_addr, buffer, sizeof buffer);
        text = buffer;
        port = ntohs(in->sin_port);
        loopbackGateway = text == "127.0.0.1" && port == guard.port;
    } else if (address->family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&address->addr);
        char buffer[INET6_ADDRSTRLEN] = {};
        inet_ntop(AF_INET6, &in6->sin6_addr, buffer, sizeof buffer);
        text = buffer;
        port = ntohs(in6->sin6_port);
    }
    if (!guard.transport->admitSocket(text, port) || !loopbackGateway) return CURL_SOCKET_BAD;
    return socket(address->family, address->socktype, address->protocol);
}

void check(CURLcode code) {
    if (code != CURLE_OK) throw NetworkError(curl_easy_strerror(code));
}

}  // namespace

TorCurlTransport::TorCurlTransport(Options options) : options_(std::move(options)) {
    ensureCurl();
    options_.onionHost = toLower(options_.onionHost);
}

void TorCurlTransport::setEndpoint(const TorSocksEndpoint& endpoint) {
    std::lock_guard lock(mutex_);
    endpoint_ = endpoint;
}

void TorCurlTransport::clearEndpoint() {
    std::lock_guard lock(mutex_);
    endpoint_ = TorSocksEndpoint{};
}

TorCurlTransport::SocketLog TorCurlTransport::socketLog() const {
    std::lock_guard lock(mutex_);
    return log_;
}

bool TorCurlTransport::admitSocket(const std::string& address, int port) {
    std::lock_guard lock(mutex_);
    const std::string entry = address + ":" + std::to_string(port);
    const bool ok = address == "127.0.0.1" && port == endpoint_.port && endpoint_.port != 0;
    (ok ? log_.allowed : log_.refused).push_back(entry);
    return ok;
}

HttpResponse TorCurlTransport::send(const HttpRequest& request) {
    return sendStreaming(request, StreamOptions{});
}

HttpResponse TorCurlTransport::sendStreaming(const HttpRequest& request, const StreamOptions& streams) {
    const auto url = parseUrl(request.url);
    if (!url || url->host != options_.onionHost || url->scheme != "http") {
        throw NetworkError("Refusing to contact " + (url ? url->host : request.url) +
                           ": private messages only travel through Redvault's onion service");
    }
    TorSocksEndpoint endpoint;
    {
        std::lock_guard lock(mutex_);
        endpoint = endpoint_;
    }
    if (endpoint.port == 0) throw NetworkError("Tor isn't connected; nothing was sent");

    std::unique_ptr<CURL, EasyDeleter> easy(curl_easy_init());
    if (!easy) throw NetworkError("Couldn't start an HTTP request");
    CURL* handle = easy.get();

    HttpResponse response;
    char errorBuffer[CURL_ERROR_SIZE] = {};
    TorCurlGuard guard{this, endpoint.port};
    const std::string proxy = "socks5h://127.0.0.1:" + std::to_string(endpoint.port);

    check(curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str()));
    // The onion service authenticates and encrypts end to end; plain http inside it.
    check(curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http"));
    check(curl_easy_setopt(handle, CURLOPT_PROXY, proxy.c_str()));
    check(curl_easy_setopt(handle, CURLOPT_PROXYUSERNAME, endpoint.username.c_str()));
    check(curl_easy_setopt(handle, CURLOPT_PROXYPASSWORD, endpoint.password.c_str()));
    // An explicit proxy beats http_proxy/ALL_PROXY; an empty NOPROXY means no host may bypass it.
    check(curl_easy_setopt(handle, CURLOPT_NOPROXY, ""));
    check(curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION, &onOpenSocket));
    check(curl_easy_setopt(handle, CURLOPT_OPENSOCKETDATA, &guard));
    check(curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L));
    check(curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L));
    check(curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer));
    check(curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(options_.connectTimeout.count())));
    check(curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L));
    check(curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, static_cast<long>(options_.stallTimeout.count())));

    if (streams.upload != nullptr) {
        check(curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L));
        if (request.method != "PUT") check(curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str()));
        check(curl_easy_setopt(handle, CURLOPT_READFUNCTION, &onUpload));
        check(curl_easy_setopt(handle, CURLOPT_READDATA, streams.upload));
        check(curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(streams.uploadSize)));
    } else if (request.method == "GET") {
        check(curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L));
    } else if (request.method == "HEAD") {
        check(curl_easy_setopt(handle, CURLOPT_NOBODY, 1L));
    } else {
        check(curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str()));
        if (!request.body.empty() || request.method == "POST" || request.method == "PUT" ||
            request.method == "PATCH") {
            check(curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.data()));
            check(curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                                   static_cast<curl_off_t>(request.body.size())));
        }
    }

    std::unique_ptr<curl_slist, SlistDeleter> headerList;
    auto append = [&](const std::string& line) {
        curl_slist* next = curl_slist_append(headerList.get(), line.c_str());
        if (next == nullptr) throw NetworkError("Out of memory building request headers");
        headerList.release();
        headerList.reset(next);
    };
    bool hasContentType = false;
    bool hasAccept = false;
    for (const auto& [name, value] : request.headers) {
        append(name + ": " + value);
        hasContentType = hasContentType || toLower(name) == "content-type";
        hasAccept = hasAccept || toLower(name) == "accept";
    }
    append("Expect:");
    if (!hasAccept) append("Accept:");
    if (!hasContentType) append("Content-Type:");
    check(curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList.get()));

    check(curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &onBody));
    BodySink sink{handle, &response.body, streams.download};
    check(curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink));
    check(curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &onHeader));
    check(curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response.headers));

    const CURLcode result = curl_easy_perform(handle);
    if (result != CURLE_OK) {
        throw NetworkError(errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(result));
    }
    long status = 0;
    check(curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status));
    response.status = static_cast<int>(status);
    return response;
}

}  // namespace redvault::net
