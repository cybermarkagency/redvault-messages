// The real VaultSession against a fake server: the page gets the count of items left out.
#include <catch2/catch_test_macros.hpp>

#include <QSettings>

#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "app_test_support.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/vault_crypto.hpp"
#include "redvault/net/cookie_jar.hpp"
#include "vault_session.hpp"

using namespace redvault;
using Json = nlohmann::json;
using redvault::test::qt;
using redvault::test::settle;
using redvault::test::waitFor;

namespace {

const std::string kBase = "https://example.com/api/";
const std::string kFirst = "10000000-0000-4000-8000-000000000001";
const std::string kSecond = "20000000-0000-4000-8000-000000000002";

/// VaultSession calls from its worker threads.
class VaultServer final : public net::HttpTransport {
public:
    net::HttpResponse send(const net::HttpRequest& request) override {
        std::lock_guard lock(mutex);
        const std::string path = request.url.substr(kBase.size());
        if (path == "vault/items") return {200, {}, Json{{"items", items}}.dump()};
        if (path == "vault/shares") return {200, {}, Json{{"shares", Json::array()}}.dump()};
        if (path == "vault/shared-with-me") return {200, {}, Json{{"items", Json::array()}}.dump()};
        return {404, {}, Json{{"error", "not_found"}, {"message", "not_found"}}.dump()};
    }
    std::mutex mutex;
    Json items = Json::array();
};

struct Fixture {
    crypto::KeyPair pair = crypto::generateKeyPair();
    std::shared_ptr<const data::SelfIdentity> identity = std::make_shared<const data::SelfIdentity>(data::SelfIdentity{
        .handle = "h", .userId = "a0000000-0000-4000-8000-000000000001", .publicKey = pair.publicKey, .privateKey = crypto::SecretBytes::copyOf(pair.privateKey),
        .role = "user", .subscriptionTier = "free"});
    std::shared_ptr<VaultServer> server = std::make_shared<VaultServer>();
    std::shared_ptr<api::ApiClient> api = std::make_shared<api::ApiClient>(kBase, server, std::make_shared<net::SessionCookieJar>());

    Fixture() {
        qt();
        QSettings().clear();
    }
    ~Fixture() { QSettings().clear(); }

    Json item(const std::string& id, const std::string& title, const std::string& boundId) const {
        const Json data{{"type", "login"}, {"title", title}, {"boundId", boundId}, {"rev", 1}};
        const auto key = crypto::vault::vaultKey(identity->privateKey.view());
        const auto sealed = crypto::vault::sealItem(data.dump(), key.view());
        return Json{{"id", id}, {"ciphertext", crypto::base64Encode(sealed.ciphertext)}, {"nonce", crypto::base64Encode(sealed.nonce)}, {"revision", 1}};
    }
};

}  // namespace

TEST_CASE("the vault page says how many items it left out", "[app][vault]") {
    Fixture f;
    f.server->items = Json::array({f.item(kFirst, "Moved here", kSecond), f.item(kSecond, "In place", kSecond)});
    app::VaultSession vault(f.api, f.identity, nullptr);
    REQUIRE(waitFor([&] { return vault.loaded(); }));
    CHECK(vault.refused() == 1);
    CHECK(vault.property("refused").toInt() == 1);
    CHECK(vault.items().size() == 1);
    settle(20);
}

TEST_CASE("control: nothing left out, nothing said", "[app][vault]") {
    Fixture f;
    f.server->items = Json::array({f.item(kFirst, "One", kFirst), f.item(kSecond, "Two", kSecond)});
    app::VaultSession vault(f.api, f.identity, nullptr);
    REQUIRE(waitFor([&] { return vault.loaded(); }));
    CHECK(vault.refused() == 0);
    CHECK(vault.items().size() == 2);
    settle(20);
}
