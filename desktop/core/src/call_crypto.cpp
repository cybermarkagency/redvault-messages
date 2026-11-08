// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/crypto/call_crypto.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"

namespace redvault::crypto::calls {

namespace {

using Json = nlohmann::json;

/// Signals older than this are replays: calls ring for 45 s at most.
constexpr std::int64_t kMaxSignalAgeMs = 5 * 60 * 1000;

/// A relay-only offer or answer is a few kilobytes; anything far larger is refused before it's parsed.
constexpr std::size_t kMaxSdpBytes = 32 * 1024;

std::string toJsonText(const CallSignal& s) {
    return Json{{"v", 1},         {"type", s.type}, {"callId", s.callId}, {"conversationId", s.conversationId},
                {"from", s.from}, {"to", s.to},     {"kind", s.kind},     {"sdp", s.sdp},
                {"sentAt", s.sentAt}}
        .dump();
}

CallSignal fromJsonText(std::string_view text) {
    Json j;
    try {
        j = Json::parse(text);
    } catch (const Json::exception&) {
        throw CallSignalError("Malformed call setup.");
    }
    try {
        if (j.at("v").get<int>() != 1) throw CallSignalError("Unsupported call setup version.");
        CallSignal s;
        s.type = j.at("type").get<std::string>();
        s.callId = j.at("callId").get<std::string>();
        s.conversationId = j.at("conversationId").get<std::string>();
        s.from = j.at("from").get<std::string>();
        s.to = j.at("to").get<std::string>();
        s.kind = j.at("kind").get<std::string>();
        s.sdp = j.at("sdp").get<std::string>();
        s.sentAt = j.at("sentAt").get<std::int64_t>();
        return s;
    } catch (const Json::exception&) {
        throw CallSignalError("Malformed call setup.");
    }
}

void check(const CallSignal& s, const Expected& e, std::int64_t now) {
    const bool matches = s.type == e.type && s.callId == e.callId && s.conversationId == e.conversationId && s.from == e.from &&
                         s.to == e.to && (!e.kind || s.kind == *e.kind) && (s.kind == "voice" || s.kind == "video");
    if (!matches) throw CallSignalError("Call setup didn't match this call.");
    if (std::llabs(now - s.sentAt) > kMaxSignalAgeMs) throw CallSignalError("Call setup is too old.");
    if (s.sdp.size() > kMaxSdpBytes) throw CallSignalError("Call setup is too large.");
    // Without a DTLS fingerprint there'd be nothing binding the media to this key.
    if (sdpFingerprint(s.sdp).empty()) throw CallSignalError("Call setup is missing its encryption fingerprint.");
}

}  // namespace

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

Sealed seal(const CallSignal& signal, ByteView recipientPublicKey, ByteView senderPrivateKey) {
    ensureSodium();
    if (recipientPublicKey.size() != crypto_box_PUBLICKEYBYTES || senderPrivateKey.size() != crypto_box_SECRETKEYBYTES) {
        throw CryptoError("Bad key size");
    }
    const std::string plain = toJsonText(signal);
    Bytes nonce = randomBytes(crypto_box_NONCEBYTES);
    Bytes cipher(plain.size() + crypto_box_MACBYTES);
    if (crypto_box_easy(cipher.data(), reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), nonce.data(),
                        recipientPublicKey.data(), senderPrivateKey.data()) != 0) {
        throw CryptoError("Sealing failed");
    }
    return Sealed{base64Encode(cipher), base64Encode(nonce)};
}

CallSignal open(const Sealed& sealed, ByteView senderPublicKey, ByteView recipientPrivateKey, const Expected& expected, std::int64_t now) {
    ensureSodium();
    Bytes cipher, nonce;
    try {
        cipher = base64Decode(sealed.ciphertext);
        nonce = base64Decode(sealed.nonce);
    } catch (const std::exception&) {
        throw CallSignalError("Malformed call setup.");
    }
    if (nonce.size() != crypto_box_NONCEBYTES || cipher.size() < crypto_box_MACBYTES || senderPublicKey.size() != crypto_box_PUBLICKEYBYTES ||
        recipientPrivateKey.size() != crypto_box_SECRETKEYBYTES) {
        throw CallSignalError("This call couldn't be verified as coming from your contact's key.");
    }
    std::string plain(cipher.size() - crypto_box_MACBYTES, '\0');
    if (crypto_box_open_easy(reinterpret_cast<unsigned char*>(plain.data()), cipher.data(), cipher.size(), nonce.data(),
                             senderPublicKey.data(), recipientPrivateKey.data()) != 0) {
        throw CallSignalError("This call couldn't be verified as coming from your contact's key.");
    }
    CallSignal s = fromJsonText(plain);
    check(s, expected, now);
    return s;
}

SecretBytes newLinkSecret() {
    ensureSodium();
    SecretBytes secret(32);
    randombytes_buf(secret.data(), secret.size());
    return secret;
}

std::string linkSecretToFragment(ByteView secret) {
    ensureSodium();
    std::string out(sodium_base64_ENCODED_LEN(secret.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(out.data(), out.size(), secret.data(), secret.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

std::optional<SecretBytes> linkSecretFromFragment(std::string_view fragment) {
    ensureSodium();
    if (!fragment.empty() && fragment.front() == '#') fragment.remove_prefix(1);
    SecretBytes secret(32);
    size_t length = 0;
    if (sodium_base642bin(secret.data(), secret.size(), fragment.data(), fragment.size(), nullptr, &length, nullptr,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0 ||
        length != 32) {
        return std::nullopt;
    }
    return secret;
}

std::string linkUrl(std::string_view webAppBase, std::string_view linkId, ByteView secret) {
    std::string base(webAppBase);
    if (base.empty() || base.back() != '/') base += '/';
    return base + "?join=" + std::string(linkId) + "#" + linkSecretToFragment(secret);
}

Sealed sealLinkName(std::string_view name, ByteView secret) {
    const AeadCiphertext c = encryptWithConversationKey(name, secret);
    return Sealed{base64Encode(c.ciphertext), base64Encode(c.nonce)};
}

std::string openLinkName(const Sealed& name, ByteView secret) {
    return decryptWithConversationKey(base64Decode(name.ciphertext), base64Decode(name.nonce), secret);
}

std::string sealSecretForOwner(ByteView secret, ByteView ownerPublicKey) {
    return base64Encode(sealConversationKey(secret, ownerPublicKey));
}

SecretBytes openOwnerSecret(std::string_view sealedB64, ByteView ownerPublicKey, ByteView ownerPrivateKey) {
    return unsealConversationKey(base64Decode(sealedB64), ownerPublicKey, ownerPrivateKey);
}

Sealed sealWithLink(const CallSignal& signal, ByteView secret) {
    const AeadCiphertext c = encryptWithConversationKey(toJsonText(signal), secret);
    return Sealed{base64Encode(c.ciphertext), base64Encode(c.nonce)};
}

CallSignal openWithLink(const Sealed& sealed, ByteView secret, const Expected& expected, std::int64_t now) {
    std::string plain;
    try {
        plain = decryptWithConversationKey(base64Decode(sealed.ciphertext), base64Decode(sealed.nonce), secret);
    } catch (const std::exception&) {
        throw CallSignalError("This request couldn't be verified with the link's secret.");
    }
    CallSignal s = fromJsonText(plain);
    check(s, expected, now);
    return s;
}

// The first line "a=fingerprint:<hash> <hex and colons>", the same rule as the regex
// (^|\n)a=fingerprint:(\S+ [0-9A-Fa-f:]+) but in one linear pass: the SDP comes from the other side of the call,
// and std::regex recursed once per character, so a long line overflowed the stack (a crash before answering).
std::string sdpFingerprint(std::string_view sdp) {
    constexpr std::string_view kPrefix = "a=fingerprint:";
    const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    const auto isHexOrColon = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0 || c == ':'; };
    for (std::size_t line = 0; line < sdp.size();) {
        const auto next = sdp.find('\n', line);
        const auto rest = sdp.substr(line, next == std::string_view::npos ? std::string_view::npos : next - line);
        if (rest.starts_with(kPrefix)) {
            const auto value = rest.substr(kPrefix.size());
            std::size_t hash = 0;
            while (hash < value.size() && !isSpace(value[hash])) ++hash;
            if (hash > 0 && hash < value.size() && value[hash] == ' ') {
                std::size_t end = hash + 1;
                while (end < value.size() && isHexOrColon(value[end])) ++end;
                if (end > hash + 1) return std::string(value.substr(0, end));
            }
        }
        if (next == std::string_view::npos) break;
        line = next + 1;
    }
    return {};
}

namespace {

// Exactly ui/src/lib/sas.ts's list: one word per byte value.
const std::array<const char*, 256> kWords = {
    "acorn", "adobe", "agent", "alpine", "amber", "anchor", "angle", "apple", "arbor", "arctic", "arrow", "aspen",
    "atlas", "attic", "autumn", "avenue", "badge", "bakery", "bamboo", "banjo", "barley", "basin", "beacon",
    "bishop", "blanket", "bloom", "bonfire", "border", "breeze", "bridge", "bronze", "bubble", "cabin", "cactus",
    "camel", "canal", "candle", "canyon", "carbon", "cargo", "castle", "cedar", "cellar", "chalk", "cherry",
    "chimney", "cider", "cinder", "citrus", "clover", "cobalt", "cocoa", "comet", "coral", "cotton", "cradle",
    "crater", "crystal", "cypress", "dahlia", "delta", "desert", "dolphin", "domino", "dragon", "drift", "dune",
    "eagle", "echo", "eclipse", "elbow", "ember", "emerald", "engine", "falcon", "fable", "feather", "fennel",
    "ferry", "fiddle", "fjord", "flint", "forest", "fossil", "fountain", "galaxy", "garden", "garnet", "gazelle",
    "geyser", "ginger", "glacier", "globe", "granite", "gravel", "grove", "hammock", "harbor", "harvest", "hazel",
    "helmet", "heron", "hollow", "honey", "horizon", "indigo", "iris", "island", "ivory", "jacket", "jasmine",
    "jungle", "juniper", "kayak", "kettle", "kiwi", "koala", "lagoon", "lantern", "lava", "lemon", "lilac",
    "linen", "lotus", "lunar", "magnet", "mango", "maple", "marble", "meadow", "melon", "meteor", "mirror",
    "mosaic", "moss", "nectar", "needle", "nimbus", "nomad", "nutmeg", "oasis", "ocean", "olive", "onyx", "opal",
    "orbit", "orchid", "otter", "oyster", "paddle", "panda", "papaya", "parrot", "pebble", "pepper", "piano",
    "pillow", "pine", "planet", "plum", "polar", "poppy", "prism", "pumpkin", "quartz", "quill", "quiver",
    "rabbit", "radar", "raven", "reef", "ribbon", "river", "rocket", "saddle", "saffron", "salmon", "sapphire",
    "satin", "shadow", "shell", "sierra", "silver", "sketch", "sparrow", "spruce", "summit", "sunset", "swan",
    "tango", "temple", "thistle", "thunder", "tiger", "timber", "topaz", "tulip", "tundra", "turtle", "valley",
    "vapor", "velvet", "violet", "walnut", "willow", "window", "winter", "wizard", "yarrow", "zebra", "zephyr",
    "zinnia", "anvil", "ballad", "bramble", "cobble", "dusk", "fern", "gable", "hive", "jade", "kelp", "larch",
    "mint", "nova", "oak", "pearl", "quail", "rain", "sage", "tide", "basalt", "beetle", "blossom", "bugle",
    "cabana", "caramel", "chestnut", "cobra", "condor", "crocus", "dingo", "falafel", "firefly", "gecko", "goblet",
    "hamlet", "igloo", "jigsaw", "kestrel", "lichen", "lobster", "mesa", "muffin", "narwhal", "nugget",
};

std::string upper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

}  // namespace

std::string_view wordListEntry(std::size_t index) {
    return kWords[index % kWords.size()];
}

std::vector<std::string> sasWords(std::string_view fingerprintA, std::string_view fingerprintB, std::size_t count) {
    ensureSodium();
    std::string a = upper(fingerprintA);
    std::string b = upper(fingerprintB);
    if (b < a) std::swap(a, b);
    const std::string message = "redvault-call-sas-v1|" + a + "|" + b;
    std::array<unsigned char, 16> digest{};
    crypto_generichash(digest.data(), digest.size(), reinterpret_cast<const unsigned char*>(message.data()), message.size(), nullptr, 0);
    std::vector<std::string> words;
    for (std::size_t i = 0; i < std::min<std::size_t>(count, digest.size()); ++i) words.emplace_back(kWords[digest[i]]);
    return words;
}

SecretBytes deriveLocalKey(ByteView privateKey, std::string_view label) {
    ensureSodium();
    SecretBytes key(32);
    crypto_generichash(key.data(), key.size(), reinterpret_cast<const unsigned char*>(label.data()), label.size(), privateKey.data(),
                       privateKey.size());
    return key;
}

namespace merkle {

namespace {

Bytes nodeHash(ByteView left, ByteView right) {
    Bytes joined;
    joined.reserve(1 + left.size() + right.size());
    joined.push_back(0x01);
    joined.insert(joined.end(), left.begin(), left.end());
    joined.insert(joined.end(), right.begin(), right.end());
    return sha256(joined);
}

bool equal(ByteView a, ByteView b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

}  // namespace

Bytes leafHash(ByteView data) {
    Bytes joined;
    joined.reserve(1 + data.size());
    joined.push_back(0x00);
    joined.insert(joined.end(), data.begin(), data.end());
    return sha256(joined);
}

// RFC 9162 2.1.3.2
bool verifyInclusion(ByteView leaf, std::uint64_t index, std::uint64_t size, const std::vector<Bytes>& proof, ByteView root) {
    if (index >= size) return false;
    std::uint64_t fn = index;
    std::uint64_t sn = size - 1;
    Bytes r(leaf.begin(), leaf.end());
    for (const Bytes& p : proof) {
        if (sn == 0) return false;
        if ((fn & 1) == 1 || fn == sn) {
            r = nodeHash(p, r);
            while ((fn & 1) == 0 && fn != 0) {
                fn >>= 1;
                sn >>= 1;
            }
        } else {
            r = nodeHash(r, p);
        }
        fn >>= 1;
        sn >>= 1;
    }
    return sn == 0 && equal(r, root);
}

// RFC 9162 2.1.4.2
bool verifyConsistency(std::uint64_t from, std::uint64_t to, ByteView fromRoot, ByteView toRoot, const std::vector<Bytes>& proof) {
    if (from < 1 || from > to) return false;
    if (from == to) return proof.empty() && equal(fromRoot, toRoot);
    std::vector<Bytes> path;
    if ((from & (from - 1)) == 0) path.emplace_back(fromRoot.begin(), fromRoot.end());
    path.insert(path.end(), proof.begin(), proof.end());
    if (path.empty()) return false;
    std::uint64_t fn = from - 1;
    std::uint64_t sn = to - 1;
    while ((fn & 1) == 1) {
        fn >>= 1;
        sn >>= 1;
    }
    Bytes fr = path[0];
    Bytes sr = path[0];
    for (std::size_t i = 1; i < path.size(); ++i) {
        const Bytes& c = path[i];
        if (sn == 0) return false;
        if ((fn & 1) == 1 || fn == sn) {
            fr = nodeHash(c, fr);
            sr = nodeHash(c, sr);
            while ((fn & 1) == 0 && fn != 0) {
                fn >>= 1;
                sn >>= 1;
            }
        } else {
            sr = nodeHash(sr, c);
        }
        fn >>= 1;
        sn >>= 1;
    }
    return sn == 0 && equal(fr, fromRoot) && equal(sr, toRoot);
}

Bytes fromHex(std::string_view hex) {
    if (hex.size() % 2 != 0) throw CryptoError("Bad hex");
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw CryptoError("Bad hex");
    };
    Bytes out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
    return out;
}

std::string toHex(ByteView bytes) {
    return crypto::toHex(bytes);
}

}  // namespace merkle

std::string checkpointMessage(std::int64_t treeSize, std::string_view rootHashHex, std::string_view signedAtIso) {
    return "redvault-transparency-log-v1\n" + std::to_string(treeSize) + "\n" + std::string(rootHashHex) + "\n" + std::string(signedAtIso);
}

bool checkpointSignatureValid(std::int64_t treeSize, std::string_view rootHashHex, std::string_view signedAtIso,
                              std::string_view signatureB64, std::string_view publicKeyB64) {
    ensureSodium();
    Bytes signature, key;
    try {
        signature = base64Decode(signatureB64);
        key = base64Decode(publicKeyB64);
    } catch (const std::exception&) {
        return false;
    }
    if (signature.size() != crypto_sign_BYTES || key.size() != crypto_sign_PUBLICKEYBYTES) return false;
    const std::string message = checkpointMessage(treeSize, rootHashHex, signedAtIso);
    return crypto_sign_verify_detached(signature.data(), reinterpret_cast<const unsigned char*>(message.data()), message.size(),
                                       key.data()) == 0;
}

}  // namespace redvault::crypto::calls
