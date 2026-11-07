#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/net/http.hpp"

namespace redvault::net {

struct Cookie {
    std::string name;
    std::string value;
    std::string host;  // host-only: the server never sets Domain
    std::string path = "/";
    bool secure = false;
    /// Empty for a session cookie.
    std::optional<std::chrono::system_clock::time_point> expiresAt;
};

/**
 * Holds the server's session cookie (and the half-session cookie used
 * between login and MFA verify) in memory only, like the Android
 * SessionCookieJar. That matches the private key's lifetime: when the
 * process exits, both are gone together and the user signs in again.
 *
 * Deliberately small: host-only cookies, Path, Secure, Expires and Max-Age.
 * That covers everything Express's res.cookie() / clearCookie() emit.
 */
class SessionCookieJar {
public:
    using Clock = std::chrono::system_clock;

    void saveFromResponse(const Url& url, const std::vector<std::string>& setCookieHeaders,
                          Clock::time_point now = Clock::now());

    /// The Cookie header value for a request to `url`, or empty if nothing applies.
    std::string headerFor(const Url& url, Clock::time_point now = Clock::now());

    void clear();

    bool empty() const;

private:
    mutable std::mutex mutex_;
    std::vector<Cookie> cookies_;
};

/// Parses one Set-Cookie header as sent to `url`. Empty if it's malformed.
std::optional<Cookie> parseSetCookie(std::string_view header, const Url& url,
                                     std::chrono::system_clock::time_point now);

/// Parses an IMF-fixdate ("Thu, 01 Jan 1970 00:00:00 GMT"), the only date format Express emits.
std::optional<std::chrono::system_clock::time_point> parseHttpDate(std::string_view text);

}  // namespace redvault::net
