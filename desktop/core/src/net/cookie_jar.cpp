#include "redvault/net/cookie_jar.hpp"

#include <algorithm>
#include <array>
#include <charconv>

namespace redvault::net {

namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && toLower(a) == toLower(b);
}

template <typename T>
std::optional<T> parseNumber(std::string_view text) {
    T value{};
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return value;
}

/// RFC 6265 5.1.4: the directory of the request path, used when Set-Cookie has no Path.
std::string defaultPath(const std::string& requestPath) {
    const auto slash = requestPath.rfind('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return requestPath.substr(0, slash);
}

/// RFC 6265 5.1.4 path-match.
bool pathMatches(std::string_view requestPath, std::string_view cookiePath) {
    if (requestPath == cookiePath) return true;
    if (!requestPath.starts_with(cookiePath)) return false;
    return cookiePath.ends_with('/') || requestPath[cookiePath.size()] == '/';
}

bool isExpired(const Cookie& cookie, std::chrono::system_clock::time_point now) {
    return cookie.expiresAt && *cookie.expiresAt <= now;
}

}  // namespace

std::optional<std::chrono::system_clock::time_point> parseHttpDate(std::string_view text) {
    // "Thu, 01 Jan 1970 00:00:00 GMT"
    text = trim(text);
    if (text.size() != 29 || text[3] != ',' || !text.ends_with(" GMT")) return std::nullopt;
    static constexpr std::array<std::string_view, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto monthIt = std::find(kMonths.begin(), kMonths.end(), text.substr(8, 3));
    if (monthIt == kMonths.end()) return std::nullopt;
    if (text[4] != ' ' || text[7] != ' ' || text[11] != ' ' || text[16] != ' ' || text[19] != ':' ||
        text[22] != ':') {
        return std::nullopt;
    }
    const auto day = parseNumber<unsigned>(text.substr(5, 2));
    const auto year = parseNumber<int>(text.substr(12, 4));
    const auto hour = parseNumber<int>(text.substr(17, 2));
    const auto minute = parseNumber<int>(text.substr(20, 2));
    const auto second = parseNumber<int>(text.substr(23, 2));
    if (!day || !year || !hour || !minute || !second || *hour < 0 || *hour > 23 || *minute < 0 || *minute > 59 ||
        *second < 0 || *second > 60 || *year < 0) {
        return std::nullopt;
    }
    using namespace std::chrono;
    // system_clock may count nanoseconds, which overflow after 2262: a later date means "never expires" anyway.
    const int clampedYear = std::clamp(*year, 1970, 2200);
    const year_month_day date{std::chrono::year{clampedYear},
                              month{static_cast<unsigned>(monthIt - kMonths.begin() + 1)}, std::chrono::day{*day}};
    if (!date.ok()) return std::nullopt;
    return sys_days{date} + hours{*hour} + minutes{*minute} + seconds{*second};
}

std::optional<Cookie> parseSetCookie(std::string_view header, const Url& url,
                                     std::chrono::system_clock::time_point now) {
    const auto firstSemicolon = header.find(';');
    const std::string_view pair = header.substr(0, firstSemicolon);
    const auto equals = pair.find('=');
    if (equals == std::string_view::npos) return std::nullopt;

    Cookie cookie;
    cookie.name = std::string(trim(pair.substr(0, equals)));
    cookie.value = std::string(trim(pair.substr(equals + 1)));
    if (cookie.name.empty()) return std::nullopt;
    cookie.host = url.host;
    cookie.path = defaultPath(url.path);

    std::optional<std::chrono::system_clock::time_point> expires;
    std::optional<std::chrono::system_clock::time_point> maxAge;
    std::string_view attributes = firstSemicolon == std::string_view::npos ? std::string_view{}
                                                                           : header.substr(firstSemicolon + 1);
    while (!attributes.empty()) {
        const auto next = attributes.find(';');
        const std::string_view attribute = trim(attributes.substr(0, next));
        attributes = next == std::string_view::npos ? std::string_view{} : attributes.substr(next + 1);

        const auto attrEquals = attribute.find('=');
        const std::string_view key = trim(attribute.substr(0, attrEquals));
        const std::string_view value =
            attrEquals == std::string_view::npos ? std::string_view{} : trim(attribute.substr(attrEquals + 1));

        if (equalsIgnoreCase(key, "path")) {
            if (value.starts_with('/')) cookie.path = std::string(value);
        } else if (equalsIgnoreCase(key, "secure")) {
            cookie.secure = true;
        } else if (equalsIgnoreCase(key, "expires")) {
            if (const auto date = parseHttpDate(value)) expires = date;
        } else if (equalsIgnoreCase(key, "max-age")) {
            if (const auto seconds = parseNumber<long long>(value)) {
                // Capped at 100 years (RFC 6265 allows any size): a bigger count overflows the clock's
                // nanoseconds (found by fuzzing, UBSan) and lasts longer than any session anyway.
                constexpr long long kMaxSeconds = 100LL * 365 * 24 * 60 * 60;
                maxAge = *seconds <= 0 ? std::chrono::system_clock::time_point::min()
                                       : now + std::chrono::seconds{std::min(*seconds, kMaxSeconds)};
            }
        }
        // Domain is ignored (host-only is stricter); HttpOnly and SameSite don't apply outside a browser.
    }
    // RFC 6265 5.3: Max-Age wins over Expires.
    cookie.expiresAt = maxAge ? maxAge : expires;
    return cookie;
}

void SessionCookieJar::saveFromResponse(const Url& url, const std::vector<std::string>& setCookieHeaders,
                                        Clock::time_point now) {
    std::lock_guard lock(mutex_);
    for (const auto& header : setCookieHeaders) {
        auto cookie = parseSetCookie(header, url, now);
        if (!cookie) continue;
        // A Secure cookie arriving over plain http is refused, as browsers do.
        if (cookie->secure && url.scheme != "https") continue;
        std::erase_if(cookies_, [&](const Cookie& existing) {
            return existing.name == cookie->name && existing.host == cookie->host && existing.path == cookie->path;
        });
        // An already-expired cookie (Express's clearCookie) just deletes.
        if (!isExpired(*cookie, now)) cookies_.push_back(std::move(*cookie));
    }
}

std::string SessionCookieJar::headerFor(const Url& url, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    std::erase_if(cookies_, [&](const Cookie& cookie) { return isExpired(cookie, now); });
    std::string out;
    for (const auto& cookie : cookies_) {
        if (cookie.host != url.host || !pathMatches(url.path, cookie.path)) continue;
        if (cookie.secure && url.scheme != "https") continue;
        if (!out.empty()) out += "; ";
        out += cookie.name;
        out += '=';
        out += cookie.value;
    }
    return out;
}

void SessionCookieJar::clear() {
    std::lock_guard lock(mutex_);
    cookies_.clear();
}

bool SessionCookieJar::empty() const {
    std::lock_guard lock(mutex_);
    return cookies_.empty();
}

}  // namespace redvault::net
