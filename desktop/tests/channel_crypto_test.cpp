// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Private channel links and channel posts forwarded into chats, pinned to
// the web app with vectors its own code produced
// (vectors/make_channel_vectors.ts): a private link made in any app opens in
// every other, the access token the server checks is the same, the owner's
// sealed secret opens anywhere, and a forwarded post is byte-identical inside
// the encrypted message.
#include <catch2/catch_test_macros.hpp>

#include <sodium.h>

#include <fstream>

#include <nlohmann/json.hpp>

#include "test_helpers.hpp"
#include "redvault/crypto/channel_crypto.hpp"
#include "redvault/model/channels.hpp"
#include "redvault/model/rich_message.hpp"

using namespace redvault;
using redvault::crypto::Bytes;
using redvault::test::b64;
using Json = nlohmann::json;
namespace links = redvault::crypto::channels;
namespace rich_message = redvault::model::rich_message;

namespace {

const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_CHANNEL_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

/// What the web app's parseStructured gave, in its own JSON shape.
Json asWebJson(const model::ParsedBody& body) {
    if (const auto* text = std::get_if<model::parsed::Text>(&body)) {
        Json out{{"kind", "text"}, {"text", text->text}};
        if (text->forwarded) out["forwarded"] = true;
        if (text->channelPost) {
            out["channelPost"] = Json{{"channelId", text->channelPost->channelId},
                                      {"postId", text->channelPost->postId},
                                      {"channelName", text->channelPost->channelName}};
        }
        return out;
    }
    if (std::holds_alternative<model::parsed::Invalid>(body)) return Json{{"kind", "invalid"}};
    return Json{{"kind", "reaction"}};
}

}  // namespace

TEST_CASE("a private link's access token is the web app's", "[channels][crypto][vectors]") {
    for (const auto& t : vectors()["tokens"]) {
        INFO(t["secret"].get<std::string>());
        CHECK(links::accessToken(t["secret"].get<std::string>()) == t["access"].get<std::string>());
    }
    // Only a real secret makes one (the web app's would throw too).
    CHECK_THROWS_AS(links::accessToken("not a secret"), crypto::CryptoError);
    CHECK_THROWS_AS(links::accessToken("#" + vectors()["tokens"][0]["secret"].get<std::string>()), crypto::CryptoError);
}

TEST_CASE("the secret is read from a link's fragment as the web app reads it", "[channels][crypto][vectors]") {
    for (const auto& c : vectors()["fragmentCases"]) {
        const auto fragment = c["fragment"].get<std::string>();
        INFO("fragment: " << fragment);
        const auto mine = links::secretFromFragment(fragment);
        if (c["secret"].is_null()) {
            CHECK_FALSE(mine);
        } else {
            REQUIRE(mine);
            CHECK(*mine == c["secret"].get<std::string>());
        }
    }
}

TEST_CASE("a new secret is 32 random bytes in base64url", "[channels][crypto]") {
    const std::string a = links::newSecret();
    const std::string b = links::newSecret();
    CHECK(a.size() == 43);
    CHECK(a != b);
    CHECK(links::secretFromFragment(a) == a);
    CHECK(links::accessToken(a) == links::accessToken(a));
    CHECK(links::accessToken(a).find(a) == std::string::npos);
}

TEST_CASE("the owner's sealed secret opens with their key, and only theirs", "[channels][crypto][vectors]") {
    const auto& v = vectors();
    const Bytes ownerPublic = b64(v["owner"]["publicKey"].get<std::string>());
    const Bytes ownerPrivate = b64(v["owner"]["privateKey"].get<std::string>());
    const Bytes otherPublic = b64(v["other"]["publicKey"].get<std::string>());
    const Bytes otherPrivate = b64(v["other"]["privateKey"].get<std::string>());
    const auto secret = v["sealed"]["secret"].get<std::string>();
    const auto sealedByWeb = v["sealed"]["sealed"].get<std::string>();

    // Sealed by the web app, opened here.
    CHECK(links::openSecret(sealedByWeb, ownerPublic, ownerPrivate) == secret);
    CHECK_FALSE(links::openSecret(sealedByWeb, otherPublic, otherPrivate));
    CHECK_FALSE(links::openSecret(std::nullopt, ownerPublic, ownerPrivate));
    CHECK_FALSE(links::openSecret(std::string("not base64!"), ownerPublic, ownerPrivate));

    // Sealed here: what's inside is the base64url text itself, as the web app seals (and later opens) it.
    const std::string sealedHere = links::sealSecret(secret, ownerPublic);
    CHECK(sealedHere != sealedByWeb);  // a fresh ephemeral key every time
    const Bytes box = b64(sealedHere);
    REQUIRE(box.size() == secret.size() + crypto_box_SEALBYTES);
    std::string inside(secret.size(), '\0');
    REQUIRE(crypto_box_seal_open(reinterpret_cast<unsigned char*>(inside.data()), box.data(), box.size(), ownerPublic.data(),
                                 ownerPrivate.data()) == 0);
    CHECK(inside == secret);
    CHECK(links::openSecret(sealedHere, ownerPublic, ownerPrivate) == secret);

    // Something sealed that isn't a secret is no use to anyone.
    CHECK_FALSE(links::openSecret(links::sealSecret("hello", ownerPublic), ownerPublic, ownerPrivate));
}

TEST_CASE("channel links come out as the web app makes them", "[channels][vectors]") {
    const auto& v = vectors();
    const auto base = v["webBase"].get<std::string>();
    const auto id = v["channelId"].get<std::string>();
    const auto secret = v["sealed"]["secret"].get<std::string>();
    CHECK(model::channels::channelUrl(base, id) == v["publicUrl"].get<std::string>());
    CHECK(model::channels::channelUrl(base, id, secret) == v["privateUrl"].get<std::string>());
    CHECK(model::channels::parseChannelLink(v["privateUrl"].get<std::string>()) == model::channels::ChannelLink{id, secret});
    CHECK(model::channels::parseChannelLink(v["publicUrl"].get<std::string>()) == model::channels::ChannelLink{id, std::nullopt});
}

TEST_CASE("a forwarded channel post is byte-identical to the web app's", "[channels][format][rich][vectors]") {
    for (const auto& f : vectors()["forwards"]) {
        const model::ChannelPostRef ref{f["ref"]["channelId"].get<std::string>(), f["ref"]["postId"].get<std::string>(),
                                        f["ref"]["channelName"].get<std::string>()};
        CHECK(rich_message::serializeChannelForward(f["text"].get<std::string>(), ref) == f["body"].get<std::string>());
    }
}

TEST_CASE("forwarded channel posts parse as the web app parses them", "[channels][format][rich][vectors]") {
    for (const auto& c : vectors()["parseCases"]) {
        const auto body = c["body"].get<std::string>();
        INFO(body);
        CHECK(asWebJson(rich_message::parse(body)) == c["parsed"]);
    }
}
