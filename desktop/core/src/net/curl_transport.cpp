#include "redvault/net/curl_transport.hpp"

#include <curl/curl.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace redvault::net {

namespace {

void ensureCurl() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw NetworkError("libcurl failed to initialise");
    });
}

#ifdef __linux__
// A libcurl bundled into an AppImage looks for CA certificates where the
// build machine keeps them, but every distribution keeps them somewhere else.
const char* systemCaBundle() {
    static const char* const found = []() -> const char* {
        for (const char* path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                                 "/etc/ssl/ca-bundle.pem", "/etc/pki/tls/cacert.pem", "/etc/ssl/cert.pem"}) {
            std::error_code ignored;
            if (std::filesystem::is_regular_file(path, ignored)) return path;
        }
        return nullptr;
    }();
    return found;
}
#endif

struct SlistDeleter {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

struct BodySink {
    CURL* handle;
    std::string* buffer;
    std::ostream* stream;  // for a 2xx body, when streaming
};

std::size_t onBody(char* data, std::size_t size, std::size_t count, void* userdata) {
    auto& sink = *static_cast<BodySink*>(userdata);
    long status = 0;
    curl_easy_getinfo(sink.handle, CURLINFO_RESPONSE_CODE, &status);
    if (sink.stream != nullptr && status >= 200 && status <= 299) {
        sink.stream->write(data, static_cast<std::streamsize>(size * count));
        // Returning short aborts the transfer (CURLE_WRITE_ERROR) if the disk write failed.
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
    // A new status line (after a 1xx, say) starts a fresh header block.
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

void check(CURLcode code) {
    if (code != CURLE_OK) throw NetworkError(curl_easy_strerror(code));
}

}  // namespace

/**
 * Idle easy handles, reused so their open connections are too
 * (curl_easy_reset keeps a handle's connection cache). Each handle is used by
 * one send() at a time; the pool itself is guarded by a mutex.
 */
struct CurlTransport::Pool {
    static constexpr std::size_t kMaxIdle = 4;
    std::mutex mutex;
    std::vector<CURL*> idle;
    SocketLog log;

    ~Pool() {
        for (CURL* handle : idle) curl_easy_cleanup(handle);
    }

    CURL* acquire() {
        {
            std::lock_guard lock(mutex);
            if (!idle.empty()) {
                CURL* handle = idle.back();
                idle.pop_back();
                curl_easy_reset(handle);
                return handle;
            }
        }
        return curl_easy_init();
    }

    void release(CURL* handle) {
        std::lock_guard lock(mutex);
        if (idle.size() < kMaxIdle) {
            idle.push_back(handle);
            return;
        }
        curl_easy_cleanup(handle);
    }
};

namespace {

/// The Tor-mode socket guard's state for one request.
struct TorGuard {
    int port = 0;
    std::mutex* mutex = nullptr;
    CurlTransport::SocketLog* log = nullptr;
};

/// libcurl asks here for every socket it wants. Only the gate's loopback port is allowed.
curl_socket_t onOpenSocket(void* userdata, curlsocktype purpose, curl_sockaddr* address) {
    auto& guard = *static_cast<TorGuard*>(userdata);
    std::string text = "unknown";
    bool allowed = false;
    if (address->family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&address->addr);
        char buffer[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &in->sin_addr, buffer, sizeof buffer);
        const int port = ntohs(in->sin_port);
        text = std::string(buffer) + ":" + std::to_string(port);
        allowed = purpose == CURLSOCKTYPE_IPCXN && std::string_view(buffer) == "127.0.0.1" && port == guard.port;
    } else if (address->family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&address->addr);
        char buffer[INET6_ADDRSTRLEN] = {};
        inet_ntop(AF_INET6, &in6->sin6_addr, buffer, sizeof buffer);
        text = "[" + std::string(buffer) + "]:" + std::to_string(ntohs(in6->sin6_port));
    }
    {
        std::lock_guard lock(*guard.mutex);
        if (allowed) {
            ++guard.log->allowed;
        } else {
            guard.log->refused.push_back(text);
        }
    }
    if (!allowed) return CURL_SOCKET_BAD;
    return socket(address->family, address->socktype, address->protocol);
}

}  // namespace

bool isOnionHost(std::string_view host) {
    constexpr std::string_view suffix = ".onion";
    if (host.size() != 56 + suffix.size() || !host.ends_with(suffix)) return false;
    for (const char c : host.substr(0, 56)) {
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;
    }
    return true;
}

CurlTransport::CurlTransport() : CurlTransport(Options{}) {}

CurlTransport::CurlTransport(Options options) : options_(std::move(options)), pool_(std::make_shared<Pool>()) {
    ensureCurl();
    if (options_.torGate) {
        options_.allowedHost = toLower(options_.allowedHost);
        if (!isOnionHost(options_.allowedHost)) {
            throw std::invalid_argument("Tor routing needs a .onion server address");
        }
    }
}

CurlTransport::~CurlTransport() = default;

CurlTransport::SocketLog CurlTransport::socketLog() const {
    std::lock_guard lock(pool_->mutex);
    return pool_->log;
}

HttpResponse CurlTransport::send(const HttpRequest& request) {
    return sendStreaming(request, StreamOptions{});
}

HttpResponse CurlTransport::sendStreaming(const HttpRequest& request, const StreamOptions& streams) {
    const bool tor = static_cast<bool>(options_.torGate);
    // Whatever produced this URL (a setting, the server), plain http leaves this computer only for an onion service.
    if (!isAllowedServerUrl(request.url)) throw NetworkError("Refusing an unencrypted connection to " + request.url);
    if (!options_.allowedHost.empty()) {
        const auto url = parseUrl(request.url);
        if (!url || url->host != toLower(options_.allowedHost)) {
            throw NetworkError("Refusing to contact " + (url ? url->host : request.url) + "; this app only talks to " +
                               options_.allowedHost);
        }
        if (tor && url->scheme != "http") {
            throw NetworkError("Inside the onion service the app speaks plain http; refusing " + url->scheme);
        }
    }
    std::optional<SocksEndpoint> gate;
    if (tor) {
        gate = options_.torGate();
        if (!gate || gate->port <= 0) throw TorUnavailableError("Tor isn't connected; nothing was sent");
    }

    CURL* handle = pool_->acquire();
    if (handle == nullptr) throw NetworkError("Couldn't start an HTTP request");
    auto pool = pool_;
    // Back to the pool however this ends (a handle with a half-done transfer is reset on reuse).
    std::unique_ptr<CURL, std::function<void(CURL*)>> holder(handle, [pool](CURL* h) { pool->release(h); });

    HttpResponse response;
    char errorBuffer[CURL_ERROR_SIZE] = {};

    check(curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str()));
    check(curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, tor ? "http" : "http,https"));
    check(curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L));
    check(curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L));
    check(curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer));
    check(curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(options_.connectTimeout.count())));
    check(curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L));
    check(curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, static_cast<long>(options_.stallTimeout.count())));

    TorGuard guard;
    std::string proxy;
    if (tor) {
        // socks5h: the gate gets the host name; nothing is resolved on this computer.
        proxy = "socks5h://127.0.0.1:" + std::to_string(gate->port);
        check(curl_easy_setopt(handle, CURLOPT_PROXY, proxy.c_str()));
        check(curl_easy_setopt(handle, CURLOPT_PROXYUSERNAME, gate->username.c_str()));
        check(curl_easy_setopt(handle, CURLOPT_PROXYPASSWORD, gate->password.c_str()));
        // An explicit proxy beats http_proxy/ALL_PROXY; an empty NOPROXY lets no host bypass it.
        check(curl_easy_setopt(handle, CURLOPT_NOPROXY, ""));
        guard = TorGuard{gate->port, &pool_->mutex, &pool_->log};
        check(curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION, &onOpenSocket));
        check(curl_easy_setopt(handle, CURLOPT_OPENSOCKETDATA, &guard));
    } else {
#ifdef _WIN32
        // Trust what Windows trusts, rather than a CA bundle shipped next to the exe.
        check(curl_easy_setopt(handle, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA)));
#elif defined(__linux__)
        if (const char* caBundle = systemCaBundle()) check(curl_easy_setopt(handle, CURLOPT_CAINFO, caBundle));
#endif
    }

    if (streams.upload != nullptr) {
        // CURLOPT_UPLOAD means PUT; any other method keeps its name.
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
    // "Name:" with no value removes a header libcurl would add on its own.
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
    // The header list dies with this call: don't leave the pooled handle pointing at it.
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, nullptr);
    if (result != CURLE_OK) {
        throw NetworkError(errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(result));
    }
    long status = 0;
    check(curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status));
    response.status = static_cast<int>(status);
    return response;
}

}  // namespace redvault::net
