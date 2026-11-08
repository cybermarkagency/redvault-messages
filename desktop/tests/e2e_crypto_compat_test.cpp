// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Pins the desktop crypto to the web client byte-for-byte. Every expected
// value below was produced by the web client's own libsodium build
// (libsodium-wrappers-sumo, the same calls as ui/src/lib/e2eCrypto.ts and
// commitment.ts), and matches android/.../crypto/E2eCryptoCompatTest.kt. If
// any of these fail, desktop users can't read web/Android users' messages or
// log in to their accounts.

#include <catch2/catch_test_macros.hpp>

#include "redvault/crypto/commitment.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "test_helpers.hpp"

using namespace redvault::crypto;
using redvault::test::b64;
using redvault::test::copy;
using redvault::test::joined;

namespace {

const std::string kPassword = "correct horse battery";
const Bytes kSalt = b64("AwoRGB8mLTQ7QklQV15lbA==");
const Bytes kWrapKey = b64("QcWwX65OHrfRNkusFLIAnSPV7csyDZixx/MIyO1XIgo=");
const Bytes kPublicKey = b64("sduaLGHdSyI5zX1H/c4hGwIcCzYEnx4pI6JTs1qCTw0=");
const Bytes kPrivateKey = b64("aXcRUXy911/1q83RboxQwY+QdYZyyE6fJW4Kc6oPNAo=");
const Bytes kWrapNonce = b64("BQwTGiEoLzY9REtSWWBnbnV8g4qRmJ+m");
const Bytes kEncryptedPrivateKey = b64("EIHl9Qlybjc5eUytEwggq2nbod3oMc/2JiwRII5xw5ZthVA/Vyy1zibp2FfL2sNz");
const Bytes kConversationKey = b64("Y2pxeH+GjZSboqmwt77FzNPa4ejv9v0ECxIZICcuNTw=");
const Bytes kSealedByWeb =
    b64("hut7Ysgml9JRk5VCtoXsXNeV+wtiaCLJebDC3LPziUN5jkmDeyVEVfJFZMQqcmU3YwP+O0J+WAvQgG0HFXFhYEkMuxPuogpGPZUSl5NuYn4=");
// "hello · wörld 🔐", spelled as UTF-8 bytes so the source encoding can't change it.
const std::string kMessagePlaintext = "hello \xC2\xB7 w\xC3\xB6rld \xF0\x9F\x94\x90";
const Bytes kMessageNonce = b64("yM/W3eTr8vkABw4VHCMqMTg/Rk1UW2Jp");
const Bytes kMessageCiphertext = b64("R2dDsSBiOfCHRBSeUmIkkMVenC6xUIY6i48LAawBivGbSLTG");

}  // namespace

TEST_CASE("Argon2id wrap key matches web", "[compat]") {
    CHECK(copy(deriveWrapKey(kPassword, kSalt)) == kWrapKey);
}

TEST_CASE("unwraps a private key wrapped by web", "[compat]") {
    SecretBytes unwrapped = unwrapPrivateKey({kEncryptedPrivateKey, kWrapNonce, kSalt}, kPassword);
    CHECK(copy(unwrapped) == kPrivateKey);
}

TEST_CASE("wrong password fails to unwrap", "[compat]") {
    CHECK_THROWS_AS(unwrapPrivateKey({kEncryptedPrivateKey, kWrapNonce, kSalt}, "wrong password"), CryptoError);
}

TEST_CASE("AEAD encrypt matches web for the same nonce", "[compat]") {
    AeadCiphertext result = aeadEncrypt(toBytes(kMessagePlaintext), kConversationKey, kMessageNonce);
    CHECK(result.ciphertext == kMessageCiphertext);
}

TEST_CASE("decrypts a message encrypted by web", "[compat]") {
    CHECK(decryptWithConversationKey(kMessageCiphertext, kMessageNonce, kConversationKey) == kMessagePlaintext);
}

TEST_CASE("unseals a conversation key sealed by web", "[compat]") {
    CHECK(copy(unsealConversationKey(kSealedByWeb, kPublicKey, kPrivateKey)) == kConversationKey);
}

TEST_CASE("seal round-trips", "[roundtrip]") {
    KeyPair pair = generateKeyPair();
    SecretBytes key = generateConversationKey();
    Bytes sealed = sealConversationKey(key, pair.publicKey);
    CHECK(copy(unsealConversationKey(sealed, pair.publicKey, pair.privateKey)) == copy(key));

    KeyPair other = generateKeyPair();
    CHECK_THROWS_AS(unsealConversationKey(sealed, other.publicKey, other.privateKey), CryptoError);
}

TEST_CASE("wrap round-trips", "[roundtrip]") {
    KeyPair pair = generateKeyPair();
    WrappedPrivateKey wrapped = wrapPrivateKey(pair.privateKey, "a new password");
    CHECK(copy(unwrapPrivateKey(wrapped, "a new password")) == copy(pair.privateKey));
}

TEST_CASE("message round-trips and rejects tampering", "[roundtrip]") {
    SecretBytes key = generateConversationKey();
    AeadCiphertext sealed = encryptWithConversationKey(kMessagePlaintext, key);
    CHECK(decryptWithConversationKey(sealed.ciphertext, sealed.nonce, key) == kMessagePlaintext);

    Bytes tampered = sealed.ciphertext;
    tampered[0] ^= 1;
    CHECK_THROWS_AS(decryptWithConversationKey(tampered, sealed.nonce, key), CryptoError);

    AeadCiphertext empty = encryptWithConversationKey("", key);
    CHECK(decryptWithConversationKey(empty.ciphertext, empty.nonce, key).empty());
}

TEST_CASE("safety number matches web in either order", "[compat]") {
    const Bytes other = b64("VAv7yr7CW/DLzzzkKkInMLSSxPlmrFnjlFIYi4CJSxM=");
    const std::string expected = "793202196896733342307744792420430796626930648824555351264617";
    CHECK(joined(safetyNumberDigits(kPublicKey, other)) == expected);
    CHECK(joined(safetyNumberDigits(other, kPublicKey)) == expected);
    CHECK(safetyNumberDigits(kPublicKey, other).size() == 12);
}

TEST_CASE("commitment matches web", "[compat]") {
    const std::string hex =
        commitment::computeHex("123e4567-e89b-12d3-a456-426614174000", kMessageCiphertext, kMessageNonce);
    CHECK(hex == "13fc08a55f02eef3fa748752614b96051b583027b4bc88495682f09dc51f63e3");
    CHECK(commitment::matches(hex, "13FC08A55F02EEF3FA748752614B96051B583027B4BC88495682F09DC51F63E3"));
    CHECK_FALSE(commitment::matches(hex, hex.substr(1)));
}

TEST_CASE("base64 is strict standard padded base64", "[base64]") {
    CHECK(base64Encode(toBytes("hi")) == "aGk=");
    CHECK(copy(b64("aGk=")) == toBytes("hi"));
    CHECK_THROWS_AS(base64Decode("aGk"), CryptoError);
    CHECK_THROWS_AS(base64Decode("aG-="), CryptoError);
    CHECK_THROWS_AS(base64Decode("aGk=!"), CryptoError);
}
