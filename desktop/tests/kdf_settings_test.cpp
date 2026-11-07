// Key-stretching settings (M-1): what a server names is checked before anything is derived or sent.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <memory>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/recovery_key.hpp"
#include "redvault/data/account_repository.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/sign_in.hpp"
#include "redvault/net/cookie_jar.hpp"

using namespace redvault;
using Json = nlohmann::json;
using crypto::base64Encode;
using redvault::test::FakeTransport;

namespace {

constexpr std::int64_t kMiB = 1024 * 1024;
constexpr const char* kWrong = "This account's sign-in details look wrong. Your password wasn't sent.";
constexpr const char* kHandle = "kdf-tester";

struct Fixture {
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::shared_ptr<api::ApiClient> api =
        std::make_shared<api::ApiClient>("https://example.com/api/", transport, std::make_shared<net::SessionCookieJar>());
    std::vector<net::HttpRequest> sent;
    Json prelogin = Json{{"kdfSalt", base64Encode(crypto::Bytes(16, 7))}, {"authVersion", 2}, {"upgrade", false}};

    Fixture() {
        data::setSignInPins({[](const std::string&) { return false; }, [](const std::string&) {}});
        transport->handler = [this](const net::HttpRequest& r) -> net::HttpResponse {
            sent.push_back(r);
            if (r.url.ends_with("auth/prelogin")) return {200, {}, prelogin.dump()};
            return {200, {}, "{}"};
        };
    }

    int count(std::string_view path) const {
        int n = 0;
        for (const auto& r : sent) n += r.url.ends_with(path) ? 1 : 0;
        return n;
    }

    Json body(std::string_view path) const {
        for (const auto& r : sent) {
            if (r.url.ends_with(path)) return Json::parse(r.body);
        }
        return {};
    }
};

}  // namespace

TEST_CASE("the default stays at the interactive values", "[kdf]") {
    CHECK(crypto::kKdfCurrent == crypto::KdfSettings{2, 64 * kMiB});
    CHECK(crypto::kKdfInteractive == crypto::KdfSettings{2, 64 * kMiB});
    CHECK(crypto::kKdfMax == crypto::KdfSettings{10, 1024 * kMiB});
}

TEST_CASE("the floor and the ceiling are inclusive", "[kdf]") {
    CHECK_NOTHROW(crypto::requireSafeKdf({2, 64 * kMiB}));
    CHECK_NOTHROW(crypto::requireSafeKdf({10, 1024 * kMiB}));
    CHECK_THROWS_AS(crypto::requireSafeKdf({1, 64 * kMiB}), crypto::UnsafeKdfError);
    CHECK_THROWS_AS(crypto::requireSafeKdf({2, 64 * kMiB - 1}), crypto::UnsafeKdfError);
    CHECK_THROWS_AS(crypto::requireSafeKdf({11, 64 * kMiB}), crypto::UnsafeKdfError);
    CHECK_THROWS_AS(crypto::requireSafeKdf({2, 1024 * kMiB + 1}), crypto::UnsafeKdfError);
    CHECK_THROWS_AS(crypto::requireSafeKdf({0, 0}), crypto::UnsafeKdfError);
    CHECK_THROWS_AS(crypto::requireSafeKdf({-2, 64 * kMiB}), crypto::UnsafeKdfError);
}

TEST_CASE("a direct derivation is refused before anything is derived", "[kdf]") {
    // A salt of the wrong length would fail too, but later: the settings come first.
    CHECK_THROWS_AS(crypto::deriveWrapKey("password", crypto::Bytes(3), {1, 8 * kMiB}), crypto::UnsafeKdfError);
    CHECK_THROWS_AS(crypto::deriveSignInKey("password", crypto::Bytes(16), {1, 8 * kMiB}), crypto::UnsafeKdfError);
}

TEST_CASE("settings out of range from the server are refused, and nothing is sent", "[kdf]") {
    const auto [ops, mem] = GENERATE(table<std::int64_t, std::int64_t>({
        {1, 64 * kMiB},          // too few passes
        {2, 8 * kMiB},           // too little memory
        {0, 0},                  // nothing at all
        {11, 64 * kMiB},         // would hang the app
        {2, 2048 * kMiB},        // would hang the app
        {-1, 64 * kMiB},
    }));
    CAPTURE(ops, mem);
    Fixture f;
    f.prelogin["kdfOps"] = ops;
    f.prelogin["kdfMem"] = mem;
    data::AuthRepository auth(f.api);
    CHECK_THROWS_MATCHES(auth.login(kHandle, "password"), api::UserFacingError, Catch::Matchers::Message(kWrong));
    CHECK(f.count("auth/login") == 0);
}

TEST_CASE("the same refusal when the account would have sent the password", "[kdf]") {
    Fixture f;
    f.prelogin["authVersion"] = 1;
    f.prelogin["kdfOps"] = 1;
    f.prelogin["kdfMem"] = 8 * kMiB;
    data::AuthRepository auth(f.api);
    CHECK_THROWS_MATCHES(auth.login(kHandle, "password"), api::UserFacingError, Catch::Matchers::Message(kWrong));
    CHECK(f.count("auth/login") == 0);
}

TEST_CASE("something that isn't a plain integer can't be used", "[kdf]") {
    Fixture f;
    f.prelogin["kdfOps"] = "lots";
    f.prelogin["kdfMem"] = 64 * kMiB;
    data::AuthRepository auth(f.api);
    CHECK_THROWS_MATCHES(auth.login(kHandle, "password"), api::UserFacingError, Catch::Matchers::Message(kWrong));
    CHECK(f.count("auth/login") == 0);
}

TEST_CASE("control: a stronger setting from the server is used", "[kdf]") {
    Fixture f;
    f.prelogin["kdfOps"] = 3;
    f.prelogin["kdfMem"] = 128 * kMiB;
    const auto salt = crypto::Bytes(16, 7);
    const auto credential = data::credentialFor(*f.api, kHandle, "password");
    CHECK(credential.password == crypto::deriveSignInKey("password", salt, {3, 128 * kMiB}));
    CHECK(credential.password != crypto::deriveSignInKey("password", salt));
}

TEST_CASE("control: an older server naming no settings means the interactive values", "[kdf]") {
    Fixture f;
    const auto credential = data::credentialFor(*f.api, kHandle, "password");
    CHECK(credential.password == crypto::deriveSignInKey("password", crypto::Bytes(16, 7), crypto::kKdfInteractive));
}

TEST_CASE("only a pair counts as naming settings", "[kdf]") {
    CHECK(data::kdfSettings(std::nullopt, std::nullopt) == crypto::kKdfInteractive);
    CHECK(data::kdfSettings(3, std::nullopt) == crypto::kKdfInteractive);
    CHECK(data::kdfSettings(std::nullopt, 128 * kMiB) == crypto::kKdfInteractive);
    CHECK(data::kdfSettings(3, 128 * kMiB) == crypto::KdfSettings{3, 128 * kMiB});
}

TEST_CASE("a new credential uses the current settings, whatever the server says", "[kdf]") {
    Fixture f;
    f.prelogin["kdfOps"] = 1;
    f.prelogin["kdfMem"] = 8 * kMiB;
    const auto salt = crypto::Bytes(16, 9);
    const auto credential = data::newCredential(*f.api, kHandle, "password", salt);
    CHECK(credential.password == crypto::deriveSignInKey("password", salt, crypto::kKdfCurrent));
}

TEST_CASE("a new wrap records its settings and opens with them", "[kdf]") {
    const auto keys = crypto::generateKeyPair();
    const auto wrapped = crypto::wrapPrivateKey(keys.privateKey, "password");
    REQUIRE(wrapped.kdf);
    CHECK(*wrapped.kdf == crypto::kKdfCurrent);
    CHECK(crypto::unwrapPrivateKey(wrapped, "password").size() == keys.privateKey.size());
}

TEST_CASE("a wrapped key with no settings opens at the interactive values", "[kdf]") {
    const auto keys = crypto::generateKeyPair();
    auto wrapped = crypto::wrapPrivateKey(keys.privateKey, "password");
    wrapped.kdf.reset();
    CHECK(crypto::unwrapPrivateKey(wrapped, "password").size() == keys.privateKey.size());
}

TEST_CASE("the login result's settings are read and checked before unwrapping", "[kdf]") {
    Fixture f;
    const auto keys = crypto::generateKeyPair();
    const auto wrapped = crypto::wrapPrivateKey(keys.privateKey, "password");
    Json login{{"userId", "ABCDEF11-1111-4111-8111-111111111111"},
               {"acceptMessageRequests", true},
               {"transparencyMode", false},
               {"expiresAt", "2099-01-01T00:00:00.000Z"},
               {"role", "user"},
               {"subscriptionTier", "free"},
               {"publicKey", base64Encode(keys.publicKey)},
               {"encryptedPrivateKey", base64Encode(wrapped.encryptedPrivateKey)},
               {"privateKeyNonce", base64Encode(wrapped.nonce)},
               {"privateKeyKdfSalt", base64Encode(wrapped.kdfSalt)}};
    bool weak = false;
    f.transport->handler = [&](const net::HttpRequest& r) -> net::HttpResponse {
        f.sent.push_back(r);
        if (r.url.ends_with("auth/prelogin")) return {200, {}, f.prelogin.dump()};
        Json answer = login;
        if (weak) {
            answer["privateKeyKdfOps"] = 1;
            answer["privateKeyKdfMem"] = 8 * kMiB;
        } else {
            answer["privateKeyKdfOps"] = crypto::kKdfCurrent.ops;
            answer["privateKeyKdfMem"] = crypto::kKdfCurrent.mem;
        }
        return {200, {}, answer.dump()};
    };
    data::AuthRepository auth(f.api);
    SECTION("control") {
        const auto outcome = auth.login(kHandle, "password");
        const auto* identity = std::get_if<data::SelfIdentity>(&outcome);
        REQUIRE(identity != nullptr);
        CHECK(identity->wrappedKey->kdf == crypto::kKdfCurrent);
        // Ids compare lowercased everywhere, starting with who this is.
        CHECK(identity->userId == "abcdef11-1111-4111-8111-111111111111");
    }
    SECTION("refused: and it isn't reported as a wrong password") {
        weak = true;
        CHECK_THROWS_MATCHES(auth.login(kHandle, "password"), api::UserFacingError, Catch::Matchers::Message(kWrong));
    }
}

TEST_CASE("the settings are on the wire in every request that sets a password", "[kdf]") {
    Fixture f;
    const auto keys = crypto::generateKeyPair();
    SECTION("register") {
        f.transport->handler = [&](const net::HttpRequest& r) -> net::HttpResponse {
            f.sent.push_back(r);
            if (r.url.ends_with("auth/prelogin")) return {200, {}, f.prelogin.dump()};
            return {201, {}, Json{{"userId", "u1"}, {"acceptMessageRequests", true}, {"transparencyMode", false}, {"expiresAt", "x"}, {"role", "user"}, {"subscriptionTier", "free"}}.dump()};
        };
        data::AuthRepository auth(f.api);
        auth.registerAccount(kHandle, "password", {true, true, true});
        const Json body = f.body("auth/register");
        CHECK(body["kdfOps"] == crypto::kKdfCurrent.ops);
        CHECK(body["kdfMem"] == crypto::kKdfCurrent.mem);
    }
    SECTION("password change") {
        data::AccountRepository account(f.api);
        data::SelfIdentity identity{.handle = kHandle, .userId = "u1", .publicKey = keys.publicKey,
                                    .privateKey = crypto::SecretBytes::copyOf(keys.privateKey)};
        account.changePassword(identity, "old password", "new password");
        const Json body = f.body("auth/password");
        REQUIRE(body.is_object());
        CHECK(body["kdfOps"] == crypto::kKdfCurrent.ops);
        CHECK(body["kdfMem"] == crypto::kKdfCurrent.mem);
    }
    SECTION("recovery") {
        const auto key = crypto::recovery::generate();
        const auto material = crypto::recovery::materialFor(key, keys.privateKey);
        const auto wrappedPublic = base64Encode(keys.publicKey);
        f.transport->handler = [&](const net::HttpRequest& r) -> net::HttpResponse {
            f.sent.push_back(r);
            if (r.url.ends_with("auth/prelogin")) return {200, {}, f.prelogin.dump()};
            if (r.url.ends_with("auth/recovery/start")) {
                return {200, {}, Json{{"publicKey", wrappedPublic}, {"recoveryEncryptedPrivateKey", material.recoveryEncryptedPrivateKey},
                                      {"recoveryPrivateKeyNonce", material.recoveryPrivateKeyNonce}, {"mfaRequired", false}}.dump()};
            }
            const Json in = Json::parse(r.body);
            return {200, {}, Json{{"userId", "u1"}, {"role", "user"}, {"subscriptionTier", "free"}, {"publicKey", wrappedPublic},
                                  {"encryptedPrivateKey", in["encryptedPrivateKey"]}, {"privateKeyNonce", in["privateKeyNonce"]},
                                  {"privateKeyKdfSalt", in["privateKeyKdfSalt"]}}.dump()};
        };
        data::AuthRepository auth(f.api);
        auth.recover(kHandle, crypto::recovery::format(key), "new password", std::nullopt);
        const Json body = f.body("auth/recovery/complete");
        CHECK(body["kdfOps"] == crypto::kKdfCurrent.ops);
        CHECK(body["kdfMem"] == crypto::kKdfCurrent.mem);
    }
}

TEST_CASE("prelogin answers parse with and without settings", "[kdf]") {
    const auto with = Json::parse(R"({"kdfSalt":"AAAA","authVersion":2,"upgrade":false,"kdfOps":3,"kdfMem":134217728})").get<api::PreLoginResult>();
    CHECK(with.kdfOps == 3);
    CHECK(with.kdfMem == 134217728);
    const auto without = Json::parse(R"({"kdfSalt":"AAAA","authVersion":1,"upgrade":false})").get<api::PreLoginResult>();
    CHECK_FALSE(without.kdfOps);
    CHECK_FALSE(without.kdfMem);
    const auto nulls = Json::parse(R"({"kdfSalt":"AAAA","kdfOps":null,"kdfMem":null})").get<api::PreLoginResult>();
    CHECK_FALSE(nulls.kdfOps);
}
