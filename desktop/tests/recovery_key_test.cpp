// Pinned to the same vector as ui/src/test/recoveryKey.test.ts and Android's
// RecoveryKeyTest, so a key made on one app works on the others.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <regex>

#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/recovery_key.hpp"
#include "test_helpers.hpp"

using namespace redvault::crypto;
using redvault::test::copy;

namespace {

Bytes fixedKey() {
    Bytes key(32);
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>((i * 13 + 5) & 0xff);
    return key;
}

}  // namespace

TEST_CASE("derives the web client's vector", "[recovery][compat]") {
    CHECK(recovery::format(fixedKey()) == "AUJB-6LBZ-IZJW-A3L2-Q6KK-DLV3-ZDK6-F374-BELC-GMB5-JJLW-I4L6-ROMA");
    CHECK(base64Encode(recovery::wrapKey(fixedKey())) == "hNkdtJ9WVWt9NeRT9e0PbW25zE/F7VmLxjWUqY12lwM=");
    CHECK(recovery::authFor(fixedKey()) == "7N3QawoTizJf9BpyLEIA7jfSfleT9oISJi8CM8b1pSI=");
}

TEST_CASE("formats and parses forgivingly", "[recovery]") {
    SecretBytes key = recovery::generate();
    const std::string text = recovery::format(key);
    CHECK(std::regex_match(text, std::regex("^([A-Z2-7]{4}-){12}[A-Z2-7]{4}$")));

    auto parsed = recovery::parse(text);
    REQUIRE(parsed);
    CHECK(copy(*parsed) == copy(key));

    std::string loose = text;
    std::ranges::transform(loose, loose.begin(), [](unsigned char c) {
        return c == '-' ? ' ' : static_cast<char>(std::tolower(c));
    });
    auto parsedLoose = recovery::parse(loose);
    REQUIRE(parsedLoose);
    CHECK(copy(*parsedLoose) == copy(key));

    auto lookalike = recovery::parse("AUJB-6LBZ-IZJW-A3L2-Q6KK-DLV3-ZDK6-F374-BELC-GMB5-JJLW-I4L6-R0MA");
    REQUIRE(lookalike);
    CHECK(copy(*lookalike) == fixedKey());

    CHECK_FALSE(recovery::parse("too-short"));
    // Non-zero spare bits: a different last character decodes to the same bytes but isn't canonical.
    CHECK_FALSE(recovery::parse("AUJB-6LBZ-IZJW-A3L2-Q6KK-DLV3-ZDK6-F374-BELC-GMB5-JJLW-I4L6-ROMB"));
}

TEST_CASE("unwraps only for the right key and account", "[recovery]") {
    KeyPair pair = generateKeyPair();
    SecretBytes key = recovery::generate();
    recovery::RecoveryMaterial m = recovery::materialFor(key, pair.privateKey);
    const std::string publicB64 = base64Encode(pair.publicKey);
    CHECK(copy(recovery::unwrap(key, m.recoveryEncryptedPrivateKey, m.recoveryPrivateKeyNonce, publicB64)) ==
          copy(pair.privateKey));

    const std::string other = base64Encode(generateKeyPair().publicKey);
    CHECK_THROWS_AS(recovery::unwrap(key, m.recoveryEncryptedPrivateKey, m.recoveryPrivateKeyNonce, other),
                    CryptoError);
    CHECK_THROWS_AS(
        recovery::unwrap(recovery::generate(), m.recoveryEncryptedPrivateKey, m.recoveryPrivateKeyNonce, publicB64),
        CryptoError);
}
