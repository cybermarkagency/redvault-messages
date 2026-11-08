// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/crypto/vault_crypto.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

#include "redvault/crypto/call_crypto.hpp"
#include "redvault/crypto/e2e_crypto.hpp"

namespace redvault::crypto::vault {

namespace {

std::uint32_t rotl(std::uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

constexpr std::size_t kPadBucket = 256;

std::string padded(std::string_view json) {
    std::string out(json);
    const std::size_t target = ((out.size() / kPadBucket) + 1) * kPadBucket;
    out.append(target - out.size(), ' ');
    return out;
}

std::string trimRight(std::string text) {
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

std::string upperHex(ByteView bytes) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::string percentDecode(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else if (text[i] == '+') {
            out += ' ';
        } else {
            out += text[i];
        }
    }
    return out;
}

/// The passwords people pick most, lowercased: an instant "Weak" whatever their length.
const std::set<std::string, std::less<>>& commonPasswords() {
    static const std::set<std::string, std::less<>> list = {
        "123456", "123456789", "12345678", "password", "qwerty", "qwerty123", "1q2w3e4r", "12345", "111111", "123123",
        "1234567890", "1234567", "abc123", "password1", "password123", "iloveyou", "admin", "welcome", "monkey", "dragon",
        "letmein", "football", "baseball", "sunshine", "princess", "000000", "654321", "superman", "qazwsx", "trustno1",
        "master", "hello", "freedom", "whatever", "shadow", "michael", "charlie", "passw0rd", "p@ssw0rd", "zaq12wsx",
        "asdfghjkl", "qwertyuiop", "starwars", "login", "welcome1", "secret", "changeme", "default", "root", "test",
    };
    return list;
}

bool isSequential(unsigned char a, unsigned char b) {
    return (b == a + 1) || (b + 1 == a);
}

}  // namespace

// --- Hashes ---

Bytes sha1(ByteView data) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    Bytes msg(data.begin(), data.end());
    const std::uint64_t bitLength = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>(bitLength >> (i * 8)));

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(msg[chunk + i * 4]) << 24) | (std::uint32_t(msg[chunk + i * 4 + 1]) << 16) |
                   (std::uint32_t(msg[chunk + i * 4 + 2]) << 8) | std::uint32_t(msg[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    Bytes out(20);
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
}

Bytes hmacSha1(ByteView key, ByteView message) {
    constexpr std::size_t block = 64;
    Bytes k(key.begin(), key.end());
    if (k.size() > block) k = sha1(k);
    k.resize(block, 0);
    Bytes inner(block), outer(block);
    for (std::size_t i = 0; i < block; ++i) {
        inner[i] = k[i] ^ 0x36;
        outer[i] = k[i] ^ 0x5c;
    }
    inner.insert(inner.end(), message.begin(), message.end());
    const Bytes innerHash = sha1(inner);
    outer.insert(outer.end(), innerHash.begin(), innerHash.end());
    return sha1(outer);
}

Bytes hmacSha256(ByteView key, ByteView message) {
    ensureSodium();
    crypto_auth_hmacsha256_state state;
    crypto_auth_hmacsha256_init(&state, key.data(), key.size());
    crypto_auth_hmacsha256_update(&state, message.data(), message.size());
    Bytes out(crypto_auth_hmacsha256_BYTES);
    crypto_auth_hmacsha256_final(&state, out.data());
    return out;
}

Bytes hmacSha512(ByteView key, ByteView message) {
    ensureSodium();
    crypto_auth_hmacsha512_state state;
    crypto_auth_hmacsha512_init(&state, key.data(), key.size());
    crypto_auth_hmacsha512_update(&state, message.data(), message.size());
    Bytes out(crypto_auth_hmacsha512_BYTES);
    crypto_auth_hmacsha512_final(&state, out.data());
    return out;
}

// --- One-time codes ---

std::optional<Bytes> base32Decode(std::string_view text) {
    Bytes out;
    std::uint32_t buffer = 0;
    int bits = 0;
    bool any = false;
    for (char raw : text) {
        if (raw == ' ' || raw == '-' || raw == '=') continue;
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        int value;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= '2' && c <= '7') value = c - '2' + 26;
        else return std::nullopt;
        any = true;
        buffer = (buffer << 5) | static_cast<std::uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            out.push_back(static_cast<std::uint8_t>(buffer >> (bits - 8)));
            bits -= 8;
        }
    }
    if (!any || out.empty()) return std::nullopt;
    return out;
}

std::optional<TotpParams> parseTotp(std::string_view input) {
    std::string text(input);
    text.erase(0, text.find_first_not_of(" \t\r\n"));
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    TotpParams params;
    if (text.rfind("otpauth://", 0) == 0) {
        const auto typeEnd = text.find('/', 10);
        if (typeEnd == std::string::npos || text.substr(10, typeEnd - 10) != "totp") return std::nullopt;
        const auto query = text.find('?', typeEnd);
        const std::string label = percentDecode(text.substr(typeEnd + 1, query == std::string::npos ? std::string::npos : query - typeEnd - 1));
        if (const auto colon = label.find(':'); colon != std::string::npos) {
            params.issuer = label.substr(0, colon);
            params.account = label.substr(colon + 1);
            params.account.erase(0, params.account.find_first_not_of(' '));
        } else {
            params.account = label;
        }
        std::string secret;
        if (query != std::string::npos) {
            std::size_t pos = query + 1;
            while (pos < text.size()) {
                const auto amp = text.find('&', pos);
                const std::string pair = text.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
                const auto eq = pair.find('=');
                if (eq != std::string::npos) {
                    std::string key = pair.substr(0, eq);
                    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
                    const std::string value = percentDecode(pair.substr(eq + 1));
                    if (key == "secret") secret = value;
                    else if (key == "issuer") params.issuer = value;
                    else if (key == "algorithm") {
                        params.algorithm = value;
                        std::transform(params.algorithm.begin(), params.algorithm.end(), params.algorithm.begin(),
                                       [](unsigned char c) { return std::toupper(c); });
                    } else if (key == "digits") params.digits = std::atoi(value.c_str());
                    else if (key == "period") params.period = std::atoi(value.c_str());
                }
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
        }
        auto decoded = base32Decode(secret);
        if (!decoded) return std::nullopt;
        params.secret = std::move(*decoded);
    } else {
        auto decoded = base32Decode(text);
        if (!decoded) return std::nullopt;
        params.secret = std::move(*decoded);
    }
    if (params.algorithm != "SHA1" && params.algorithm != "SHA256" && params.algorithm != "SHA512") return std::nullopt;
    if (params.digits < 6 || params.digits > 8) params.digits = 6;
    if (params.period < 1 || params.period > 300) params.period = 30;
    return params;
}

std::string totpCode(const TotpParams& params, std::int64_t unixSeconds) {
    const std::uint64_t counter = static_cast<std::uint64_t>(unixSeconds < 0 ? 0 : unixSeconds) / static_cast<std::uint64_t>(params.period);
    std::uint8_t message[8];
    for (int i = 7; i >= 0; --i) message[7 - i] = static_cast<std::uint8_t>(counter >> (i * 8));
    const Bytes mac = params.algorithm == "SHA256"   ? hmacSha256(params.secret, {message, 8})
                      : params.algorithm == "SHA512" ? hmacSha512(params.secret, {message, 8})
                                                     : hmacSha1(params.secret, {message, 8});
    const int offset = mac.back() & 0x0F;
    const std::uint32_t binary = ((mac[offset] & 0x7Fu) << 24) | (std::uint32_t(mac[offset + 1]) << 16) |
                                 (std::uint32_t(mac[offset + 2]) << 8) | std::uint32_t(mac[offset + 3]);
    std::uint32_t modulo = 1;
    for (int i = 0; i < params.digits; ++i) modulo *= 10;
    std::string code = std::to_string(binary % modulo);
    return std::string(static_cast<std::size_t>(params.digits) - code.size(), '0') + code;
}

int totpSecondsLeft(const TotpParams& params, std::int64_t unixSeconds) {
    return params.period - static_cast<int>(unixSeconds % params.period);
}

// --- Breach check ---

std::string breachHash(std::string_view password) {
    return upperHex(sha1(toBytes(password)));
}

std::int64_t breachCount(std::string_view hashHex, std::string_view rangeResponse) {
    if (hashHex.size() != 40) return 0;
    const std::string_view suffix = hashHex.substr(5);
    std::size_t pos = 0;
    while (pos < rangeResponse.size()) {
        auto end = rangeResponse.find('\n', pos);
        if (end == std::string_view::npos) end = rangeResponse.size();
        std::string_view line = rangeResponse.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto colon = line.find(':');
        if (colon == 35 && line.substr(0, 35) == suffix) {
            // Padding entries have a count of 0: not a real breach.
            return std::atoll(std::string(line.substr(colon + 1)).c_str());
        }
        pos = end + 1;
    }
    return 0;
}

// --- Strength ---

StrengthEstimate estimateStrength(std::string_view password) {
    StrengthEstimate out;
    const auto finish = [&](double bits) {
        out.bits = bits;
        out.strength = bits >= 75 ? Strength::Strong : bits >= 50 ? Strength::Fair : Strength::Weak;
        out.label = out.strength == Strength::Strong ? "Strong" : out.strength == Strength::Fair ? "Fair" : "Weak";
        return out;
    };
    if (password.empty()) return finish(0);

    std::string lower(password);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (commonPasswords().count(lower)) return finish(8);

    bool hasLower = false, hasUpper = false, hasDigit = false, hasSymbol = false, hasOther = false;
    for (unsigned char c : password) {
        if (std::islower(c)) hasLower = true;
        else if (std::isupper(c)) hasUpper = true;
        else if (std::isdigit(c)) hasDigit = true;
        else if (c < 128) hasSymbol = true;
        else hasOther = true;
    }
    const int pool = (hasLower ? 26 : 0) + (hasUpper ? 26 : 0) + (hasDigit ? 10 : 0) + (hasSymbol ? 33 : 0) + (hasOther ? 64 : 0);

    // Characters that add little: repeats of the previous one and runs like "abc" or "321".
    double effective = 1;
    for (std::size_t i = 1; i < password.size(); ++i) {
        const auto a = static_cast<unsigned char>(password[i - 1]);
        const auto b = static_cast<unsigned char>(password[i]);
        if (a == b) effective += 0.2;
        else if (isSequential(a, b)) effective += 0.35;
        else effective += 1;
    }
    double bits = effective * std::log2(std::max(pool, 2));
    // A common password with a few characters added is still mostly guessable.
    for (const auto& common : commonPasswords()) {
        if (common.size() >= 5 && lower.find(common) != std::string::npos) {
            bits -= static_cast<double>(common.size()) * std::log2(std::max(pool, 2)) * 0.8;
            break;
        }
    }
    return finish(std::max(bits, 0.0));
}

// --- Generator ---

std::string generatePassword(const GeneratorOptions& options) {
    ensureSodium();
    if (options.words) {
        const int count = std::clamp(options.length, 4, 12);
        std::string out;
        for (int i = 0; i < count; ++i) {
            if (i > 0) out += '-';
            std::string word(calls::wordListEntry(randombytes_uniform(256)));
            if (options.upper && i == 0 && !word.empty()) word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
            out += word;
        }
        if (options.digits) out += "-" + std::to_string(randombytes_uniform(100));
        return out;
    }

    const std::string lowerSet = "abcdefghijkmnopqrstuvwxyz";  // no l: it reads like 1 and I
    const std::string upperSet = "ABCDEFGHJKLMNPQRSTUVWXYZ";   // no I or O
    const std::string digitSet = "23456789";                   // no 0 or 1
    const std::string symbolSet = "!@#$%^&*-_=+?";
    std::vector<const std::string*> sets = {&lowerSet};
    if (options.upper) sets.push_back(&upperSet);
    if (options.digits) sets.push_back(&digitSet);
    if (options.symbols) sets.push_back(&symbolSet);
    std::string all;
    for (const auto* set : sets) all += *set;

    const int length = std::clamp(options.length, 8, 128);
    // Redraw until every chosen class appears (rarely needs a second try).
    while (true) {
        std::string out;
        out.reserve(static_cast<std::size_t>(length));
        for (int i = 0; i < length; ++i) out += all[randombytes_uniform(static_cast<std::uint32_t>(all.size()))];
        const bool complete = std::all_of(sets.begin(), sets.end(), [&](const std::string* set) {
            return out.find_first_of(*set) != std::string::npos;
        });
        if (complete) return out;
    }
}

// --- Items ---

SecretBytes vaultKey(ByteView privateKey) {
    return calls::deriveLocalKey(privateKey, "redvault-password-vault-v1");
}

SealedItem sealItem(std::string_view json, ByteView key) {
    const auto sealed = encryptWithConversationKey(padded(json), key);
    return {sealed.ciphertext, sealed.nonce};
}

std::string openItem(ByteView ciphertext, ByteView nonce, ByteView key) {
    return trimRight(decryptWithConversationKey(ciphertext, nonce, key));
}

Bytes sealForRecipient(std::string_view json, ByteView recipientPublicKey) {
    ensureSodium();
    if (recipientPublicKey.size() != crypto_box_PUBLICKEYBYTES) throw CryptoError("Wrong public key size");
    const std::string plain = padded(json);
    Bytes out(plain.size() + crypto_box_SEALBYTES);
    if (crypto_box_seal(out.data(), reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), recipientPublicKey.data()) != 0) {
        throw CryptoError("Couldn't seal the item");
    }
    return out;
}

std::string openFromSender(ByteView sealed, ByteView publicKey, ByteView privateKey) {
    ensureSodium();
    if (sealed.size() < crypto_box_SEALBYTES) throw CryptoError("Sealed item is too short");
    std::string out(sealed.size() - crypto_box_SEALBYTES, '\0');
    if (crypto_box_seal_open(reinterpret_cast<unsigned char*>(out.data()), sealed.data(), sealed.size(), publicKey.data(), privateKey.data()) != 0) {
        throw CryptoError("Couldn't open the shared item");
    }
    return trimRight(out);
}

}  // namespace redvault::crypto::vault
