#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "redvault/net/cookie_jar.hpp"
#include "redvault/net/http.hpp"

using namespace redvault::net;
using namespace std::chrono;

namespace {

Url url(const char* text) {
    auto parsed = parseUrl(text);
    REQUIRE(parsed);
    return *parsed;
}

const system_clock::time_point kNow = sys_days{year{2026} / 9 / 25} + hours{12};

}  // namespace

TEST_CASE("parses absolute http(s) URLs", "[net]") {
    const auto parsed = url("HTTPS://Example.COM:8443/api/auth/login?x=1#frag");
    CHECK(parsed.scheme == "https");
    CHECK(parsed.host == "example.com");
    CHECK(parsed.path == "/api/auth/login");

    CHECK(url("http://localhost").path == "/");
    CHECK(url("http://[::1]:3000/api/").host == "[::1]");

    CHECK_FALSE(parseUrl("ftp://example.com/"));
    CHECK_FALSE(parseUrl("/api/auth"));
    CHECK_FALSE(parseUrl("https://user:pw@example.com/"));
    CHECK_FALSE(parseUrl("https:///nohost"));
}

TEST_CASE("percent-encodes everything but unreserved characters", "[net]") {
    CHECK(percentEncode("abc-XYZ_0.9~") == "abc-XYZ_0.9~");
    CHECK(percentEncode("a b/c?d&e") == "a%20b%2Fc%3Fd%26e");
    CHECK(percentEncode("\xC3\xA9") == "%C3%A9");
}

TEST_CASE("response header lookup uses lowercase names", "[net]") {
    HttpResponse response{200, {{"set-cookie", "a=1"}, {"content-type", "x"}, {"set-cookie", "b=2"}}, ""};
    CHECK(response.header("content-type") == "x");
    CHECK_FALSE(response.header("missing"));
    CHECK(response.headersNamed("set-cookie") == std::vector<std::string>{"a=1", "b=2"});
}

TEST_CASE("parses Express's cookie dates", "[net][cookies]") {
    CHECK(parseHttpDate("Thu, 01 Jan 1970 00:00:00 GMT") == system_clock::time_point{});
    CHECK(parseHttpDate("Fri, 25 Sep 2026 12:00:00 GMT") == kNow);
    CHECK_FALSE(parseHttpDate("Fri, 31 Feb 2026 12:00:00 GMT"));
    CHECK_FALSE(parseHttpDate("2026-09-25T12:00:00Z"));
}

TEST_CASE("far-off cookie dates don't overflow the clock", "[net][cookies]") {
    // Found by fuzzing (UBSan): years past 2262 overflowed a nanosecond time_point.
    const auto far = parseHttpDate("Fri, 31 Dec 9999 23:59:59 GMT");
    REQUIRE(far);
    CHECK(*far > kNow);
    const auto early = parseHttpDate("Sat, 01 Jan 0001 00:00:00 GMT");
    REQUIRE(early);
    CHECK(*early <= system_clock::time_point{} + hours(24));
    CHECK_FALSE(parseHttpDate("Fri, 25 Sep 2026 -1:00:00 GMT"));

    // The same through Max-Age: a huge count is capped instead of overflowing.
    const auto cookie = parseSetCookie("sid=abc; Max-Age=13333333333333", url("https://example.com/api/"), kNow);
    REQUIRE(cookie);
    REQUIRE(cookie->expiresAt);
    CHECK(*cookie->expiresAt > kNow + hours(24 * 365 * 99));
}

TEST_CASE("session cookie round trip, as Express sets and clears it", "[net][cookies]") {
    SessionCookieJar jar;
    const auto login = url("https://example.com/api/auth/login");
    jar.saveFromResponse(login,
                         {"sid=abc123; Path=/; Expires=Sat, 26 Sep 2026 12:00:00 GMT; HttpOnly; Secure; SameSite=Lax"},
                         kNow);

    CHECK(jar.headerFor(url("https://example.com/api/conversations"), kNow) == "sid=abc123");
    // Host-only: never sent to another host, even a subdomain.
    CHECK(jar.headerFor(url("https://evil.example.com/api/"), kNow).empty());
    // Secure: never over plain http.
    CHECK(jar.headerFor(url("http://example.com/api/"), kNow).empty());
    // Expired: dropped.
    CHECK(jar.headerFor(url("https://example.com/api/"), kNow + hours{25}).empty());
    CHECK(jar.empty());

    jar.saveFromResponse(login, {"sid=abc123; Path=/; Max-Age=60"}, kNow);
    CHECK_FALSE(jar.empty());
    // res.clearCookie() sends an Expires in 1970, which deletes it.
    jar.saveFromResponse(url("https://example.com/api/auth/logout"),
                         {"sid=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT"}, kNow);
    CHECK(jar.empty());
}

TEST_CASE("cookie jar follows Path, Max-Age precedence and replacement", "[net][cookies]") {
    SessionCookieJar jar;
    const auto base = url("http://localhost/api/auth/login");

    // Session cookie with no Path defaults to the request's directory.
    jar.saveFromResponse(base, {"half=1"}, kNow);
    CHECK(jar.headerFor(url("http://localhost/api/auth/mfa/verify"), kNow) == "half=1");
    CHECK(jar.headerFor(url("http://localhost/api/conversations"), kNow).empty());
    CHECK(jar.headerFor(url("http://localhost/api/authority"), kNow).empty());

    // Same name/host/path replaces; Max-Age beats Expires.
    jar.saveFromResponse(base, {"half=2; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=30"}, kNow);
    CHECK(jar.headerFor(url("http://localhost/api/auth/x"), kNow) == "half=2");
    CHECK(jar.headerFor(url("http://localhost/api/auth/x"), kNow + seconds{31}).empty());

    // A Secure cookie over plain http is refused outright.
    jar.saveFromResponse(base, {"s=1; Path=/; Secure"}, kNow);
    CHECK(jar.empty());

    // Malformed lines are skipped without disturbing the rest.
    jar.saveFromResponse(base, {"no-equals-sign", "=empty-name", "ok=1; Path=/"}, kNow);
    CHECK(jar.headerFor(url("http://localhost/"), kNow) == "ok=1");

    jar.clear();
    CHECK(jar.empty());
}
