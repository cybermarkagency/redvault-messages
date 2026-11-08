// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <string>

#include "fake_transport.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/data/sign_in.hpp"

using namespace redvault;
using namespace redvault::data;
using api::Json;
using redvault::test::FakeTransport;

namespace {

const std::string kSalt = crypto::base64Encode(crypto::Bytes(16, 7));

struct Fixture {
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    api::ApiClient api{"https://example.com/api/", transport, std::make_shared<net::SessionCookieJar>()};

    void preLogin(int authVersion, bool upgrade) {
        transport->enqueue(200, Json{{"kdfSalt", kSalt}, {"authVersion", authVersion}, {"upgrade", upgrade}}.dump());
    }
};

}  // namespace

TEST_CASE("the sign-in key matches the vector every client tests against", "[signin]") {
    CHECK(crypto::deriveSignInKey("correct horse battery staple", crypto::Bytes(16, 7)) == "FctalMlnA5L7J4K3YTVRGWqHwQRvCdnMWno0LvXQI2I=");
}

TEST_CASE("an upgraded account never sends the password", "[signin]") {
    Fixture f;
    f.preLogin(2, true);
    const auto credential = credentialFor(f.api, "alice", "a password");
    CHECK(credential.password != "a password");
    CHECK(credential.authVersion == 2);
}

TEST_CASE("an old account sends the password once, with the key to move to", "[signin]") {
    Fixture f;
    f.preLogin(1, true);
    const auto credential = credentialFor(f.api, "bob", "a password");
    CHECK(credential.password == "a password");
    REQUIRE(credential.upgradeAuthKey);
    CHECK(*credential.upgradeAuthKey == crypto::deriveSignInKey("a password", crypto::base64Decode(kSalt)));
}

TEST_CASE("a pinned account never falls back to sending the password", "[signin]") {
    Fixture f;
    pinSignInKeys("carol");
    f.preLogin(1, true);
    CHECK_THROWS_AS(credentialFor(f.api, "carol", "a password"), api::UserFacingError);
    f.preLogin(1, false);
    CHECK_THROWS_AS(newCredential(f.api, "carol", "a password", crypto::Bytes(16, 7)), api::UserFacingError);
}

TEST_CASE("a sign-in pin holds for any spelling of the handle", "[signin]") {
    Fixture f;
    pinSignInKeys("  Erin ");
    for (const std::string spelling : {"erin", "ERIN", "  eRiN\t", "Erin"}) {
        CAPTURE(spelling);
        f.preLogin(1, false);
        CHECK_THROWS_AS(credentialFor(f.api, spelling, "a password"), api::UserFacingError);
    }
}

TEST_CASE("control: another account isn't pinned by that", "[signin]") {
    Fixture f;
    pinSignInKeys("frank");
    f.preLogin(1, false);
    CHECK(credentialFor(f.api, "franklin", "a password").password == "a password");
}

TEST_CASE("a sign-in pin goes with the account when it's renamed", "[signin]") {
    Fixture f;
    pinSignInKeys("grace");
    moveSignInPin("GRACE", "Hopper");
    f.preLogin(1, false);
    CHECK_THROWS_AS(credentialFor(f.api, "hopper", "a password"), api::UserFacingError);
}

TEST_CASE("control: renaming an account with no pin pins nothing", "[signin]") {
    Fixture f;
    moveSignInPin("ivan", "ivana");
    f.preLogin(1, false);
    CHECK(credentialFor(f.api, "ivana", "a password").password == "a password");
}

TEST_CASE("a pin stored under the spelling that was typed still counts", "[signin]") {
    // How the app stored them before handles were folded.
    static std::set<std::string> stored{"Judy"};
    setSignInPins({[](const std::string& handle) { return stored.count(handle) > 0; }, [](const std::string& handle) { stored.insert(handle); }});
    Fixture f;
    f.preLogin(1, false);
    CHECK_THROWS_AS(credentialFor(f.api, " Judy ", "a password"), api::UserFacingError);
    pinSignInKeys("KARL ");
    CHECK(stored.count("karl") == 1);
}
