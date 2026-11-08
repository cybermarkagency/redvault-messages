// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "redvault/crypto/drive_crypto.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/vault_crypto.hpp"

using namespace redvault::crypto;

namespace {

std::string hex(ByteView bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (auto b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

}  // namespace

TEST_CASE("SHA-1 and HMAC-SHA1 match the published test vectors") {
    CHECK(hex(vault::sha1(toBytes(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex(vault::sha1(toBytes("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(vault::sha1(toBytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    // RFC 2202 test case 2.
    CHECK(hex(vault::hmacSha1(toBytes("Jefe"), toBytes("what do ya want for nothing?"))) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
}

TEST_CASE("TOTP matches RFC 6238's test vectors (SHA-1, SHA-256, SHA-512, 8 digits)") {
    vault::TotpParams sha1{toBytes("12345678901234567890"), "SHA1", 8, 30, {}, {}};
    vault::TotpParams sha256{toBytes("12345678901234567890123456789012"), "SHA256", 8, 30, {}, {}};
    vault::TotpParams sha512{toBytes("1234567890123456789012345678901234567890123456789012345678901234"), "SHA512", 8, 30, {}, {}};
    CHECK(vault::totpCode(sha1, 59) == "94287082");
    CHECK(vault::totpCode(sha256, 59) == "46119246");
    CHECK(vault::totpCode(sha512, 59) == "90693936");
    CHECK(vault::totpCode(sha1, 1111111109) == "07081804");
    CHECK(vault::totpCode(sha256, 1111111109) == "68084774");
    CHECK(vault::totpCode(sha512, 1111111109) == "25091201");
    CHECK(vault::totpCode(sha1, 20000000000) == "65353130");
    CHECK(vault::totpSecondsLeft(sha1, 59) == 1);
    CHECK(vault::totpSecondsLeft(sha1, 60) == 30);
}

TEST_CASE("base32 secrets and otpauth URIs parse like authenticator apps read them") {
    // "12345678901234567890" in base32.
    const auto plain = vault::parseTotp("GEZD GNBV GY3T QOJQ GEZD GNBV GY3T QOJQ");
    REQUIRE(plain);
    CHECK(plain->secret == toBytes("12345678901234567890"));
    CHECK(plain->digits == 6);

    const auto uri = vault::parseTotp("otpauth://totp/Example%20Co:you%40example.com?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=Example%20Co&digits=8&algorithm=SHA1&period=30");
    REQUIRE(uri);
    CHECK(uri->issuer == "Example Co");
    CHECK(uri->account == "you@example.com");
    CHECK(uri->digits == 8);
    CHECK(vault::totpCode(*uri, 59) == "94287082");

    CHECK_FALSE(vault::parseTotp("not a secret!"));
    CHECK_FALSE(vault::parseTotp("otpauth://hotp/x?secret=GEZDGNBV"));
}

TEST_CASE("The breach check sends 5 characters and finds the rest on the device") {
    const std::string hash = vault::breachHash("password");
    CHECK(hash == "5BAA61E4C9B93F3F0682250B6CF8331B7EE68FD8");
    const std::string response = "0018A45C4D1DEF81644B54AB7F969B88D65:1\r\n1E4C9B93F3F0682250B6CF8331B7EE68FD8:9545824\r\n"
                                 "011053FD0102E94D6AE2F8B83D76FAF94F6:0";
    CHECK(vault::breachCount(hash, response) == 9545824);
    CHECK(vault::breachCount(vault::breachHash("a much better passphrase"), response) == 0);
    // A padding line (count 0) is not a breach.
    CHECK(vault::breachCount("5BAA6011053FD0102E94D6AE2F8B83D76FAF94F6", response) == 0);
}

TEST_CASE("Strength: common and short passwords are weak, long random ones strong") {
    CHECK(vault::estimateStrength("password").strength == vault::Strength::Weak);
    CHECK(vault::estimateStrength("Password123").strength == vault::Strength::Weak);
    CHECK(vault::estimateStrength("aaaaaaaaaaaaaaaa").strength == vault::Strength::Weak);
    CHECK(vault::estimateStrength("hunter22").strength == vault::Strength::Weak);
    CHECK(vault::estimateStrength("sunflower2024").strength == vault::Strength::Fair);
    CHECK(vault::estimateStrength("vT7#qL!m2Zp9@wXe4Rk$").strength == vault::Strength::Strong);
    CHECK(vault::estimateStrength("vT7#qL!m2Zp9@wXe4Rk$").label == "Strong");
}

TEST_CASE("The generator honours length and character classes, and never repeats itself") {
    vault::GeneratorOptions options;
    std::set<std::string> seen;
    for (int i = 0; i < 50; ++i) {
        const std::string p = vault::generatePassword(options);
        CHECK(p.size() == 24);
        CHECK(p.find_first_of("ABCDEFGHJKLMNPQRSTUVWXYZ") != std::string::npos);
        CHECK(p.find_first_of("23456789") != std::string::npos);
        CHECK(p.find_first_of("!@#$%^&*-_=+?") != std::string::npos);
        seen.insert(p);
    }
    CHECK(seen.size() == 50);
    CHECK(vault::estimateStrength(vault::generatePassword(options)).strength == vault::Strength::Strong);

    options.symbols = false;
    options.length = 12;
    const std::string noSymbols = vault::generatePassword(options);
    CHECK(noSymbols.size() == 12);
    CHECK(noSymbols.find_first_of("!@#$%^&*-_=+?") == std::string::npos);

    vault::GeneratorOptions words;
    words.words = true;
    words.length = 6;
    const std::string phrase = vault::generatePassword(words);
    CHECK(std::count(phrase.begin(), phrase.end(), '-') == 6);  // 6 words and a number
}

TEST_CASE("Items: sealed under the vault key, padded, and only the recipient opens a shared copy") {
    const auto owner = generateKeyPair();
    const auto other = generateKeyPair();
    const auto key = vault::vaultKey(owner.privateKey);
    CHECK(vault::vaultKey(owner.privateKey).view().size() == 32);

    const std::string json = R"({"type":"login","title":"Cloud console","password":"vT7#qL!m2Zp9@wXe4Rk$"})";
    const auto sealed = vault::sealItem(json, key);
    CHECK(sealed.ciphertext.size() % 256 == 16);  // padded to a 256-byte bucket, plus the tag
    CHECK(vault::openItem(sealed.ciphertext, sealed.nonce, key) == json);
    const auto wrongKey = vault::vaultKey(other.privateKey);
    CHECK_THROWS(vault::openItem(sealed.ciphertext, sealed.nonce, wrongKey));

    const auto shared = vault::sealForRecipient(json, other.publicKey);
    CHECK(vault::openFromSender(shared, other.publicKey, other.privateKey) == json);
    CHECK_THROWS(vault::openFromSender(shared, owner.publicKey, owner.privateKey));
}

TEST_CASE("Files: metadata under the drive key; link keys need the password when there is one") {
    const auto owner = generateKeyPair();
    const auto key = drive::driveKey(owner.privateKey);
    const auto sealed = drive::sealMeta(R"({"name":"Contract_signed.pdf"})", key);
    CHECK(drive::openMeta(sealed.ciphertext, sealed.nonce, key) == R"({"name":"Contract_signed.pdf"})");

    const Bytes secret(32, 7);
    const auto salt = drive::newPasswordSalt();
    CHECK(drive::linkKey(secret, "", salt).view().size() == 32);
    const auto withPassword = drive::linkKey(secret, "correct horse", salt);
    const auto again = drive::linkKey(secret, "correct horse", salt);
    const auto wrong = drive::linkKey(secret, "wrong horse", salt);
    CHECK(Bytes(withPassword.view().begin(), withPassword.view().end()) == Bytes(again.view().begin(), again.view().end()));
    CHECK(Bytes(withPassword.view().begin(), withPassword.view().end()) != Bytes(wrong.view().begin(), wrong.view().end()));
    CHECK(drive::fileLinkUrl("https://example.com/app", "abc", secret).rfind("https://example.com/app/?file=abc#", 0) == 0);
}
