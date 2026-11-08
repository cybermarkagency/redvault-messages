// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Drives AuthRepository against a tiny in-memory stand-in for the server's
// auth routes, with real crypto, so register → login → recover round-trips
// the same key material the server would store.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>

#include "fake_transport.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/recovery_key.hpp"
#include "redvault/data/auth_repository.hpp"
#include "test_helpers.hpp"

using namespace redvault;
using namespace redvault::data;
using api::Json;
using redvault::test::copy;
using redvault::test::FakeTransport;

namespace {

const SignupConsent kAllTicked{.termsAccepted = true, .privacyAcknowledged = true, .age18Confirmed = true};

/// What the server keeps per account: only wrapped keys and derived tokens.
struct FakeAuthServer {
    Json account;  // the RegisterRequest body
    bool mfaEnabled = false;
    /// What a forged sign-in answer gives as this account's public key.
    std::optional<std::string> forgedPublicKey;
    std::string mfaCode = "123456";
    std::string passwordHashStandIn;
    /// Answers as an account on sign-in keys.
    bool signInKeys = false;
    std::string passwordSeen;

    Json loginBody() const {
        return Json{{"userId", "u1"},
                    {"acceptMessageRequests", true},
                    {"transparencyMode", false},
                    {"avatarUrl", nullptr},
                    {"expiresAt", "2026-10-25T00:00:00.000Z"},
                    {"role", "user"},
                    {"subscriptionTier", "free"},
                    {"publicKey", forgedPublicKey ? Json(*forgedPublicKey) : account["publicKey"]},
                    {"encryptedPrivateKey", account["encryptedPrivateKey"]},
                    {"privateKeyNonce", account["privateKeyNonce"]},
                    {"privateKeyKdfSalt", account["privateKeyKdfSalt"]}};
    }

    static net::HttpResponse json(int status, const Json& body) {
        return net::HttpResponse{status, {}, body.dump()};
    }

    static net::HttpResponse error(int status, const char* code) {
        return json(status, Json{{"error", code}, {"message", code}});
    }

    net::HttpResponse handle(const net::HttpRequest& request) {
        const auto path = request.url.substr(std::string("https://example.com/api/").size());
        const Json body = request.body.empty() ? Json() : Json::parse(request.body);
        if (path == "auth/login") passwordSeen = body.value("password", "");

        if (path == "auth/register") {
            // Like the server: no account unless all three sign-up boxes were ticked.
            if (body.value("terms_accepted", false) != true || body.value("privacy_acknowledged", false) != true ||
                body.value("age_18_confirmed", false) != true) {
                return error(400, "consent_required");
            }
            account = body;
            passwordHashStandIn = body["password"];
            return json(201, Json{{"userId", "u1"},
                                  {"acceptMessageRequests", true},
                                  {"transparencyMode", false},
                                  {"expiresAt", "2026-10-25T00:00:00.000Z"},
                                  {"role", "user"},
                                  {"subscriptionTier", "free"}});
        }
        if (path == "auth/prelogin") {
            return json(200, Json{{"kdfSalt", account.is_object() ? account.value("privateKeyKdfSalt", "AAAAAAAAAAAAAAAAAAAAAA==") : "AAAAAAAAAAAAAAAAAAAAAA=="}, {"authVersion", signInKeys ? 2 : 1}, {"upgrade", false}});
        }
        if (path == "auth/login") {
            if (body["username"] != account["username"] || body["password"] != passwordHashStandIn) {
                return error(401, "invalid_credentials");
            }
            if (mfaEnabled) return json(200, Json{{"mfaRequired", true}});
            return json(200, loginBody());
        }
        if (path == "auth/mfa/verify") {
            if (body["code"] != mfaCode) return error(401, "invalid_code");
            return json(200, loginBody());
        }
        if (path == "auth/recovery/start") {
            const auto& recovery = account["recovery"];
            if (body["recoveryAuth"] != recovery["recoveryAuth"]) return error(401, "invalid_recovery");
            return json(200, Json{{"publicKey", account["publicKey"]},
                                  {"recoveryEncryptedPrivateKey", recovery["recoveryEncryptedPrivateKey"]},
                                  {"recoveryPrivateKeyNonce", recovery["recoveryPrivateKeyNonce"]},
                                  {"mfaRequired", mfaEnabled}});
        }
        if (path == "auth/recovery/complete") {
            if (mfaEnabled && body.value("code", "") != mfaCode) return error(401, "invalid_code");
            passwordHashStandIn = body["newPassword"];
            for (const char* key : {"encryptedPrivateKey", "privateKeyNonce", "privateKeyKdfSalt"}) {
                account[key] = body[key];
            }
            return json(200, loginBody());
        }
        if (path == "auth/logout") return net::HttpResponse{204, {}, ""};
        return error(404, "not_found");
    }
};

struct Fixture {
    FakeAuthServer server;
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::shared_ptr<net::SessionCookieJar> cookies = std::make_shared<net::SessionCookieJar>();
    std::shared_ptr<api::ApiClient> api =
        std::make_shared<api::ApiClient>("https://example.com/api/", transport, cookies);
    AuthRepository auth{api};

    Fixture() {
        transport->handler = [this](const net::HttpRequest& request) { return server.handle(request); };
    }
};

}  // namespace

TEST_CASE("sign-up sends the three boxes as ticked, and the server refuses it without them", "[auth]") {
    Fixture f;
    f.auth.registerAccount("alice", "correct horse", kAllTicked);
    CHECK(f.server.account["terms_accepted"] == true);
    CHECK(f.server.account["privacy_acknowledged"] == true);
    CHECK(f.server.account["age_18_confirmed"] == true);

    Fixture g;
    for (const auto& consent : {SignupConsent{.termsAccepted = false, .privacyAcknowledged = true, .age18Confirmed = true},
                                SignupConsent{.termsAccepted = true, .privacyAcknowledged = false, .age18Confirmed = true},
                                SignupConsent{.termsAccepted = true, .privacyAcknowledged = true, .age18Confirmed = false}}) {
        CHECK_FALSE(consent.complete());
        try {
            g.auth.registerAccount("bob", "correct horse", consent);
            FAIL("expected ApiError");
        } catch (const api::ApiError& e) {
            CHECK(e.code() == "consent_required");
        }
    }
    CHECK(kAllTicked.complete());
}

TEST_CASE("register, then log in with the same password", "[auth]") {
    Fixture f;
    auto registered = f.auth.registerAccount("alice", "correct horse", kAllTicked);
    CHECK(registered.identity.userId == "u1");
    CHECK(registered.identity.privateKey.size() == crypto::kBoxSecretKeyBytes);
    CHECK(registered.recoveryKey.size() == 64);  // 13 groups of 4, dash-separated
    // The server only ever sees the wrapped key.
    CHECK(f.server.account["encryptedPrivateKey"] != crypto::base64Encode(registered.identity.privateKey));

    auto outcome = f.auth.login("alice", "correct horse");
    REQUIRE(std::holds_alternative<SelfIdentity>(outcome));
    const auto& identity = std::get<SelfIdentity>(outcome);
    CHECK(copy(identity.privateKey) == copy(registered.identity.privateKey));
    CHECK(identity.publicKey == registered.identity.publicKey);
    CHECK_FALSE(identity.mfaEnabled);

    try {
        f.auth.login("alice", "wrong");
        FAIL("expected ApiError");
    } catch (const api::ApiError& e) {
        CHECK(e.code() == "invalid_credentials");
    }
}

TEST_CASE("MFA login goes through verifyMfa", "[auth]") {
    Fixture f;
    auto registered = f.auth.registerAccount("alice", "pw", kAllTicked);
    f.server.mfaEnabled = true;

    CHECK(std::holds_alternative<MfaRequired>(f.auth.login("alice", "pw")));
    CHECK_THROWS_AS(f.auth.verifyMfa("alice", "pw", "000000"), api::ApiError);
    const auto identity = f.auth.verifyMfa("alice", "pw", "123456");
    CHECK(identity.mfaEnabled);
    CHECK(copy(identity.privateKey) == copy(registered.identity.privateKey));
}

TEST_CASE("recovery key restores the same private key under a new password", "[auth][recovery]") {
    Fixture f;
    auto registered = f.auth.registerAccount("alice", "forgotten", kAllTicked);

    CHECK_THROWS_AS(f.auth.recover("alice", "not a key", "new pw", std::nullopt), api::UserFacingError);

    auto outcome = f.auth.recover("alice", registered.recoveryKey, "new pw", std::nullopt);
    REQUIRE(std::holds_alternative<SelfIdentity>(outcome));
    CHECK(copy(std::get<SelfIdentity>(outcome).privateKey) == copy(registered.identity.privateKey));

    // The re-wrapped key now opens with the new password.
    auto login = f.auth.login("alice", "new pw");
    REQUIRE(std::holds_alternative<SelfIdentity>(login));
    CHECK(copy(std::get<SelfIdentity>(login).privateKey) == copy(registered.identity.privateKey));
}

TEST_CASE("recovery asks for the MFA code before changing anything", "[auth][recovery]") {
    Fixture f;
    auto registered = f.auth.registerAccount("alice", "pw", kAllTicked);
    f.server.mfaEnabled = true;
    const auto wrappedBefore = f.server.account["encryptedPrivateKey"];

    CHECK(std::holds_alternative<CodeRequired>(f.auth.recover("alice", registered.recoveryKey, "new", std::nullopt)));
    CHECK(std::holds_alternative<CodeRequired>(f.auth.recover("alice", registered.recoveryKey, "new", "  ")));
    CHECK(f.server.account["encryptedPrivateKey"] == wrappedBefore);

    try {
        f.auth.recover("alice", registered.recoveryKey, "new", "000000");
        FAIL("expected UserFacingError");
    } catch (const api::UserFacingError& e) {
        CHECK(std::string(e.what()) == "Wrong code. Check your authenticator app and try again.");
    }

    auto outcome = f.auth.recover("alice", registered.recoveryKey, "new", "123456");
    REQUIRE(std::holds_alternative<SelfIdentity>(outcome));
    CHECK(std::get<SelfIdentity>(outcome).mfaEnabled);
}

TEST_CASE("a wrong recovery key reads as a mismatch", "[auth][recovery]") {
    Fixture f;
    f.auth.registerAccount("alice", "pw", kAllTicked);
    const auto otherKey = crypto::recovery::format(crypto::recovery::generate());
    try {
        f.auth.recover("alice", otherKey, "new", std::nullopt);
        FAIL("expected UserFacingError");
    } catch (const api::UserFacingError& e) {
        CHECK(std::string(e.what()) == "That handle and recovery key don't match.");
    }
}

TEST_CASE("a public key that isn't this account's is refused at sign-in", "[auth][forged]") {
    Fixture f;
    f.auth.registerAccount("alice", "pw", kAllTicked);
    f.server.forgedPublicKey = crypto::base64Encode(crypto::generateKeyPair().publicKey);
    CHECK_THROWS_AS(f.auth.login("alice", "pw"), api::UserFacingError);
    f.server.forgedPublicKey.reset();
    CHECK(std::holds_alternative<SelfIdentity>(f.auth.login("alice", "pw")));
}

TEST_CASE("the same holds for a code sign-in and for recovery", "[auth][forged]") {
    Fixture f;
    auto registered = f.auth.registerAccount("alice", "pw", kAllTicked);
    SECTION("code") {
        f.server.mfaEnabled = true;
        f.server.forgedPublicKey = crypto::base64Encode(crypto::generateKeyPair().publicKey);
        CHECK(std::holds_alternative<MfaRequired>(f.auth.login("alice", "pw")));
        CHECK_THROWS_AS(f.auth.verifyMfa("alice", "pw", "123456"), api::UserFacingError);
    }
    SECTION("recovery") {
        f.server.forgedPublicKey = crypto::base64Encode(crypto::generateKeyPair().publicKey);
        CHECK_THROWS_AS(f.auth.recover("alice", registered.recoveryKey, "new pw", std::nullopt), api::UserFacingError);
    }
}

TEST_CASE("recovery onto sign-in keys pins the account", "[auth][forged]") {
    Fixture f;
    auto registered = f.auth.registerAccount("Laura", "pw", kAllTicked);
    f.server.signInKeys = true;
    REQUIRE(std::holds_alternative<SelfIdentity>(f.auth.recover("Laura", registered.recoveryKey, "new pw", std::nullopt)));
    f.server.signInKeys = false;
    // The server now says this account never moved: the password still doesn't go.
    CHECK_THROWS_AS(f.auth.login("laura", "new pw"), api::UserFacingError);
    CHECK(f.server.passwordSeen.find("new pw") == std::string::npos);
}

TEST_CASE("logout clears the session even when the server is unreachable", "[auth]") {
    Fixture f;
    f.cookies->saveFromResponse(*net::parseUrl("https://example.com/api/auth/login"), {"sid=abc; Path=/"});
    f.transport->failWithNetworkError = true;
    f.auth.logout();
    CHECK(f.cookies->empty());
}
