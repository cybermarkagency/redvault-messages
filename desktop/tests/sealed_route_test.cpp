// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The sealed backup route's switching rules (docs/tor-sealed-fallback.md, e2ev3/src/sealed_route.cpp).
// The sealing itself is tested in crypto-core (tests/sealed.rs) and end to end against the real gateway.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <map>
#include <sstream>

#include <nlohmann/json.hpp>

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

    explicit Rig(bool withKey = true, std::shared_ptr<SealedFallbackTransport::Sealer> sealer = nullptr) {
        SealedFallbackTransport::Config config;
        config.onionOrigin = kOnion;
        config.pathPrefix = "/redvaultmessages";
        config.gatewayUrl = kGateway;
        if (withKey) config.keyConfig = testKey();
        config.probeEvery = std::chrono::seconds(3600);  // probes are driven by hand here
        config.probeUrl = kApi + "public/routing";
        config.sealer = std::move(sealer);
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

TEST_CASE("the backup route carries API requests only, and other bodies up to 8 MB at a time") {
    Rig rig;
    rig.failOnion(3);
    REQUIRE(rig.route->onBackup());
    CHECK_THROWS_AS(rig.route->send(HttpRequest{"GET", kOnion + "/redvaultmessages/desktop/latest.json", {}, ""}), NetworkError);
    std::istringstream big(std::string(9 * 1024 * 1024, 'x'));
    CHECK_THROWS_AS(rig.route->sendStreaming(rig.post("files"), {.upload = &big, .uploadSize = 9ull * 1024 * 1024}), NetworkError);
    CHECK(rig.website->requests.empty());
}

// --- Chat files bigger than one sealed message: parts up, ranges down, through a fake gateway ---

namespace {

/// Test framing in place of Oblivious HTTP, so the fake gateway can read what the app sealed:
/// a 4-byte head length, the head (JSON), then the body. Never used outside these tests.
std::string frame(const std::string& head, const std::string& body) {
    const auto n = static_cast<std::uint32_t>(head.size());
    std::string out{static_cast<char>(n >> 24), static_cast<char>(n >> 16), static_cast<char>(n >> 8), static_cast<char>(n)};
    return out + head + body;
}

void unframe(const std::string& framed, std::string& head, std::string& body) {
    REQUIRE(framed.size() >= 4);
    const auto byte = [&](int i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(framed[static_cast<std::size_t>(i)])); };
    const std::uint32_t n = byte(0) << 24 | byte(1) << 16 | byte(2) << 8 | byte(3);
    head = framed.substr(4, n);
    body = framed.substr(4 + n);
}

class PlainSealer final : public SealedFallbackTransport::Sealer {
public:
    class Pending final : public Sealer::Pending {
    public:
        void open(const std::string& reply, std::string& headJson, std::string& body) override { unframe(reply, headJson, body); }
    };
    std::string seal(const std::string& headJson, const std::string& body, std::unique_ptr<Sealer::Pending>& pending) override {
        pending = std::make_unique<Pending>();
        return frame(headJson, body);
    }
};

using nlohmann::json;

/// The fake gateway in front of a fake chat-file API with the server's rules (fileService.receiveUploadPart).
struct FakeFileApi {
    static constexpr std::uint64_t kPart = SealedFallbackTransport::kPartBytes;
    std::map<std::uint64_t, std::string> parts;
    std::string stored;
    std::uint64_t declared = 0;
    std::size_t largestSealed = 0;
    std::vector<json> heads;
    int dropNext = 0;  // answers this many requests with nothing (a lost connection)

    HttpResponse handle(const HttpRequest& outer) {
        if (dropNext > 0) {
            --dropNext;
            throw NetworkError("connection reset");
        }
        largestSealed = std::max(largestSealed, outer.body.size());
        std::string headText;
        std::string body;
        unframe(outer.body, headText, body);
        const json head = json::parse(headText);
        heads.push_back(head);
        std::map<std::string, std::string> headers;
        for (const auto& pair : head["headers"]) headers[pair[0].get<std::string>()] = pair[1].get<std::string>();
        const auto answer = [](int status, json::array_t headerList, const std::string& answerBody) {
            return HttpResponse{200, {}, frame(json{{"status", status}, {"headers", headerList}}.dump(), answerBody)};
        };
        if (head["method"] == "PUT") {
            const auto range = headers.at("content-range");
            std::uint64_t start = 0;
            std::uint64_t end = 0;
            std::uint64_t total = 0;
            REQUIRE(std::sscanf(range.c_str(), "bytes %llu-%llu/%llu", reinterpret_cast<unsigned long long*>(&start),
                                reinterpret_cast<unsigned long long*>(&end), reinterpret_cast<unsigned long long*>(&total)) == 3);
            REQUIRE(total == declared);
            REQUIRE(start % kPart == 0);
            REQUIRE(body.size() == end - start + 1);
            REQUIRE((end + 1 == total || body.size() == kPart));
            parts[start] = body;
            if (end + 1 < total) return answer(202, {}, R"({"status":"pending"})");
            for (const auto& [offset, data] : parts) stored += data;
            return answer(200, {json::array({"content-type", "application/json"})}, R"({"status":"uploaded"})");
        }
        const auto range = headers.find("range");
        if (range == headers.end()) return answer(200, {}, stored);
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        REQUIRE(std::sscanf(range->second.c_str(), "bytes=%llu-%llu", reinterpret_cast<unsigned long long*>(&start),
                            reinterpret_cast<unsigned long long*>(&end)) == 2);
        end = std::min<std::uint64_t>(end, stored.size() - 1);
        const std::string slice = stored.substr(start, end - start + 1);
        return answer(206,
                      {json::array({"content-type", "application/octet-stream"}),
                       json::array({"content-range", "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                                                         std::to_string(stored.size())}),
                       json::array({"content-length", std::to_string(slice.size())})},
                      slice);
    }
};

std::string randomBytes(std::size_t size) {
    std::string out(size, '\0');
    std::uint32_t x = 2463534242u;
    for (auto& c : out) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = static_cast<char>(x);
    }
    return out;
}

}  // namespace

TEST_CASE("a chat file over 8 MB goes up in parts and comes down in ranges on the backup route") {
    Rig rig(true, std::make_shared<PlainSealer>());
    auto api = std::make_shared<FakeFileApi>();
    rig.website->handler = [api](const HttpRequest& request) {
        CHECK(request.url == kGateway);
        return api->handle(request);
    };
    rig.failOnion(3);
    REQUIRE(rig.route->onBackup());
    rig.website->requests.clear();

    const std::string file = randomBytes(20 * 1024 * 1024 + 12345);  // three parts
    api->declared = file.size();
    const std::string url = kApi + "v1/files/0f8fad5b-d9cb-469f-a165-70867728950e/content";
    std::istringstream upload(file);
    api->dropNext = 0;
    const auto put = rig.route->sendStreaming(
        HttpRequest{"PUT", url, {{"content-type", "application/octet-stream"}, {"cookie", "session=s"}}, ""},
        {.upload = &upload, .uploadSize = file.size()});
    CHECK(put.status == 200);
    CHECK(put.body == R"({"status":"uploaded"})");
    CHECK(api->stored == file);
    REQUIRE(api->heads.size() == 3);
    CHECK(api->heads[0]["path"] == "/api/v1/files/0f8fad5b-d9cb-469f-a165-70867728950e/content");
    // Every sealed message stays within one message's limit.
    CHECK(api->largestSealed <= SealedFallbackTransport::kMaxSealedBody);

    api->heads.clear();
    api->largestSealed = 0;
    api->dropNext = 0;
    std::ostringstream download;
    const auto get = rig.route->sendStreaming(HttpRequest{"GET", url, {{"cookie", "session=s"}}, ""}, {.download = &download});
    CHECK(get.status == 200);
    CHECK(get.header("content-length") == std::to_string(file.size()));
    CHECK_FALSE(get.header("content-range").has_value());
    CHECK(download.str() == file);
    CHECK(api->heads.size() == 3);
    CHECK(api->largestSealed <= SealedFallbackTransport::kMaxSealedBody);
}

TEST_CASE("a lost part is sent again; a part refused by the server ends the upload with its answer") {
    Rig rig(true, std::make_shared<PlainSealer>());
    auto api = std::make_shared<FakeFileApi>();
    rig.website->handler = [api](const HttpRequest& request) { return api->handle(request); };
    rig.failOnion(3);
    const std::string file = randomBytes(9 * 1024 * 1024);
    api->declared = file.size();
    const std::string url = kApi + "v1/files/0f8fad5b-d9cb-469f-a165-70867728950e/content";

    std::istringstream upload(file);
    api->dropNext = 1;  // the first part's connection drops once
    CHECK(rig.route->sendStreaming(HttpRequest{"PUT", url, {}, ""}, {.upload = &upload, .uploadSize = file.size()}).status == 200);
    CHECK(api->stored == file);

    // The server refuses the first part: the app gets that answer, and nothing more is sent.
    rig.website->handler = [](const HttpRequest&) {
        return HttpResponse{200, {}, frame(R"({"status":410,"headers":[]})", R"({"error":"upload_expired"})")};
    };
    rig.website->requests.clear();
    std::istringstream again(file);
    const auto refused = rig.route->sendStreaming(HttpRequest{"PUT", url, {}, ""}, {.upload = &again, .uploadSize = file.size()});
    CHECK(refused.status == 410);
    CHECK(refused.body == R"({"error":"upload_expired"})");
    CHECK(rig.website->requests.size() == 1);
}

TEST_CASE("a small download on the backup route is one ranged request, and a route without ranges still works") {
    Rig rig(true, std::make_shared<PlainSealer>());
    rig.failOnion(3);
    rig.website->handler = [](const HttpRequest&) { return HttpResponse{200, {}, frame(R"({"status":200,"headers":[]})", "whole")}; };
    rig.website->requests.clear();
    std::ostringstream sink;
    CHECK(rig.route->sendStreaming(rig.get("avatars/1"), {.download = &sink}).status == 200);
    CHECK(sink.str() == "whole");
    CHECK(rig.website->requests.size() == 1);
}
