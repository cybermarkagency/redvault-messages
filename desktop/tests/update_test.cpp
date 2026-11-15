// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>
#include <sodium.h>

#include "fake_transport.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/update/update.hpp"
#include "redvault/util/text.hpp"

using namespace redvault;
using namespace redvault::update;
using Json = nlohmann::json;

namespace {

const std::string kPrefix = "https://www.redvaultsystems.com/redvaultmessages/desktop/";

Release release(const std::string& version = "0.2.0") {
    return Release{version, kPrefix + "Redvault-Setup-" + version + ".exe", std::string(64, 'a'), 1234,
                   "Bug fixes", "2026-09-26T00:00:00Z"};
}

/// Signs arbitrary payload bytes, to forge manifests signManifest() would never produce.
std::string signRaw(const std::string& payload, crypto::ByteView secretKey) {
    crypto::Bytes signature(crypto_sign_BYTES);
    crypto_sign_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(payload.data()),
                         payload.size(), secretKey.data());
    return Json{{"payload", crypto::base64Encode(crypto::toBytes(payload))}, {"signature", crypto::base64Encode(signature)}}.dump();
}

}  // namespace

TEST_CASE("versions parse and compare numerically", "[update]") {
    CHECK(parseVersion("0.1.0") == Version{0, 1, 0});
    CHECK(parseVersion("v1.10.2") == Version{1, 10, 2});
    CHECK(*parseVersion("0.10.0") > *parseVersion("0.9.9"));
    CHECK_FALSE(parseVersion("1.2"));
    CHECK_FALSE(parseVersion("1.2.3.4"));
    CHECK_FALSE(parseVersion("1.x.3"));
    CHECK_FALSE(parseVersion("-1.2.3"));
}

TEST_CASE("a signed manifest for a newer version is accepted", "[update]") {
    const auto keys = generateSigningKey();
    const auto manifest = signManifest(release("0.2.0"), keys.secretKey);
    const auto found = verifyManifest(manifest, keys.publicKey, "0.1.0", kPrefix);
    REQUIRE(found);
    CHECK(found->version == "0.2.0");
    CHECK(found->url == kPrefix + "Redvault-Setup-0.2.0.exe");
    CHECK(found->size == 1234);
    CHECK(found->notes == "Bug fixes");
}

TEST_CASE("the same or an older version is never offered", "[update]") {
    const auto keys = generateSigningKey();
    CHECK_FALSE(verifyManifest(signManifest(release("0.2.0"), keys.secretKey), keys.publicKey, "0.2.0", kPrefix));
    CHECK_FALSE(verifyManifest(signManifest(release("0.1.9"), keys.secretKey), keys.publicKey, "0.2.0", kPrefix));
}

TEST_CASE("forged, tampered or misdirected manifests are rejected", "[update]") {
    const auto keys = generateSigningKey();
    const auto attacker = generateSigningKey();
    const auto good = signManifest(release(), keys.secretKey);

    // Signed with someone else's key.
    CHECK_THROWS_AS(verifyManifest(signManifest(release(), attacker.secretKey), keys.publicKey, "0.1.0", kPrefix),
                    UpdateError);

    // Payload changed after signing (e.g. the URL swapped), signature kept.
    auto envelope = Json::parse(good);
    auto payload = crypto::base64Decode(envelope["payload"].get<std::string>());
    std::string text(payload.begin(), payload.end());
    text.replace(text.find("Redvault-Setup"), 8, "Evilvaul");
    envelope["payload"] = crypto::base64Encode(crypto::toBytes(text));
    CHECK_THROWS_AS(verifyManifest(envelope.dump(), keys.publicKey, "0.1.0", kPrefix), UpdateError);

    // Validly signed, but pointing somewhere else.
    auto elsewhere = release();
    elsewhere.url = "https://evil.example/Redvault-Setup-0.2.0.exe";
    CHECK_THROWS_AS(verifyManifest(signManifest(elsewhere, keys.secretKey), keys.publicKey, "0.1.0", kPrefix),
                    UpdateError);
    elsewhere.url = kPrefix + "../../other/app.exe";
    CHECK_THROWS_AS(verifyManifest(signManifest(elsewhere, keys.secretKey), keys.publicKey, "0.1.0", kPrefix),
                    UpdateError);

    // Garbage, and what nginx's index.html fallback would return for a missing file.
    CHECK_THROWS_AS(verifyManifest("not json", keys.publicKey, "0.1.0", kPrefix), UpdateError);
    CHECK_THROWS_AS(verifyManifest("<!doctype html><html></html>", keys.publicKey, "0.1.0", kPrefix), UpdateError);

    // A build without a key never trusts anything.
    CHECK_THROWS_AS(verifyManifest(good, crypto::ByteView{}, "0.1.0", kPrefix), UpdateError);

    // Validly signed, but for another product or with a bad hash.
    CHECK_THROWS_AS(verifyManifest(signRaw(R"({"product":"other","version":"9.0.0","url":")" + kPrefix +
                                               R"(a.exe","sha256":")" + std::string(64, 'a') + R"(","size":1})",
                                           keys.secretKey),
                                   keys.publicKey, "0.1.0", kPrefix),
                    UpdateError);
    CHECK_THROWS_AS(verifyManifest(signRaw(R"({"product":"redvault-desktop","version":"9.0.0","url":")" + kPrefix +
                                               R"(a.exe","sha256":"xyz","size":1})",
                                           keys.secretKey),
                                   keys.publicKey, "0.1.0", kPrefix),
                    UpdateError);
}

TEST_CASE("the installer must match the manifest's size and hash", "[update]") {
    const auto dir = std::filesystem::temp_directory_path() / ("redvault-update-" + util::randomUuid());
    std::filesystem::create_directories(dir);
    const auto file = dir / "setup.exe";
    std::ofstream(file, std::ios::binary) << "installer bytes";

    Release r = release();
    r.size = 15;
    r.sha256 = crypto::toHex(crypto::sha256(crypto::toBytes("installer bytes")));
    CHECK(installerMatches(file, r));

    r.size = 16;
    CHECK_FALSE(installerMatches(file, r));
    r.size = 15;
    r.sha256[0] = r.sha256[0] == 'a' ? 'b' : 'a';
    CHECK_FALSE(installerMatches(file, r));
    CHECK_FALSE(installerMatches(dir / "missing.exe", r));
    std::filesystem::remove_all(dir);
}

TEST_CASE("the release key file opens only with its passphrase", "[update]") {
    const auto keys = generateSigningKey();
    const auto file = sealSigningKey(keys, "correct horse battery staple");
    CHECK(file.find(crypto::base64Encode(keys.secretKey)) == std::string::npos);

    const auto opened = openSigningKey(file, "correct horse battery staple");
    CHECK(opened.publicKey == keys.publicKey);
    // It signs manifests the app accepts.
    CHECK(verifyManifest(signManifest(release(), opened.secretKey), keys.publicKey, "0.1.0", kPrefix));

    CHECK_THROWS_AS(openSigningKey(file, "wrong"), UpdateError);
    CHECK_THROWS_AS(openSigningKey("{}", "x"), UpdateError);
}

// --- Freshness: a signed manifest says when it stops being news ---

namespace {

constexpr std::int64_t kJan2027 = 1798761600000;   // 2027-01-01T00:00:00Z
constexpr std::int64_t kFeb2027 = 1801440000000;   // 2027-02-01T00:00:00Z
constexpr std::int64_t kMar2027 = 1803859200000;   // 2027-03-01T00:00:00Z

Release expiring(const std::string& version, const std::string& expires) {
    Release r = release(version);
    r.expires = expires;
    return r;
}

}  // namespace

TEST_CASE("control: a manifest inside its date is current and says nothing", "[update][freshness]") {
    const auto keys = generateSigningKey();
    const auto manifest = signManifest(expiring("0.2.0", "2027-02-01T00:00:00Z"), keys.secretKey);
    const auto check = checkManifest(manifest, keys.publicKey, "0.1.0", kPrefix, kJan2027);
    CHECK(check.freshness == Freshness::Current);
    REQUIRE(check.release);
    CHECK(check.release->expires == "2027-02-01T00:00:00Z");
}

TEST_CASE("an expired manifest is flagged, whether or not it offers anything", "[update][freshness]") {
    const auto keys = generateSigningKey();
    // A replayed manifest for the version already installed: without this, the app would say "up to date" for ever.
    const auto same = signManifest(expiring("0.2.0", "2027-02-01T00:00:00Z"), keys.secretKey);
    const auto frozen = checkManifest(same, keys.publicKey, "0.2.0", kPrefix, kMar2027);
    CHECK(frozen.freshness == Freshness::Expired);
    CHECK_FALSE(frozen.release);

    // Newer than installed, but old news: still offered (it can only move forward), with the warning.
    const auto newer = checkManifest(same, keys.publicKey, "0.1.0", kPrefix, kMar2027);
    CHECK(newer.freshness == Freshness::Expired);
    CHECK(newer.release);

    // The very moment it runs out counts.
    CHECK(checkManifest(same, keys.publicKey, "0.2.0", kPrefix, kFeb2027).freshness == Freshness::Expired);
}

TEST_CASE("a manifest older than the installed version is still never offered", "[update][freshness]") {
    const auto keys = generateSigningKey();
    const auto old = signManifest(expiring("0.1.9", "2027-02-01T00:00:00Z"), keys.secretKey);
    CHECK_FALSE(checkManifest(old, keys.publicKey, "0.2.0", kPrefix, kJan2027).release);
    CHECK_FALSE(checkManifest(old, keys.publicKey, "0.2.0", kPrefix, kMar2027).release);
}

TEST_CASE("a manifest from before expiry dates is accepted, but only with a warning", "[update][freshness]") {
    const auto keys = generateSigningKey();
    const auto legacy = signManifest(release("0.2.0"), keys.secretKey);
    CHECK(Json::parse(std::string(reinterpret_cast<const char*>(crypto::base64Decode(Json::parse(legacy)["payload"].get<std::string>()).data()),
                                  crypto::base64Decode(Json::parse(legacy)["payload"].get<std::string>()).size()))
              .contains("expires") == false);
    const auto check = checkManifest(legacy, keys.publicKey, "0.1.0", kPrefix, kJan2027);
    CHECK(check.freshness == Freshness::NoExpiry);
    CHECK(check.release);
}

TEST_CASE("the expiry is covered by the signature, and a bad one is refused", "[update][freshness]") {
    const auto keys = generateSigningKey();
    const auto good = signManifest(expiring("0.2.0", "2027-02-01T00:00:00Z"), keys.secretKey);

    // Pushing the date forward in a stale manifest breaks the signature.
    auto envelope = Json::parse(good);
    auto payload = crypto::base64Decode(envelope["payload"].get<std::string>());
    std::string text(payload.begin(), payload.end());
    text.replace(text.find("2027-02-01"), 10, "2099-02-01");
    envelope["payload"] = crypto::base64Encode(crypto::toBytes(text));
    CHECK_THROWS_AS(checkManifest(envelope.dump(), keys.publicKey, "0.1.0", kPrefix, kMar2027), UpdateError);

    // Correctly signed nonsense isn't read as "no expiry".
    for (const auto& bad : {Json("soon"), Json(1801440000), Json(nullptr)}) {
        Json body = Json::parse(R"({"product":"redvault-desktop","version":"0.2.0","size":1234})");
        body["url"] = kPrefix + "Redvault-Setup-0.2.0.exe";
        body["sha256"] = std::string(64, 'a');
        body["expires"] = bad;
        CHECK_THROWS_AS(checkManifest(signRaw(body.dump(), keys.secretKey), keys.publicKey, "0.1.0", kPrefix, kJan2027), UpdateError);
    }
}

// --- Updates over Tor (docs/tor-by-default.md): the onion service's copy, never the normal internet ---

namespace {

constexpr const char* kManifest = "https://www.redvaultsystems.com/redvaultmessages/desktop/latest.json";
const std::string kOnion = "http://" + std::string(56, 'a') + ".onion/redvaultmessages/api/";
constexpr const char* kClearApi = "https://www.redvaultsystems.com/redvaultmessages/api/";

UpdateSource sourceFor(const SigningKeyPair& keys, const std::string& apiBase, bool torOnly) {
    return UpdateSource{kManifest, keys.publicKey, "0.1.0", kPrefix, apiBase, torOnly, kJan2027};
}

}  // namespace

TEST_CASE("a Tor-only build never sends an update check to the normal internet", "[update][tor]") {
    const auto keys = generateSigningKey();
    test::FakeTransport transport;
    transport.handler = [](const net::HttpRequest&) -> net::HttpResponse { FAIL("no request may be sent"); return {}; };

    // Tor-only, even if something points it at a clearnet address.
    CHECK_FALSE(fetchManifest(transport, sourceFor(keys, kClearApi, true)).release);
    // Something that only looks like an onion address (v2-looking, uppercase): no route at all, in any build.
    CHECK_FALSE(fetchManifest(transport, sourceFor(keys, "http://EXAMPLE.ONION/api/", false)).release);
    CHECK_FALSE(fetchManifest(transport, sourceFor(keys, "http://example.onion/api/", true)).release);
    // An address that can't be read gets no benefit of the doubt.
    CHECK_FALSE(fetchManifest(transport, sourceFor(keys, "not a url", true)).release);
    CHECK_FALSE(fetchManifest(transport, sourceFor(keys, "not a url", false)).release);
    CHECK(transport.requests.empty());

    CHECK_FALSE(updatesPermitted(kClearApi, true));
    CHECK_FALSE(updateRoute(kClearApi, true, kManifest, kPrefix));
}

TEST_CASE("over Tor the manifest and installer come from the onion service's copy", "[update][tor]") {
    const auto keys = generateSigningKey();
    const std::string onionHost = std::string(56, 'a') + ".onion";
    for (const bool torOnly : {true, false}) {
        const auto route = updateRoute(kOnion, torOnly, kManifest, kPrefix);
        REQUIRE(route);
        CHECK(route->viaTor);
        CHECK(route->manifestUrl == "http://" + onionHost + "/redvaultmessages/desktop/latest.json");
        CHECK(route->downloadPrefix == "http://" + onionHost + "/redvaultmessages/desktop/");
        CHECK(updatesPermitted(kOnion, torOnly));

        test::FakeTransport transport;
        transport.handler = [&](const net::HttpRequest&) {
            return net::HttpResponse{200, {{"content-type", "application/json"}}, signManifest(expiring("0.2.0", "2027-02-01T00:00:00Z"), keys.secretKey)};
        };
        const auto check = fetchManifest(transport, sourceFor(keys, kOnion, torOnly));
        REQUIRE(transport.requests.size() == 1);
        CHECK(transport.requests[0].url == route->manifestUrl);  // the onion host, never www.redvaultsystems.com
        CHECK(transport.requests[0].headers.empty());
        REQUIRE(check.release);
        // The signed URL names the published folder; the download goes to the same file on the onion service.
        CHECK(check.release->url.starts_with(kPrefix));
        const std::string url = installerUrl(*check.release, *route, kPrefix);
        CHECK(url == "http://" + onionHost + "/redvaultmessages/desktop/" + check.release->url.substr(std::string(kPrefix).size()));
        CHECK(net::parseUrl(url)->host == onionHost);
    }
    // A URL outside the published folder has no onion copy.
    Release outside = expiring("0.2.0", "2027-02-01T00:00:00Z");
    outside.url = "https://elsewhere.example/setup.exe";
    CHECK(installerUrl(outside, *updateRoute(kOnion, true, kManifest, kPrefix), kPrefix).empty());
    // The onion service has no copy yet: nothing newer, and still nothing on the normal internet.
    test::FakeTransport missing;
    missing.handler = [](const net::HttpRequest&) { return net::HttpResponse{404, {}, "gone"}; };
    CHECK_FALSE(fetchManifest(missing, sourceFor(keys, kOnion, true)).release);
    REQUIRE(missing.requests.size() == 1);
    CHECK(net::parseUrl(missing.requests[0].url)->host == onionHost);
}

TEST_CASE("control: a normal build still checks, and offers a newer version", "[update][tor]") {
    const auto keys = generateSigningKey();
    test::FakeTransport transport;
    transport.handler = [&](const net::HttpRequest&) {
        return net::HttpResponse{200, {{"content-type", "application/json"}}, signManifest(expiring("0.2.0", "2027-02-01T00:00:00Z"), keys.secretKey)};
    };
    CHECK(updatesPermitted(kClearApi, false));
    const auto check = fetchManifest(transport, sourceFor(keys, kClearApi, false));
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests[0].url == kManifest);
    CHECK(transport.requests[0].headers.empty());  // a plain GET: no cookie, nothing about the user
    REQUIRE(check.release);
    CHECK(check.release->version == "0.2.0");
}

TEST_CASE("nothing published yet means nothing newer, not an error", "[update][tor]") {
    const auto keys = generateSigningKey();
    test::FakeTransport transport;
    transport.handler = [](const net::HttpRequest&) { return net::HttpResponse{404, {}, "gone"}; };
    CHECK_FALSE(fetchManifest(transport, sourceFor(keys, kClearApi, false)).release);
    transport.handler = [](const net::HttpRequest&) { return net::HttpResponse{200, {{"content-type", "text/html; charset=utf-8"}}, "<html>"}; };
    CHECK_FALSE(fetchManifest(transport, sourceFor(keys, kClearApi, false)).release);
    transport.handler = [](const net::HttpRequest&) { return net::HttpResponse{500, {}, ""}; };
    CHECK_THROWS_AS(fetchManifest(transport, sourceFor(keys, kClearApi, false)), UpdateError);
}

// --- The Android app's manifest (android-latest.json) ---

namespace {

/// A fixed test key (never the release key): its seed is public, so the vector below is reproducible.
SigningKeyPair androidTestKey() {
    std::array<unsigned char, crypto_sign_SEEDBYTES> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<unsigned char>(0xA0 + i);
    SigningKeyPair keys;
    keys.publicKey = crypto::Bytes(crypto_sign_PUBLICKEYBYTES);
    keys.secretKey = crypto::SecretBytes(crypto_sign_SECRETKEYBYTES);
    crypto_sign_seed_keypair(keys.publicKey.data(), keys.secretKey.data(), seed.data());
    return keys;
}

Release androidRelease() {
    Release r;
    r.product = std::string(kAndroidProduct);
    r.version = "0.6.2";
    r.versionCode = 12;
    r.url = kPrefix + "RedVault-Messages-0.6.2.apk";
    r.sha256 = std::string(64, 'b');
    r.size = 4321;
    r.notes = "Test release";
    r.published = "2026-11-13T00:00:00Z";
    r.expires = "2027-02-11T00:00:00Z";
    return r;
}

}  // namespace

TEST_CASE("an Android manifest signs and verifies, with its versionCode", "[update]") {
    const auto keys = androidTestKey();
    const auto manifest = signManifest(androidRelease(), keys.secretKey);
    const auto checked = verifyAndroidManifest(manifest, keys.publicKey, kPrefix);
    CHECK(checked.versionCode == 12);
    CHECK(checked.version == "0.6.2");
    CHECK(checked.url == kPrefix + "RedVault-Messages-0.6.2.apk");
    CHECK(checked.size == 4321);
}

TEST_CASE("the desktop app never takes an Android manifest, nor the other way round", "[update]") {
    const auto keys = androidTestKey();
    CHECK_THROWS_AS(verifyManifest(signManifest(androidRelease(), keys.secretKey), keys.publicKey, "0.0.1", kPrefix), UpdateError);
    CHECK_THROWS_AS(verifyAndroidManifest(signManifest(release("9.9.9"), keys.secretKey), keys.publicKey, kPrefix), UpdateError);
}

TEST_CASE("Android manifests without a versionCode, outside the folder or not an APK are refused", "[update]") {
    const auto keys = androidTestKey();
    auto r = androidRelease();
    r.versionCode = 0;
    CHECK_THROWS_AS(verifyAndroidManifest(signManifest(r, keys.secretKey), keys.publicKey, kPrefix), UpdateError);
    r = androidRelease();
    r.url = "https://evil.example/RedVault-Messages-0.6.2.apk";
    CHECK_THROWS_AS(verifyAndroidManifest(signManifest(r, keys.secretKey), keys.publicKey, kPrefix), UpdateError);
    r = androidRelease();
    r.url = kPrefix + "RedVault-Messages-0.6.2.exe";
    CHECK_THROWS_AS(verifyAndroidManifest(signManifest(r, keys.secretKey), keys.publicKey, kPrefix), UpdateError);
}

#ifdef REDVAULT_ANDROID_UPDATE_VECTOR
/// The vector the Android app's tests verify (android/app/src/test/resources/android_update_vector.json):
/// signed here by the release tool's code, checked there by the app's Kotlin. Ed25519 is deterministic, so it's stable.
TEST_CASE("the shared Android update vector is what the signer produces", "[update]") {
    const auto keys = androidTestKey();
    const Json vector = {{"publicKey", crypto::base64Encode(keys.publicKey)}, {"manifest", signManifest(androidRelease(), keys.secretKey)}};
    const std::filesystem::path path = REDVAULT_ANDROID_UPDATE_VECTOR;
    if (!std::filesystem::exists(path)) {
        std::ofstream(path, std::ios::binary) << vector.dump(2) << "\n";
        FAIL("Wrote " << path.string() << ": run again");
    }
    std::ifstream in(path, std::ios::binary);
    const Json stored = Json::parse(in);
    CHECK(stored == vector);
}
#endif

namespace {

Release linuxRelease(std::string_view product, const std::string& version = "0.7.3") {
    Release r = release(version);
    r.product = std::string(product);
    r.url = product == kLinuxAppImageProduct ? kPrefix + "redvaultmessages-" + version + "-x86_64.AppImage"
                                             : kPrefix + "redvaultmessages_" + version + "_amd64.deb";
    return r;
}

}  // namespace

TEST_CASE("Linux manifests verify only as their own kind of install", "[update][linux]") {
    const auto keys = generateSigningKey();
    const auto appImage = signManifest(linuxRelease(kLinuxAppImageProduct), keys.secretKey);
    const auto deb = signManifest(linuxRelease(kLinuxDebProduct), keys.secretKey);

    const auto found = verifyManifest(appImage, keys.publicKey, "0.7.2", kPrefix, kLinuxAppImageProduct);
    REQUIRE(found);
    CHECK(found->product == kLinuxAppImageProduct);
    CHECK(found->url == kPrefix + "redvaultmessages-0.7.3-x86_64.AppImage");
    REQUIRE(verifyManifest(deb, keys.publicKey, "0.7.2", kPrefix, kLinuxDebProduct));

    // An AppImage install never takes the .deb's manifest (or the Windows one), and so on.
    CHECK_THROWS_AS(verifyManifest(deb, keys.publicKey, "0.7.2", kPrefix, kLinuxAppImageProduct), UpdateError);
    CHECK_THROWS_AS(verifyManifest(appImage, keys.publicKey, "0.7.2", kPrefix, kLinuxDebProduct), UpdateError);
    CHECK_THROWS_AS(verifyManifest(appImage, keys.publicKey, "0.7.2", kPrefix), UpdateError);
    const auto windows = signManifest(release("0.7.3"), keys.secretKey);
    CHECK_THROWS_AS(verifyManifest(windows, keys.publicKey, "0.7.2", kPrefix, kLinuxAppImageProduct), UpdateError);
    CHECK_THROWS_AS(verifyManifest(windows, keys.publicKey, "0.7.2", kPrefix, kLinuxDebProduct), UpdateError);
}

TEST_CASE("a Linux manifest naming the wrong kind of file is refused, even when signed", "[update][linux]") {
    const auto keys = generateSigningKey();
    auto r = linuxRelease(kLinuxAppImageProduct);
    r.url = kPrefix + "redvaultmessages-0.7.3.sh";
    CHECK_THROWS_AS(verifyManifest(signManifest(r, keys.secretKey), keys.publicKey, "0.7.2", kPrefix, kLinuxAppImageProduct),
                    UpdateError);
    r = linuxRelease(kLinuxDebProduct);
    r.url = kPrefix + "redvaultmessages-0.7.3-x86_64.AppImage";
    CHECK_THROWS_AS(verifyManifest(signManifest(r, keys.secretKey), keys.publicKey, "0.7.2", kPrefix, kLinuxDebProduct),
                    UpdateError);
    r = linuxRelease(kLinuxDebProduct);
    r.url = "https://evil.example/redvaultmessages_0.7.3_amd64.deb";
    CHECK_THROWS_AS(verifyManifest(signManifest(r, keys.secretKey), keys.publicKey, "0.7.2", kPrefix, kLinuxDebProduct),
                    UpdateError);
}

TEST_CASE("Linux updates only move forward", "[update][linux]") {
    const auto keys = generateSigningKey();
    CHECK_FALSE(verifyManifest(signManifest(linuxRelease(kLinuxDebProduct, "0.7.2"), keys.secretKey), keys.publicKey, "0.7.2",
                               kPrefix, kLinuxDebProduct));
    CHECK_FALSE(verifyManifest(signManifest(linuxRelease(kLinuxAppImageProduct, "0.7.1"), keys.secretKey), keys.publicKey,
                               "0.7.2", kPrefix, kLinuxAppImageProduct));
}

TEST_CASE("the Linux manifest is fetched over the same route as the Windows one", "[update][linux][tor]") {
    const auto keys = generateSigningKey();
    const std::string onionApi = "http://" + std::string(56, 'a') + ".onion/redvaultmessages/api/";
    const std::string manifestUrl = kPrefix + "linux-deb-latest.json";
    test::FakeTransport transport;
    transport.enqueue(200, signManifest(linuxRelease(kLinuxDebProduct), keys.secretKey), {{"content-type", "application/json"}});
    UpdateSource source{manifestUrl, keys.publicKey, "0.7.2", kPrefix, onionApi, true, 0};
    source.product = std::string(kLinuxDebProduct);
    const auto check = fetchManifest(transport, source);
    REQUIRE(check.release);
    CHECK(check.release->version == "0.7.3");
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests[0].url == "http://" + std::string(56, 'a') + ".onion/redvaultmessages/desktop/linux-deb-latest.json");
}
