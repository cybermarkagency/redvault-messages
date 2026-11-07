#include "redvault/net/http.hpp"

#include <algorithm>
#include <charconv>

#include "redvault/net/curl_transport.hpp"

namespace redvault::net {

std::optional<std::string> HttpResponse::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (key == name) return value;
    }
    return std::nullopt;
}

std::vector<std::string> HttpResponse::headersNamed(std::string_view name) const {
    std::vector<std::string> out;
    for (const auto& [key, value] : headers) {
        if (key == name) out.push_back(value);
    }
    return out;
}

HttpResponse HttpTransport::sendStreaming(const HttpRequest& request, const StreamOptions& streams) {
    HttpRequest buffered = request;
    if (streams.upload != nullptr) {
        buffered.body.assign(static_cast<std::size_t>(streams.uploadSize), '\0');
        streams.upload->read(buffered.body.data(), static_cast<std::streamsize>(buffered.body.size()));
        if (static_cast<std::uint64_t>(streams.upload->gcount()) != streams.uploadSize) {
            throw NetworkError("Upload body ended early");
        }
    }
    HttpResponse response = send(buffered);
    if (streams.download != nullptr && response.status >= 200 && response.status <= 299) {
        streams.download->write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        response.body.clear();
    }
    return response;
}

std::string toLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return out;
}

std::optional<Url> parseUrl(std::string_view url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) return std::nullopt;
    Url out;
    out.scheme = toLower(url.substr(0, schemeEnd));
    if (out.scheme != "http" && out.scheme != "https") return std::nullopt;

    std::string_view rest = url.substr(schemeEnd + 3);
    const auto authorityEnd = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, authorityEnd);
    // Credentials in the URL are never legitimate for this client.
    if (authority.empty() || authority.find('@') != std::string_view::npos) return std::nullopt;

    std::string_view host = authority;
    if (host.front() == '[') {
        const auto close = host.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        host = host.substr(0, close + 1);
    } else if (const auto colon = host.rfind(':'); colon != std::string_view::npos) {
        host = host.substr(0, colon);
    }
    if (host.empty()) return std::nullopt;
    out.host = toLower(host);

    std::string_view path = authorityEnd == std::string_view::npos ? std::string_view{} : rest.substr(authorityEnd);
    path = path.substr(0, path.find_first_of("?#"));
    out.path = path.empty() ? "/" : std::string(path);
    return out;
}

bool isLoopbackHost(std::string_view host) {
    if (host == "localhost" || host == "[::1]") return true;
    // 127.0.0.0/8, written out in full: "127.0.0.1.example.com" and "127.1" don't count.
    if (!host.starts_with("127.")) return false;
    int parts = 0;
    for (std::string_view rest = host;;) {
        const auto dot = rest.find('.');
        const std::string_view part = rest.substr(0, dot);
        int value = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || part.size() > 3 || error != std::errc{} || end != part.data() + part.size() || value > 255) return false;
        ++parts;
        if (dot == std::string_view::npos) break;
        rest.remove_prefix(dot + 1);
    }
    return parts == 4;
}

bool isAllowedServerUrl(std::string_view url, bool allowPlainHttp) {
    const auto parsed = parseUrl(url);
    if (!parsed) return false;
    if (parsed->scheme == "https") return true;
    return allowPlainHttp || isLoopbackHost(parsed->host) || isOnionHost(parsed->host);
}

bool isHttpsUrl(std::string_view url) {
    const auto parsed = parseUrl(url);
    return parsed && parsed->scheme == "https";
}

bool isCheckoutUrl(std::string_view url) {
    const auto parsed = parseUrl(url);
    return parsed && parsed->scheme == "https" && (parsed->host == "buy.stripe.com" || parsed->host == "checkout.stripe.com");
}

std::string percentEncode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0f]);
        }
    }
    return out;
}

}  // namespace redvault::net
