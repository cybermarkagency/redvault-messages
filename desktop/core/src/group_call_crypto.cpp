// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/crypto/group_call_crypto.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "redvault/crypto/base64.hpp"

namespace redvault::crypto::groupcalls {

namespace {

using Json = nlohmann::json;

constexpr const char* kType = "group-call-key";

}  // namespace

SecretBytes newRoomKey() {
    ensureSodium();
    SecretBytes key(kRoomKeyBytes);
    randombytes_buf(key.data(), key.size());
    return key;
}

int keySlot(std::int64_t epoch) {
    return static_cast<int>(((epoch % kKeyringSize) + kKeyringSize) % kKeyringSize);
}

Sealed sealRoomKey(const KeyMessage& message, ByteView key, ByteView recipientPublicKey, ByteView senderPrivateKey, std::int64_t now) {
    ensureSodium();
    if (recipientPublicKey.size() != crypto_box_PUBLICKEYBYTES || senderPrivateKey.size() != crypto_box_SECRETKEYBYTES) {
        throw CryptoError("Bad key size");
    }
    // The same fields, with the same names, as the web app's RoomKeyMessage.
    const std::string plain = Json{{"v", 1},
                                   {"type", kType},
                                   {"callId", message.callId},
                                   {"conversationId", message.conversationId},
                                   {"epoch", message.epoch},
                                   {"from", message.from},
                                   {"to", message.to},
                                   {"key", base64Encode(key)},
                                   {"sentAt", now}}
                                  .dump();
    Bytes nonce = randomBytes(crypto_box_NONCEBYTES);
    Bytes cipher(plain.size() + crypto_box_MACBYTES);
    if (crypto_box_easy(cipher.data(), reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), nonce.data(),
                        recipientPublicKey.data(), senderPrivateKey.data()) != 0) {
        throw CryptoError("Sealing failed");
    }
    return Sealed{base64Encode(cipher), base64Encode(nonce)};
}

SecretBytes openRoomKey(const Sealed& sealed, ByteView senderPublicKey, ByteView recipientPrivateKey, const KeyMessage& expected,
                        std::int64_t now) {
    ensureSodium();
    const auto unverified = [] { return GroupKeyError("The call key couldn't be verified as coming from the keyholder's identity key."); };

    Bytes cipher, nonce;
    try {
        cipher = base64Decode(sealed.ciphertext);
        nonce = base64Decode(sealed.nonce);
    } catch (const std::exception&) {
        throw unverified();
    }
    if (nonce.size() != crypto_box_NONCEBYTES || cipher.size() < crypto_box_MACBYTES || senderPublicKey.size() != crypto_box_PUBLICKEYBYTES ||
        recipientPrivateKey.size() != crypto_box_SECRETKEYBYTES) {
        throw unverified();
    }
    std::string plain(cipher.size() - crypto_box_MACBYTES, '\0');
    if (crypto_box_open_easy(reinterpret_cast<unsigned char*>(plain.data()), cipher.data(), cipher.size(), nonce.data(),
                             senderPublicKey.data(), recipientPrivateKey.data()) != 0) {
        throw unverified();
    }

    Json j;
    try {
        j = Json::parse(plain);
    } catch (const Json::exception&) {
        throw unverified();
    }
    // Compared as the web app compares them: a field of the wrong type is a mismatch, not an error.
    const auto text = [&](const char* name) -> const std::string* {
        const auto it = j.find(name);
        return it != j.end() && it->is_string() ? it->get_ptr<const std::string*>() : nullptr;
    };
    const auto number = [&](const char* name) -> const Json* {
        const auto it = j.find(name);
        return it != j.end() && it->is_number() ? &*it : nullptr;
    };
    const auto* callId = text("callId");
    const auto* conversationId = text("conversationId");
    const auto* from = text("from");
    const auto* to = text("to");
    const auto* type = text("type");
    const auto* keyText = text("key");
    const auto* epoch = number("epoch");
    const auto* v = number("v");
    const bool matches = j.is_object() && v && v->get<double>() == 1 && type && *type == kType && callId && *callId == expected.callId &&
                         conversationId && *conversationId == expected.conversationId && epoch &&
                         epoch->get<double>() == static_cast<double>(expected.epoch) && from && *from == expected.from && to &&
                         *to == expected.to && keyText;
    if (!matches) throw GroupKeyError("The call key didn't match this call.");

    const auto* sentAt = number("sentAt");
    if (!sentAt || std::abs(static_cast<double>(now) - sentAt->get<double>()) > static_cast<double>(kMaxKeyAgeMs)) {
        throw GroupKeyError("The call key is too old.");
    }

    Bytes raw;
    try {
        raw = base64Decode(*keyText);
    } catch (const std::exception&) {
        throw GroupKeyError("The call key has the wrong length.");
    }
    if (raw.size() != kRoomKeyBytes) throw GroupKeyError("The call key has the wrong length.");
    SecretBytes key = SecretBytes::copyOf(raw);
    sodium_memzero(raw.data(), raw.size());
    return key;
}

std::vector<std::string> groupSasWords(std::string_view callId, const std::vector<Participant>& participants, std::size_t count) {
    ensureSodium();
    std::vector<std::string> entries;
    entries.reserve(participants.size());
    for (const auto& p : participants) entries.push_back(p.userId + ":" + toHex(p.publicKey));
    std::sort(entries.begin(), entries.end());
    std::string message = "redvault-group-call-sas-v1|" + std::string(callId) + "|";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i) message += '|';
        message += entries[i];
    }
    std::array<unsigned char, 16> digest{};
    crypto_generichash(digest.data(), digest.size(), reinterpret_cast<const unsigned char*>(message.data()), message.size(), nullptr, 0);
    std::vector<std::string> words;
    for (std::size_t i = 0; i < std::min<std::size_t>(count, digest.size()); ++i) words.emplace_back(calls::wordListEntry(digest[i]));
    return words;
}

}  // namespace redvault::crypto::groupcalls
