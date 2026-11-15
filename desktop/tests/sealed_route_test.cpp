// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The sealed backup route's switching rules (docs/tor-sealed-fallback.md, e2ev3/src/sealed_route.cpp).
// The sealing itself is tested in crypto-core (tests/sealed.rs) and end to end against the real gateway.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <sstream>

#include "fake_transport.hpp"
#include "redvault/e2ev3/sealed_route.hpp"

using redvault::net::HttpRequest;
using redvault::net::HttpResponse;
using redvault::net::NetworkError;
using redvault::net::SealedFallbackTransport;
using redvault::net::TorUnavailableError;
using redvault::test::FakeTransport;

namespace {

// crypto-core/tests/vectors/sealed_test_key.json: the test key, never the live gateway's.
const char* kTestKeyHex = "0100208cfd57b6e65ffdbfbb5087ff6665b7122e1fee211913dee4ced8a3357cdfa079000400010003";
const std::string kOnion = "http://abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz2345.onion";
const std::string kApi = kOnion + "/redvaultmessages/api/";
const std::string kGateway = "https://www.example.test/redvaultmessages/ohttp";

std::vector<std::uint8_t> testKey() {
    std::vector<std::uint8_t> key;
    const std::string hex = kTestKeyHex;
    for (std::size_t i = 0; i < hex.size(); i += 2) key.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return key;
}

/// An onion transport that fails (or not) on demand, and a Tor-down mode.
class FlakyOnion final : public redvault::net::HttpTransport {
public:
    bool failing = false;
    bool torDown = false;
    int calls = 0;
    HttpResponse send(const HttpRequest&) override {
        ++calls;
        if (torDown) throw TorUnavailableError("Tor isn't connected");
        if (failing) throw NetworkError("timed out");
        return HttpResponse{200, {}, "onion"};
    }
};

struct Rig {
    std::shared_ptr<FlakyOnion> onion = std::make_shared<FlakyOnion>();
    std::shared_ptr<FakeTransport> website = std::make_shared<FakeTransport>();
    std::shared_ptr<std::atomic<bool>> onionOnly = std::make_shared<std::atomic<bool>>(false);
    std::vector<bool> changes;
    std::unique_ptr<SealedFallbackTransport> route;

    explicit Rig(bool withKey = true) {
        SealedFallbackTransport::Config config;
        config.onionOrigin = kOnion;
        config.pathPrefix = "/redvaultmessages";
        config.gatewayUrl = kGateway;
        if (withKey) config.keyConfig = testKey();
        config.probeEvery = std::chrono::seconds(3600);  // probes are driven by hand here
        config.probeUrl = kApi + "public/routing";
        auto flag = onionOnly;
        route = std::make_unique<SealedFallbackTransport>(
            onion, website, config, [flag] { return !flag->load(); }, [this](bool backup) { changes.push_back(backup); });
        // The website stands in for the gateway: it answers 502 (it can't open requests without the
        // gateway's private key), which is enough to see what reached it.
        website->handler = [](const HttpRequest&) { return HttpResponse{502, {}, ""}; };
    }

    HttpRequest post(const std::string& path) {
        return HttpRequest{"POST", kApi + path, {{"content-type", "application/json"}, {"cookie", "session=secret-session-value"}}, R"({"text":"hello"})"};
    }
    HttpRequest get(const std::string& path) { return HttpRequest{"GET", kApi + path, {{"cookie", "session=secret-session-value"}}, ""}; }

    void failOnion(int times) {
        onion->failing = true;
        for (int i = 0; i < times; ++i) {
            try {
                route->send(post("messages"));
            } catch (const NetworkError&) {
            }
        }
    }
};

}  // namespace

TEST_CASE("the onion stays in use while it answers") {
    Rig rig;
    CHECK(rig.route->send(rig.get("me")).body == "onion");
    CHECK_FALSE(rig.route->onBackup());
    CHECK(rig.website->requests.empty());
}

TEST_CASE("three onion failures in a row switch to the backup route, sealed") {
    Rig rig;
    rig.failOnion(3);
    CHECK(rig.route->onBackup());
    CHECK(rig.changes == std::vector<bool>{true});
    // The next request goes to the gateway, sealed: nothing readable in it.
    CHECK_THROWS_AS(rig.route->send(rig.post("messages")), NetworkError);  // the stand-in gateway answers 502
    REQUIRE(rig.website->requests.size() == 1);
    const auto& sealed = rig.website->last();
    CHECK(sealed.method == "POST");
    CHECK(sealed.url == kGateway);
    CHECK(FakeTransport::headerOf(sealed, "content-type") == "message/ohttp-req");
    CHECK(sealed.headers.size() == 1);  // no cookie, no user agent, nothing else outside the seal
    CHECK(sealed.body.size() >= 1024);  // padded
    CHECK(sealed.body.find("secret-session-value") == std::string::npos);
    CHECK(sealed.body.find("/api/messages") == std::string::npos);
    CHECK(sealed.body.find("hello") == std::string::npos);
}

TEST_CASE("a failed GET is sent again on the backup route at the switch, a POST is not") {
    Rig rig;
    rig.failOnion(2);
    rig.website->requests.clear();
    CHECK_THROWS_AS(rig.route->send(rig.get("conversations")), NetworkError);
    CHECK(rig.route->onBackup());
    CHECK(rig.website->requests.size() == 1);  // the GET went on to the gateway

    Rig other;
    other.failOnion(3);
    CHECK(other.website->requests.empty());  // the third (switching) POST wasn't re-sent
}

TEST_CASE("Tor being down never switches routes") {
    Rig rig;
    rig.onion->torDown = true;
    for (int i = 0; i < 5; ++i) CHECK_THROWS_AS(rig.route->send(rig.get("me")), TorUnavailableError);
    CHECK_FALSE(rig.route->onBackup());
    CHECK(rig.website->requests.empty());
}

TEST_CASE("only use the onion address: never the backup route") {
    Rig rig;
    rig.onionOnly->store(true);
    rig.failOnion(10);
    CHECK_FALSE(rig.route->onBackup());
    CHECK(rig.website->requests.empty());
}

TEST_CASE("turning only-onion on while on the backup route moves back at once") {
    Rig rig;
    rig.failOnion(3);
    REQUIRE(rig.route->onBackup());
    rig.onionOnly->store(true);
    rig.onion->failing = false;
    CHECK(rig.route->send(rig.get("me")).body == "onion");
    CHECK_FALSE(rig.route->onBackup());
}

TEST_CASE("without a pinned key there is no backup route") {
    Rig rig(false);
    rig.failOnion(10);
    CHECK_FALSE(rig.route->onBackup());
}

TEST_CASE("the onion check moves back once the onion answers") {
    Rig rig;
    rig.failOnion(3);
    REQUIRE(rig.route->onBackup());
    CHECK_FALSE(rig.route->probeOnionNow());  // still failing
    CHECK(rig.route->onBackup());
    rig.onion->failing = false;
    CHECK(rig.route->probeOnionNow());
    CHECK_FALSE(rig.route->onBackup());
    CHECK(rig.changes == std::vector<bool>{true, false});
}

TEST_CASE("the backup route carries API requests only, and up to 8 MB at a time") {
    Rig rig;
    rig.failOnion(3);
    REQUIRE(rig.route->onBackup());
    CHECK_THROWS_AS(rig.route->send(HttpRequest{"GET", kOnion + "/redvaultmessages/desktop/latest.json", {}, ""}), NetworkError);
    std::istringstream big(std::string(9 * 1024 * 1024, 'x'));
    CHECK_THROWS_AS(rig.route->sendStreaming(rig.post("files"), {.upload = &big, .uploadSize = 9ull * 1024 * 1024}), NetworkError);
    CHECK(rig.website->requests.empty());
}
