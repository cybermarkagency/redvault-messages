// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Public share links (docs/public-share-links.md), pinned to the web app with
// vectors its own code produced (vectors/make_share_vectors.ts): a link made
// in any app opens in every other, the tokens the server checks are the same,
// and the owner's sealed copy, items and uploads open anywhere.
#include <catch2/catch_test_macros.hpp>

#include <sodium.h>

#include <fstream>

#include <nlohmann/json.hpp>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/call_crypto.hpp"
#include "redvault/crypto/share_crypto.hpp"
#include "test_helpers.hpp"

using namespace redvault;
using redvault::crypto::Bytes;
using redvault::test::b64;
using redvault::test::copy;
using Json = nlohmann::json;
namespace share = redvault::crypto::share;

namespace {

const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_SHARE_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

/// The app normalises passwords to NFC (QString::normalized) before they reach the core; the vectors keep one decomposed on purpose.
std::string nfc(const std::string& password) {
    // "cafe" + U+0301 COMBINING ACUTE -> "caf" + U+00E9.
    const std::string decomposed = "cafe\xCC\x81";
    const std::string composed = "caf\xC3\xA9";
    std::string out = password;
    if (const auto at = out.find(decomposed); at != std::string::npos) out.replace(at, decomposed.size(), composed);
    return out;
}

}  // namespace

TEST_CASE("share keys match the web app: lookup, access and wrap, with and without a password", "[share][crypto][vectors]") {
    for (const auto& c : vectors().at("keys")) {
        const auto secret = crypto::calls::linkSecretFromFragment(c.at("secret").get<std::string>());
        REQUIRE(secret);
        std::optional<share::PasswordParams> params;
        std::optional<std::string> password;
        if (!c.at("params").is_null()) {
            params = share::PasswordParams{b64(c.at("params").at("salt").get<std::string>()), c.at("params").at("ops").get<unsigned long long>(),
                                           c.at("params").at("mem").get<std::size_t>()};
            password = nfc(c.at("password").get<std::string>());
        }
        const auto keys = share::deriveKeys(secret->view(), password ? std::optional<std::string_view>(*password) : std::nullopt, params);
        CHECK(crypto::base64Encode(keys.lookup.view()) == c.at("lookup").get<std::string>());
        CHECK(crypto::base64Encode(keys.access.view()) == c.at("access").get<std::string>());
        CHECK(crypto::base64Encode(keys.wrap.view()) == c.at("wrap").get<std::string>());
        CHECK(share::verifierOf(keys.lookup.view()) == c.at("lookupVerifier").get<std::string>());
        CHECK(share::verifierOf(keys.access.view()) == c.at("accessVerifier").get<std::string>());
    }
}

TEST_CASE("a wrong password gives other keys, and silly Argon2id costs are refused", "[share][crypto]") {
    const auto secret = share::newLinkSecret();
    const share::PasswordParams params{Bytes(16, 1), 2, 64ull * 1024 * 1024};
    const auto right = share::deriveKeys(secret.view(), std::string_view("correct horse"), params);
    const auto wrong = share::deriveKeys(secret.view(), std::string_view("correct hors"), params);
    CHECK(copy(right.lookup) == copy(wrong.lookup));
    CHECK(copy(right.access) != copy(wrong.access));
    CHECK(copy(right.wrap) != copy(wrong.wrap));
    CHECK_THROWS_AS(share::deriveKeys(secret.view(), std::nullopt, params), crypto::CryptoError);
    CHECK_THROWS_AS(share::deriveKeys(secret.view(), std::string_view("x"), share::PasswordParams{Bytes(16, 1), 1, 64ull * 1024 * 1024}),
                    crypto::CryptoError);
    CHECK_THROWS_AS(share::deriveKeys(secret.view(), std::string_view("x"), share::PasswordParams{Bytes(16, 1), 3, 1024ull * 1024 * 1024}),
                    crypto::CryptoError);
}

TEST_CASE("web-made envelopes open here, and nothing altered does", "[share][crypto][vectors]") {
    for (const auto& c : vectors().at("envelopes")) {
        const auto wrap = b64(c.at("wrap").get<std::string>());
        auto ciphertext = b64(c.at("ciphertext").get<std::string>());
        const auto nonce = b64(c.at("nonce").get<std::string>());
        const auto opened = share::openEnvelope(wrap, ciphertext, nonce);
        REQUIRE(opened);
        CHECK(crypto::base64Encode(opened->key) == c.at("key").get<std::string>());
        CHECK(opened->root == c.at("root").get<std::string>());
        if (c.at("upload").is_null()) {
            CHECK_FALSE(opened->upload);
        } else {
            REQUIRE(opened->upload);
            CHECK(crypto::base64Encode(*opened->upload) == c.at("upload").get<std::string>());
        }
        ciphertext[3] ^= 1;
        CHECK_FALSE(share::openEnvelope(wrap, ciphertext, nonce));
        // And one sealed here opens with the same key.
        const auto resealed = share::sealEnvelope(wrap, *opened);
        CHECK(resealed.ciphertext.size() % 128 == 16);
        CHECK(share::openEnvelope(wrap, resealed.ciphertext, resealed.nonce)->root == opened->root);
    }
}

TEST_CASE("web-made items open here under their own id only", "[share][crypto][vectors]") {
    for (const auto& c : vectors().at("items")) {
        const auto key = b64(c.at("shareKey").get<std::string>());
        const auto id = c.at("itemId").get<std::string>();
        const auto ciphertext = b64(c.at("ciphertext").get<std::string>());
        const auto nonce = b64(c.at("nonce").get<std::string>());
        const auto meta = share::openItem(key, id, ciphertext, nonce);
        REQUIRE(meta);
        CHECK(*meta == c.at("meta"));
        CHECK_FALSE(share::openItem(key, id + "x", ciphertext, nonce));
        const auto here = share::sealItem(key, id, *meta);
        CHECK(*share::openItem(key, id, here.ciphertext, here.nonce) == c.at("meta"));
    }
}

TEST_CASE("malformed item metadata is refused even when it decrypts", "[share][crypto]") {
    const auto key = share::newShareKey();
    Json file{{"v", 1},           {"kind", "file"},   {"name", "a.txt"}, {"parent", ""},     {"mime", "text/plain"}, {"size", 3},
              {"modified", 1},    {"key", "AAAA"},    {"header", "BBBB"}, {"chunk", 65536}, {"sha", std::string(64, 'a')}};
    const auto check = [&](const Json& meta) {
        const auto sealed = share::sealItem(key, "i", meta);
        return share::openItem(key, "i", sealed.ciphertext, sealed.nonce).has_value();
    };
    CHECK(check(file));
    auto bad = file;
    bad["sha"] = "nothex";
    CHECK_FALSE(check(bad));
    bad = file;
    bad["size"] = -1;
    CHECK_FALSE(check(bad));
    bad = file;
    bad["kind"] = "script";
    CHECK_FALSE(check(bad));
    bad = file;
    bad["v"] = 2;
    CHECK_FALSE(check(bad));
}

TEST_CASE("the owner's sealed copy opens with their Files key", "[share][crypto][vectors]") {
    const auto& o = vectors().at("ownerBox");
    const auto box = share::openOwnerBox(b64(o.at("driveKey").get<std::string>()), b64(o.at("ciphertext").get<std::string>()),
                                         b64(o.at("nonce").get<std::string>()));
    REQUIRE(box);
    CHECK(box->secret == o.at("box").at("secret").get<std::string>());
    CHECK(box->key == o.at("box").at("key").get<std::string>());
    CHECK(box->root == o.at("box").at("root").get<std::string>());
    CHECK(box->label == o.at("box").at("label").get<std::string>());
    CHECK(box->created == o.at("box").at("created").get<std::int64_t>());
    CHECK_FALSE(share::openOwnerBox(Bytes(32, 9), b64(o.at("ciphertext").get<std::string>()), b64(o.at("nonce").get<std::string>())));
}

TEST_CASE("upload key pairs and web-sealed uploads match", "[share][crypto][vectors]") {
    for (const auto& k : vectors().at("uploadKeys")) {
        const auto pair = share::uploadKeyPair(b64(k.at("driveKey").get<std::string>()), b64(k.at("salt").get<std::string>()));
        CHECK(crypto::base64Encode(pair.publicKey) == k.at("publicKey").get<std::string>());
        CHECK(crypto::base64Encode(pair.privateKey.view()) == k.at("privateKey").get<std::string>());
    }
    const auto& c = vectors().at("contribution");
    const share::KeyPair pair{b64(c.at("publicKey").get<std::string>()), crypto::SecretBytes::copyOf(b64(c.at("privateKey").get<std::string>()))};
    const auto meta = share::openContribution(pair, b64(c.at("sealed").get<std::string>()));
    REQUIRE(meta);
    CHECK(*meta == c.at("meta"));
    const auto other = share::uploadKeyPair(Bytes(32, 1), Bytes(16, 2));
    CHECK_FALSE(share::openContribution(other, b64(c.at("sealed").get<std::string>())));
}

TEST_CASE("links carry the secret only in the fragment, as the web app writes them", "[share][crypto][vectors]") {
    const auto& u = vectors().at("url");
    const auto secret = crypto::calls::linkSecretFromFragment(u.at("secret").get<std::string>());
    REQUIRE(secret);
    CHECK(share::shareUrl(u.at("webBase").get<std::string>(), u.at("publicId").get<std::string>(), secret->view()) == u.at("url").get<std::string>());
    // "{}" padded to one 128-byte bucket, as JSON.stringify + spaces does on the web.
    const auto padded = share::padJson(Json::object(), 128);
    CHECK(padded.size() == 128);
    CHECK(std::string(padded.begin(), padded.begin() + 3) == "{} ");
}
