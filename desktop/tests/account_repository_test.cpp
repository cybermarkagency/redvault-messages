#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "fake_transport.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/recovery_key.hpp"
#include "redvault/data/account_repository.hpp"
#include "test_helpers.hpp"

using namespace redvault;
using namespace redvault::data;
using api::Json;
using redvault::test::copy;
using redvault::test::FakeTransport;

namespace {

struct Fixture {
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::shared_ptr<api::ApiClient> api = std::make_shared<api::ApiClient>(
        "https://example.com/api/", transport, std::make_shared<net::SessionCookieJar>());
    AccountRepository account{api};
    crypto::KeyPair keys = crypto::generateKeyPair();
    SelfIdentity me{.handle = "alice",
                    .userId = "u1",
                    .publicKey = keys.publicKey,
                    .privateKey = crypto::SecretBytes::copyOf(keys.privateKey),
                    .role = "user",
                    .subscriptionTier = "free"};

    Json lastBody() const { return Json::parse(transport->last().body); }
};

}  // namespace

TEST_CASE("changing the password re-wraps the same private key under the new one", "[account]") {
    Fixture f;
    const Json pre{{"kdfSalt", "AAAAAAAAAAAAAAAAAAAAAA=="}, {"authVersion", 1}, {"upgrade", false}};
    f.transport->enqueue(200, pre.dump());
    f.transport->enqueue(200, pre.dump());
    f.transport->enqueue(204, "");
    f.account.changePassword(f.me, "old password", "new password");

    CHECK(f.transport->last().method == "PATCH");
    CHECK(f.transport->last().url == "https://example.com/api/auth/password");
    const auto body = f.lastBody();
    CHECK(body["currentPassword"] == "old password");
    CHECK(body["newPassword"] == "new password");
    const auto unwrapped = crypto::unwrapPrivateKey(
        crypto::WrappedPrivateKey{crypto::base64Decode(body["encryptedPrivateKey"].get<std::string>()),
                                  crypto::base64Decode(body["privateKeyNonce"].get<std::string>()),
                                  crypto::base64Decode(body["privateKeyKdfSalt"].get<std::string>())},
        "new password");
    CHECK(copy(unwrapped) == copy(f.keys.privateKey));
}

TEST_CASE("a new recovery key unlocks the account's private key", "[account][recovery]") {
    Fixture f;
    f.transport->enqueue(200, R"({"kdfSalt":"AAAAAAAAAAAAAAAAAAAAAA==","authVersion":1,"upgrade":false})");
    f.transport->enqueue(204, "");
    const auto formatted = f.account.createRecoveryKey(f.me, "password");

    const auto body = f.lastBody();
    CHECK(f.transport->last().url == "https://example.com/api/auth/recovery");
    CHECK(body["currentPassword"] == "password");
    const auto key = crypto::recovery::parse(formatted);
    REQUIRE(key);
    CHECK(body["recoveryAuth"] == crypto::recovery::authFor(*key));
    const auto unwrapped = crypto::recovery::unwrap(*key, body["recoveryEncryptedPrivateKey"].get<std::string>(),
                                                    body["recoveryPrivateKeyNonce"].get<std::string>(),
                                                    crypto::base64Encode(f.keys.publicKey));
    CHECK(copy(unwrapped) == copy(f.keys.privateKey));
}

TEST_CASE("two-factor setup, confirm and disable hit the right endpoints", "[account]") {
    Fixture f;
    f.transport->enqueue(200, R"({"secretBase32":"JBSWY3DPEHPK3PXP","otpauthUri":"otpauth://totp/Redvault:alice?secret=JBSWY3DPEHPK3PXP"})");
    CHECK(f.account.startMfaSetup().secretBase32 == "JBSWY3DPEHPK3PXP");
    f.transport->enqueue(204, "");
    f.account.confirmMfa("123456");
    CHECK(f.lastBody() == Json{{"code", "123456"}});
    f.transport->enqueue(200, R"({"kdfSalt":"AAAAAAAAAAAAAAAAAAAAAA==","authVersion":1,"upgrade":false})");
    f.transport->enqueue(204, "");
    f.account.disableMfa(f.me, "pw");
    CHECK(f.transport->last().url == "https://example.com/api/auth/mfa/disable");
    CHECK(f.lastBody() == Json{{"password", "pw"}});
    f.transport->enqueue(204, "");
    f.account.setAcceptMessageRequests(false);
    CHECK(f.transport->last().body == R"({"acceptMessageRequests":false})");
}
