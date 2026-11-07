#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "fake_transport.hpp"
#include "redvault/api/api_client.hpp"

using namespace redvault;
using namespace redvault::api;
using redvault::test::FakeTransport;

namespace {

struct Fixture {
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::shared_ptr<net::SessionCookieJar> cookies = std::make_shared<net::SessionCookieJar>();
    ApiClient api{"https://example.com/api", transport, cookies};
};

template <typename F>
ApiError apiErrorFrom(F&& call) {
    try {
        call();
    } catch (const ApiError& e) {
        return e;
    }
    FAIL("expected ApiError");
    throw std::logic_error("unreachable");
}

}  // namespace

TEST_CASE("resolves paths under the API base", "[api]") {
    Fixture f;
    CHECK(f.api.baseUrl() == "https://example.com/api/");
    CHECK(f.api.resolve("auth/me") == "https://example.com/api/auth/me");
    CHECK(f.api.resolve("/auth/me") == "https://example.com/api/auth/me");
    CHECK_THROWS_AS(ApiClient("not a url", f.transport, f.cookies), std::invalid_argument);
}

TEST_CASE("sends JSON with the session cookie and nothing identifying", "[api]") {
    Fixture f;
    f.transport->enqueue(200, R"({"mfaRequired":true})",
                         {{"set-cookie", "sid=half; Path=/; HttpOnly; Secure; SameSite=Lax"}});
    CHECK(f.api.login({.username = "alice", .password = "pw"}).mfaRequired);

    const auto& login = f.transport->last();
    CHECK(login.method == "POST");
    CHECK(login.url == "https://example.com/api/auth/login");
    CHECK(Json::parse(login.body) == Json{{"username", "alice"}, {"password", "pw"}});
    CHECK(FakeTransport::headerOf(login, "content-type") == "application/json");
    CHECK(FakeTransport::headerOf(login, "cookie").empty());
    for (const auto& [name, value] : login.headers) CHECK(name != "user-agent");

    // The half-session cookie from login rides along on the MFA verify.
    f.transport->enqueue(200, R"({"userId":"u1","role":"user","subscriptionTier":"free","publicKey":"pk",
        "encryptedPrivateKey":"e","privateKeyNonce":"n","privateKeyKdfSalt":"s"})");
    CHECK(f.api.verifyMfa({.code = "123456"}).userId == "u1");
    CHECK(FakeTransport::headerOf(f.transport->last(), "cookie") == "sid=half");
}

TEST_CASE("maps failures the way Android's apiCall does", "[api]") {
    Fixture f;

    f.transport->enqueue(401, R"({"error":"invalid_credentials","message":"Wrong handle or password."})");
    auto e = apiErrorFrom([&] { f.api.login({.username = "a", .password = "b"}); });
    CHECK(e.status() == 401);
    CHECK(e.code() == "invalid_credentials");
    CHECK(std::string(e.what()) == "Wrong handle or password.");

    f.transport->enqueue(502, "<html>Bad Gateway</html>");
    e = apiErrorFrom([&] { f.api.me(); });
    CHECK(e.status() == 502);
    CHECK(e.code() == "unknown_error");
    CHECK(std::string(e.what()) == "The server is restarting or unavailable. Try again in a minute.");

    f.transport->enqueue(200, R"({"unexpected":true})");
    e = apiErrorFrom([&] { f.api.me(); });
    CHECK(e.status() == 0);
    CHECK(e.code() == "malformed_response");

    f.transport->enqueue(200, "not json");
    e = apiErrorFrom([&] { f.api.listConversations(); });
    CHECK(e.code() == "malformed_response");

    f.transport->failWithNetworkError = true;
    e = apiErrorFrom([&] { f.api.logout(); });
    CHECK(e.status() == 0);
    CHECK(e.code() == "network_error");
}

TEST_CASE("void endpoints ignore the response body", "[api]") {
    Fixture f;
    f.transport->enqueue(204, "");
    f.api.logout();
    f.transport->enqueue(200, R"({"ok":true})");
    f.api.markRead("c1", {.status = "read"});
    CHECK(f.transport->last().url == "https://example.com/api/conversations/c1/read");
}

TEST_CASE("encodes ids and query values, and sends the odd bodies right", "[api]") {
    Fixture f;
    f.transport->handler = [](const net::HttpRequest&) { return net::HttpResponse{204, {}, ""}; };

    f.api.removeParticipant("c/1", "u 2");
    CHECK(f.transport->last().method == "DELETE");
    CHECK(f.transport->last().url == "https://example.com/api/conversations/c%2F1/participants/u%202");

    f.api.setDisappearing("c1", std::nullopt);
    CHECK(f.transport->last().body == R"({"seconds":null})");
    f.api.setDisappearing("c1", 3600);
    CHECK(f.transport->last().body == R"({"seconds":3600})");

    f.transport->handler = [](const net::HttpRequest&) {
        return net::HttpResponse{200, {}, R"({"userId":"u2","publicKey":"AA=="})"};
    };
    f.api.lookupUser("bob&x=1");
    CHECK(f.transport->last().url == "https://example.com/api/users/lookup?username=bob%26x%3D1");

    f.transport->handler = [](const net::HttpRequest&) {
        return net::HttpResponse{200, {}, R"({"objectId":"o1","status":"available"})"};
    };
    f.api.completeFileUpload("o1");
    CHECK(f.transport->last().body == "{}");
}

TEST_CASE("avatars are fetched only from the avatar route, with the session cookie", "[api][avatar]") {
    Fixture f;
    f.cookies->saveFromResponse(*net::parseUrl("https://example.com/api/auth/login"), {"sid=abc; Path=/"});
    f.transport->enqueue(200, std::string("\xFF\xD8\xFF", 3));
    const std::string url = "/api/users/3f2504e0-4f89-11d3-9a0c-0305e82c3301/avatar?v=123";
    CHECK(f.api.fetchAvatar(url) == std::string("\xFF\xD8\xFF", 3));
    CHECK(f.transport->last().url == "https://example.com" + url);
    CHECK(FakeTransport::headerOf(f.transport->last(), "cookie") == "sid=abc");

    const auto requestsBefore = f.transport->requests.size();
    CHECK_THROWS_AS(f.api.fetchAvatar("https://evil.example/x.png"), ApiError);
    CHECK_THROWS_AS(f.api.fetchAvatar("/api/users/../auth/me"), ApiError);
    CHECK_THROWS_AS(f.api.fetchAvatar("/api/users/%2e%2e/auth/me/avatar"), ApiError);
    CHECK_THROWS_AS(f.api.fetchAvatar(url + "&x=1"), ApiError);
    CHECK_THROWS_AS(f.api.fetchAvatar("/api/auth/me"), ApiError);
    CHECK(f.transport->requests.size() == requestsBefore);

    f.transport->enqueue(404, R"({"error":"not_found","message":"No avatar"})");
    CHECK_THROWS_AS(f.api.fetchAvatar("/api/users/3f2504e0-4f89-11d3-9a0c-0305e82c3302/avatar?v=1"), ApiError);

    f.transport->enqueue(200, R"({"avatarUrl":"/api/users/u1/avatar?v=456"})");
    CHECK(f.api.uploadAvatarImage("/9j/AA==") == "/api/users/u1/avatar?v=456");
    CHECK(f.transport->last().method == "PUT");
    CHECK(Json::parse(f.transport->last().body) == Json{{"image", "/9j/AA=="}});
}

TEST_CASE("logout's cleared cookie empties the jar", "[api]") {
    Fixture f;
    f.transport->enqueue(200, R"({"mfaRequired":true})", {{"set-cookie", "sid=abc; Path=/"}});
    f.api.login({.username = "a", .password = "b"});
    CHECK_FALSE(f.cookies->empty());

    f.transport->enqueue(204, "", {{"set-cookie", "sid=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT"}});
    f.api.logout();
    CHECK(f.cookies->empty());
}
