// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Vault items bound to their record, mirroring android's VaultBindingTest: the server here is the attacker, and answers
// whatever a test says. Sealing is real.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/vault_crypto.hpp"
#include "redvault/data/key_trust.hpp"
#include "redvault/data/vault_repository.hpp"
#include "redvault/net/cookie_jar.hpp"

using namespace redvault;
using namespace redvault::data;
using crypto::base64Encode;
using Json = nlohmann::json;
using redvault::test::FakeTransport;

namespace {

const std::string kBase = "https://example.com/api/";
const std::string kSelf = "a0000000-0000-4000-8000-000000000001";
const std::string kFirst = "10000000-0000-4000-8000-000000000001";
const std::string kSecond = "20000000-0000-4000-8000-000000000002";
const std::string kFriend = "b0000000-0000-4000-8000-000000000002";

struct Item {
    std::string id, ciphertext, nonce;
    int revision = 1;
    Json dto() const { return Json{{"id", id}, {"ciphertext", ciphertext}, {"nonce", nonce}, {"revision", revision}}; }
};

struct Fixture {
    crypto::KeyPair pair = crypto::generateKeyPair();
    std::shared_ptr<const SelfIdentity> identity = std::make_shared<const SelfIdentity>(SelfIdentity{
        .handle = "h", .userId = kSelf, .publicKey = pair.publicKey, .privateKey = crypto::SecretBytes::copyOf(pair.privateKey), .role = "user", .subscriptionTier = "free"});
    crypto::KeyPair friendPair = crypto::generateKeyPair();
    std::string friendKey = base64Encode(friendPair.publicKey);
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::shared_ptr<api::ApiClient> api = std::make_shared<api::ApiClient>(kBase, transport, std::make_shared<net::SessionCookieJar>());
    std::shared_ptr<MemoryVaultRevs> revs = std::make_shared<MemoryVaultRevs>();
    std::shared_ptr<MemoryContactKeyPins> contacts = std::make_shared<MemoryContactKeyPins>();
    VaultRepository repository{api, identity, revs, contacts};

    std::vector<Item> items;
    Json sharedWithMe = Json{{"items", Json::array()}};
    Json shares = Json{{"shares", Json::array()}};
    std::vector<std::string> writes;
    int sharesWritten = 0;
    bool failSecondWrite = false;

    Fixture() {
        transport->handler = [this](const net::HttpRequest& r) { return handle(r); };
    }

    static net::HttpResponse json(const Json& body) { return {200, {}, body.dump()}; }

    net::HttpResponse handle(const net::HttpRequest& r) {
        const std::string path = r.url.substr(kBase.size());
        const Json body = r.body.empty() ? Json() : Json::parse(r.body);
        if (r.method == "GET" && path == "vault/items") {
            Json out = Json::array();
            for (const auto& item : items) out.push_back(item.dto());
            return json(Json{{"items", out}});
        }
        if (r.method == "GET" && path == "vault/shares") return json(shares);
        if (r.method == "GET" && path == "vault/shared-with-me") return json(sharedWithMe);
        if (r.method == "POST" && path == "vault/items") {
            writes.push_back("create");
            items.push_back(Item{kSecond, body["ciphertext"], body["nonce"], 1});
            return json(items.back().dto());
        }
        if (r.method == "PUT" && path.find("/shares/") != std::string::npos) {
            ++sharesWritten;
            return {204, {}, ""};
        }
        if (r.method == "PUT" && path.starts_with("vault/items/")) {
            if (failSecondWrite) return {503, {}, Json{{"error", "unavailable"}, {"message", "unavailable"}}.dump()};
            const std::string id = path.substr(12);
            writes.push_back("update");
            std::erase_if(items, [&](const Item& item) { return item.id == id; });
            items.push_back(Item{id, body["ciphertext"], body["nonce"], body["revision"].get<int>() + 1});
            return json(items.back().dto());
        }
        if (r.method == "GET" && path.starts_with("users/")) {
            return json(Json{{"userId", path.substr(6)}, {"username", "u"}, {"publicKey", friendKey}, {"createdAt", "2026-10-01T00:00:00.000Z"}});
        }
        return {404, {}, Json{{"error", "not_found"}, {"message", "not_found"}}.dump()};
    }

    Item seal(const std::string& id, const std::string& title, const Json& extra = Json::object()) const {
        Json data{{"type", "login"}, {"title", title}};
        data.update(extra);
        const auto key = crypto::vault::vaultKey(identity->privateKey.view());
        const auto sealed = crypto::vault::sealItem(data.dump(), key.view());
        return Item{id, base64Encode(sealed.ciphertext), base64Encode(sealed.nonce), 1};
    }

    Json shareCopy(const std::string& title, std::int64_t rev, const std::string& boundId = kFirst) const {
        const Json data{{"type", "login"}, {"title", title}, {"boundId", boundId}, {"rev", rev}};
        const auto sealed = crypto::vault::sealForRecipient(data.dump(), identity->publicKey);
        return Json{{"items", Json::array({Json{{"itemId", kFirst}, {"ownerId", kFriend}, {"sealed", base64Encode(sealed)}}})}};
    }

    std::vector<std::string> titles() {
        std::vector<std::string> out;
        for (const auto& entry : repository.list()) out.push_back(entry.data.value("title", ""));
        return out;
    }
};

Json bound(const std::string& id, std::int64_t rev) {
    return Json{{"boundId", id}, {"rev", rev}};
}

using Titles = std::vector<std::string>;

}  // namespace

TEST_CASE("control: an item from before binding still shows", "[vault][binding]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Old login")};
    CHECK(f.titles() == Titles{"Old login"});
    CHECK(f.repository.refused() == 0);
}

TEST_CASE("an item in another record's place is left out", "[vault][binding]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Moved here", bound(kSecond, 1)), f.seal(kSecond, "In place", bound(kSecond, 2))};
    CHECK(f.titles() == Titles{"In place"});
    CHECK(f.repository.refused() == 1);
}

TEST_CASE("a boundId that isn't text is left out too", "[vault][binding]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Odd", Json{{"boundId", 7}})};
    CHECK(f.titles().empty());
    CHECK(f.repository.refused() == 1);
}

TEST_CASE("the same id in capitals is the same record", "[vault][binding]") {
    Fixture f;
    const std::string upper = "ABCDEF00-0000-4000-8000-000000000001";
    const std::string lower = "abcdef00-0000-4000-8000-000000000001";
    f.items = {f.seal(upper, "Same", bound(lower, 1))};
    CHECK(f.titles() == Titles{"Same"});
    CHECK(f.revs->seen(kSelf, lower) == 1);
}

TEST_CASE("an older copy is left out, and so is an unbound one", "[vault][binding]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Current", bound(kFirst, 5))};
    CHECK(f.titles().size() == 1);
    f.items = {f.seal(kFirst, "Older", bound(kFirst, 4))};
    CHECK(f.titles().empty());
    CHECK(f.repository.refused() == 1);
    f.items = {f.seal(kFirst, "Unbound")};
    CHECK(f.titles().empty());
    CHECK(f.repository.refused() == 1);
}

TEST_CASE("the count goes back down when what was left out is back", "[vault][binding]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Current", bound(kFirst, 5))};
    f.titles();
    f.items = {f.seal(kFirst, "Older", bound(kFirst, 4))};
    f.titles();
    REQUIRE(f.repository.refused() == 1);
    f.items = {f.seal(kFirst, "Current", bound(kFirst, 5))};
    CHECK(f.titles() == Titles{"Current"});
    CHECK(f.repository.refused() == 0);
}

TEST_CASE("what this computer has seen is kept per account and lasts", "[vault][binding]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Current", bound(kFirst, 5))};
    f.titles();
    // The app restarted: a new repository, the same stored revisions.
    VaultRepository restarted(f.api, f.identity, f.revs, f.contacts);
    f.items = {f.seal(kFirst, "Older", bound(kFirst, 4))};
    CHECK(restarted.list().empty());
    CHECK(restarted.refused() == 1);
    CHECK(f.revs->seen("someone else", kFirst) == 0);
}

TEST_CASE("a new item is created, then bound to the id the server gave it", "[vault][binding]") {
    Fixture f;
    VaultEntry entry;
    entry.data = Json{{"type", "login"}, {"title", "New"}};
    const auto saved = f.repository.save(entry);
    CHECK(f.writes == Titles{"create", "update"});
    CHECK(saved.id == kSecond);
    CHECK(saved.data["boundId"] == kSecond);
    CHECK(saved.data["rev"] == 1);
    CHECK(saved.revision == 2);
    CHECK(f.titles() == Titles{"New"});
    CHECK(f.repository.refused() == 0);
}

TEST_CASE("a new item survives its second write failing, and binds at its next save", "[vault][binding]") {
    Fixture f;
    VaultEntry entry;
    entry.data = Json{{"type", "login"}, {"title", "New"}};
    f.failSecondWrite = true;
    auto saved = f.repository.save(entry);
    CHECK(saved.id == kSecond);
    CHECK_FALSE(saved.data.contains("boundId"));
    CHECK(f.titles() == Titles{"New"});
    f.failSecondWrite = false;
    saved = f.repository.save(saved);
    CHECK(saved.data["boundId"] == kSecond);
    CHECK(saved.data["rev"] == 1);
}

TEST_CASE("a shared item can't go back to an older copy either", "[vault][binding]") {
    Fixture f;
    f.sharedWithMe = f.shareCopy("Shared now", 3);
    CHECK(f.titles() == Titles{"Shared now"});
    f.sharedWithMe = f.shareCopy("Shared before", 2);
    CHECK(f.titles().empty());
    CHECK(f.repository.refused() == 1);
}

TEST_CASE("a shared copy keeps its own revision, under a separate name", "[vault][binding]") {
    Fixture f;
    f.sharedWithMe = f.shareCopy("Shared", 9);
    f.items = {f.seal(kFirst, "Mine", bound(kFirst, 2))};
    CHECK(f.titles().size() == 2);
    CHECK(f.repository.refused() == 0);
    CHECK(f.revs->seen(kSelf, kFirst) == 2);
    CHECK(f.revs->seen(kSelf, "shared:" + kFirst) == 9);
}

TEST_CASE("a shared copy in another item's place is left out", "[vault][binding]") {
    Fixture f;
    f.sharedWithMe = f.shareCopy("Moved", 1, kSecond);
    CHECK(f.titles().empty());
    CHECK(f.repository.refused() == 1);
}

TEST_CASE("saving an old item binds it and raises its revision", "[vault][binding]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Old login")};
    auto entries = f.repository.list();
    REQUIRE(entries.size() == 1);
    CHECK(f.writes.empty());  // Nothing rewritten just by reading it.
    entries[0].data["title"] = "Changed";
    const auto saved = f.repository.save(entries[0]);
    CHECK(saved.data["boundId"] == kFirst);
    CHECK(saved.data["rev"] == 1);
    CHECK(f.writes == Titles{"update"});
    CHECK(f.titles() == Titles{"Changed"});
    // What it was before the save can't come back now.
    f.items = {f.seal(kFirst, "Old login")};
    CHECK(f.titles().empty());
    CHECK(f.repository.refused() == 1);
}

TEST_CASE("each save raises the revision by one", "[vault][binding]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Item", bound(kFirst, 4))};
    auto entry = f.repository.list().at(0);
    entry = f.repository.save(entry);
    CHECK(entry.data["rev"] == 5);
    entry = f.repository.save(entry);
    CHECK(entry.data["rev"] == 6);
    CHECK(f.revs->seen(kSelf, kFirst) == 6);
}

TEST_CASE("fields this version doesn't know survive a save", "[vault][binding]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Item", Json{{"fromANewerVersion", Json{{"nested", 1}}}, {"tags", Json::array({"a", "b"})}})};
    auto entry = f.repository.list().at(0);
    entry.data["title"] = "Changed";
    const auto saved = f.repository.save(entry);
    CHECK(saved.data["fromANewerVersion"]["nested"] == 1);
    CHECK(saved.data["tags"] == Json::array({"a", "b"}));
    const auto reread = f.repository.list().at(0);
    CHECK(reread.data["fromANewerVersion"]["nested"] == 1);
    CHECK(reread.data["tags"] == Json::array({"a", "b"}));
    CHECK(reread.data["title"] == "Changed");
}

TEST_CASE("a rev that isn't a whole number counts as none", "[vault][binding]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Item", Json{{"rev", "9"}})};
    CHECK(f.titles() == Titles{"Item"});
    CHECK(f.revs->seen(kSelf, kFirst) == 0);
}

// --- Sharing ---

TEST_CASE("a new share refuses a changed key, and a control goes through", "[vault][seal]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Item", bound(kFirst, 1))};
    const auto entry = f.repository.list().at(0);
    SECTION("control") {
        REQUIRE_FALSE(f.contacts->keyChanged(kSelf, kFriend, f.friendKey));
        f.repository.share(entry, kFriend);
        CHECK(f.sharesWritten == 1);
    }
    SECTION("changed") {
        REQUIRE_FALSE(f.contacts->keyChanged(kSelf, kFriend, f.friendKey));
        f.friendKey = base64Encode(crypto::generateKeyPair().publicKey);
        CHECK_THROWS_MATCHES(f.repository.share(entry, kFriend), api::UserFacingError, Catch::Matchers::Message(KeyTrust::kContactKeyChanged));
        CHECK(f.sharesWritten == 0);
    }
}

TEST_CASE("a re-share skips someone whose key changed, and the save still goes through", "[vault][seal]") {
    Fixture f;
    f.items = {f.seal(kFirst, "Item", bound(kFirst, 1))};
    f.shares = Json{{"shares", Json::array({Json{{"itemId", kFirst}, {"recipientId", kFriend}}})}};
    auto entry = f.repository.list().at(0);
    REQUIRE(entry.sharedWith == Titles{kFriend});
    SECTION("control") {
        REQUIRE_FALSE(f.contacts->keyChanged(kSelf, kFriend, f.friendKey));
        const auto saved = f.repository.save(entry);
        CHECK(saved.data["rev"] == 2);
        CHECK(f.sharesWritten == 1);
    }
    SECTION("changed") {
        REQUIRE_FALSE(f.contacts->keyChanged(kSelf, kFriend, f.friendKey));
        f.friendKey = base64Encode(crypto::generateKeyPair().publicKey);
        const auto saved = f.repository.save(entry);
        CHECK(saved.data["rev"] == 2);
        CHECK(f.writes == Titles{"update"});
        CHECK(f.sharesWritten == 0);
    }
}
