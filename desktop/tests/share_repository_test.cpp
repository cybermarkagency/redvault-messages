// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The desktop's side of public share links (data/share_repository.hpp):
// what a folder link shows as Files changes, and what goes to the server
// when a link is made (only verifiers and ciphertext, never a name).
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>

#include "fake_transport.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/call_crypto.hpp"
#include "redvault/crypto/drive_crypto.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/share_crypto.hpp"
#include "redvault/data/share_repository.hpp"
#include "test_helpers.hpp"

using namespace redvault;
using namespace redvault::data;
using api::Json;
using redvault::test::FakeTransport;

namespace {

DriveEntry folder(const std::string& id, const std::string& name, const std::string& parent) {
    DriveEntry e;
    e.id = id;
    e.kind = "folder";
    e.name = name;
    e.parent = parent;
    e.meta = Json{{"v", 1}, {"kind", "folder"}, {"name", name}, {"parent", parent}};
    return e;
}

DriveEntry file(const std::string& id, const std::string& name, const std::string& parent, const std::string& fileId = "", int version = 1) {
    DriveEntry e;
    e.id = id;
    e.kind = "file";
    e.name = name;
    e.parent = parent;
    e.mimeType = "text/plain";
    e.size = 10;
    e.modifiedMs = 1000 + version;
    e.fileId = fileId.empty() ? id : fileId;
    e.version = version;
    e.meta = Json{{"key", "a2V5"}, {"header", "aGVhZGVy"}, {"chunk", 65536}, {"sha", std::string(64, 'b')}};
    return e;
}

const ShareItemPlan* byBackup(const std::vector<ShareItemPlan>& plan, const std::string& id) {
    for (const auto& p : plan) {
        if (p.backupId == id) return &p;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("a folder link shows the folder and what's in it: newest versions, nothing trashed, locked or outside", "[share][plan]") {
    auto locked = file("f-locked", "locked.txt", "root");
    locked.meta = Json{{"lock", Json::object()}};
    auto trashed = file("f-trash", "old.txt", "root");
    trashed.trashed = true;
    const std::vector<DriveEntry> entries{
        folder("root", "Project", ""),
        folder("sub", "Drafts", "root"),
        folder("other", "Private", ""),
        file("f1-v1", "report.txt", "root", "f1", 1),
        file("f1-v2", "report.txt", "root", "f1", 2),
        file("f2", "notes.txt", "sub"),
        file("f3", "secret.txt", "other"),
        locked,
        trashed,
    };
    const auto plan = ShareRepository::plan("root", "ROOT-ITEM", entries, {});
    std::set<std::string> ids;
    for (const auto& p : plan) ids.insert(p.backupId);
    CHECK(ids == std::set<std::string>{"root", "sub", "f1-v2", "f2"});
    CHECK(byBackup(plan, "root")->itemId == "ROOT-ITEM");
    CHECK(byBackup(plan, "root")->meta.at("parent") == "");
    CHECK(byBackup(plan, "sub")->meta.at("parent") == "ROOT-ITEM");
    CHECK(byBackup(plan, "f2")->meta.at("parent") == byBackup(plan, "sub")->itemId);
    CHECK(byBackup(plan, "f1-v2")->meta.at("key") == "a2V5");
    CHECK_FALSE(byBackup(plan, "f1-v2")->meta.contains("fileId"));

    // Item ids survive: by object, and across a new version of the same file.
    const std::vector<api::shares::OwnerItem> existing{{"ROOT-ITEM", "root", "folder", "", ""},
                                                       {"SUB-ITEM", "sub", "folder", "", ""},
                                                       {"REPORT-ITEM", "f1-v1", "file", "", ""}};
    const auto again = ShareRepository::plan("root", "ROOT-ITEM", entries, existing);
    CHECK(byBackup(again, "sub")->itemId == "SUB-ITEM");
    CHECK(byBackup(again, "f1-v2")->itemId == "REPORT-ITEM");
}

TEST_CASE("a file link is just that file, and nothing once it's in the Trash", "[share][plan]") {
    auto f = file("f1", "a.txt", "");
    CHECK(ShareRepository::plan("f1", "ITEM", {f}, {}).size() == 1);
    f.trashed = true;
    CHECK(ShareRepository::plan("f1", "ITEM", {f}, {}).empty());
    CHECK(ShareRepository::plan("missing", "ITEM", {}, {}).empty());
}

TEST_CASE("making a link sends verifiers and ciphertext only, then the items under the share key", "[share][repository]") {
    auto transport = std::make_shared<FakeTransport>();
    auto api = std::make_shared<api::ApiClient>("https://example.com/api/", transport, std::make_shared<net::SessionCookieJar>());
    const auto keys = crypto::generateKeyPair();
    auto identity = std::make_shared<SelfIdentity>(SelfIdentity{.handle = "alice",
                                                                .userId = "u1",
                                                                .publicKey = keys.publicKey,
                                                                .privateKey = crypto::SecretBytes::copyOf(keys.privateKey),
                                                                .role = "user",
                                                                .subscriptionTier = "free"});
    ShareRepository repo(api, identity, std::filesystem::temp_directory_path(), "https://www.example.test/redvaultmessages/");
    const auto driveKey = crypto::drive::driveKey(identity->privateKey.view());

    Json created;
    transport->handler = [&](const net::HttpRequest& request) -> net::HttpResponse {
        if (request.method == "POST" && request.url.ends_with("/shares")) {
            created = Json::parse(request.body);
            Json link{{"id", "L1"},
                      {"publicId", "AAAAAAAAAAAAAAAAAAAAAA"},
                      {"backupId", created["backupId"]},
                      {"kind", "folder"},
                      {"permission", created["permission"]},
                      {"keyEpoch", 1},
                      {"password", created["password"]},
                      {"envelope", created["envelope"]},
                      {"envelopeNonce", created["envelopeNonce"]},
                      {"ownerBox", created["ownerBox"]},
                      {"ownerBoxNonce", created["ownerBoxNonce"]},
                      {"uploadKeySalt", created["uploadKeySalt"]},
                      {"expiresAt", nullptr},
                      {"expired", false},
                      {"revoked", false},
                      {"maxDownloads", nullptr},
                      {"downloadCount", 0},
                      {"openCount", 0},
                      {"upload", Json{{"maxFileSize", nullptr}, {"allowance", nullptr}, {"maxFiles", 5}, {"count", 0}, {"bytes", 0}, {"inProgress", 0}}},
                      {"incoming", 0}};
            return {201, {}, link.dump()};
        }
        if (request.url.ends_with("/shares/L1/items")) return {200, {}, Json{{"stored", 3}}.dump()};
        return {404, {}, "{}"};
    };

    const std::vector<DriveEntry> entries{folder("root", "Project Files", ""), folder("sub", "Drafts", "root"), file("f2", "notes.txt", "sub")};
    ShareOptions options;
    options.permission = "contributor";
    options.label = "Team uploads";
    options.maxFiles = 5;
    const auto made = repo.create(entries[0], entries, options);

    // Nothing readable reaches the server.
    const std::string body = created.dump();
    for (const char* name : {"Project Files", "Drafts", "notes.txt", "Team uploads"}) CHECK(body.find(name) == std::string::npos);
    CHECK(created["password"].is_null());
    CHECK(created["upload"]["maxFiles"] == 5);
    REQUIRE(made.box);
    CHECK(made.box->label == "Team uploads");
    CHECK(made.url.starts_with("https://www.example.test/redvaultmessages/share/?id=AAAAAAAAAAAAAAAAAAAAAA#"));

    // The link's own keys open what was sent: the envelope (with the upload key) and every item.
    const auto secret = crypto::calls::linkSecretFromFragment(made.box->secret);
    REQUIRE(secret);
    const auto linkKeys = crypto::share::deriveKeys(secret->view(), std::nullopt, std::nullopt);
    CHECK(crypto::share::verifierOf(linkKeys.access.view()) == created["accessVerifier"].get<std::string>());
    const auto envelope = crypto::share::openEnvelope(linkKeys.wrap.view(), crypto::base64Decode(created["envelope"].get<std::string>()),
                                                      crypto::base64Decode(created["envelopeNonce"].get<std::string>()));
    REQUIRE(envelope);
    CHECK(envelope->root == made.box->root);
    REQUIRE(envelope->upload);
    CHECK(*envelope->upload == crypto::share::uploadKeyPair(driveKey.view(), crypto::base64Decode(created["uploadKeySalt"].get<std::string>())).publicKey);

    const auto& items = transport->requests.back();
    REQUIRE(items.url.ends_with("/shares/L1/items"));
    const auto sent = Json::parse(items.body);
    CHECK(sent["items"].size() == 3);
    std::set<std::string> names;
    for (const auto& i : sent["items"]) {
        const auto meta = crypto::share::openItem(envelope->key, i["itemId"].get<std::string>(), crypto::base64Decode(i["metaCiphertext"].get<std::string>()),
                                                  crypto::base64Decode(i["metaNonce"].get<std::string>()));
        REQUIRE(meta);
        names.insert(meta->at("name").get<std::string>());
    }
    CHECK(names == std::set<std::string>{"Project Files", "Drafts", "notes.txt"});
}
