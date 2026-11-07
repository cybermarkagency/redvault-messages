#include "redvault/crypto/group_invite_crypto.hpp"

#include <sodium.h>

#include <nlohmann/json.hpp>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/call_crypto.hpp"
#include "redvault/crypto/e2e_crypto.hpp"

namespace redvault::crypto::invites {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kKeyLabel = "redvault-group-invite-v1:key";
constexpr std::string_view kAuthLabel = "redvault-group-invite-v1:auth";

Bytes derive(std::string_view label, ByteView secret) {
    ensureSodium();
    if (secret.size() < crypto_generichash_KEYBYTES_MIN || secret.size() > crypto_generichash_KEYBYTES_MAX) {
        throw CryptoError("Wrong invite secret size");
    }
    // The label is the message and the secret the key, as libsodium.js's crypto_generichash(32, label, secret).
    Bytes out(32);
    crypto_generichash(out.data(), out.size(), reinterpret_cast<const unsigned char*>(label.data()), label.size(), secret.data(),
                       secret.size());
    return out;
}

struct Encrypted {
    std::string ciphertext;
    std::string nonce;
};

Encrypted encrypt(ByteView plaintext, ByteView key, ByteView nonce) {
    const AeadCiphertext c = aeadEncrypt(plaintext, key, nonce);
    return {base64Encode(c.ciphertext), base64Encode(c.nonce)};
}

SecretBytes decrypt(std::string_view ciphertext, std::string_view nonce, ByteView key) {
    return aeadDecrypt(base64Decode(ciphertext), base64Decode(nonce), key);
}

/// JSON.stringify({ v: 1, name }): the same keys in the same order, so the plaintext is the web app's byte for byte.
std::string previewJson(std::string_view name) {
    return "{\"v\":1,\"name\":" + Json(std::string(name)).dump(-1, ' ', false, Json::error_handler_t::replace) + "}";
}

bool isBase64UrlChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool allBase64Url(std::string_view text) {
    for (const char c : text) {
        if (!isBase64UrlChar(c)) return false;
    }
    return true;
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// application/x-www-form-urlencoded decoding, as URLSearchParams does it.
std::string formDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < text.size() && hexValue(text[i + 1]) >= 0 && hexValue(text[i + 2]) >= 0) {
            out += static_cast<char>(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2]));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

/// The address in a paste, like the web app's pastedUrl: the first http(s) link in it, or else the whole (trimmed) text.
std::string_view pastedUrl(std::string_view text) {
    for (std::size_t at = text.find("http"); at != std::string_view::npos; at = text.find("http", at + 1)) {
        std::string_view rest = text.substr(at);
        const std::size_t scheme = rest.starts_with("https://") ? 8 : rest.starts_with("http://") ? 7 : 0;
        if (scheme == 0) continue;
        std::size_t end = scheme;
        while (end < rest.size() && !isSpace(rest[end])) ++end;
        if (end == scheme) continue;  // "https://" alone isn't a link (\S+ needs a character)
        return rest.substr(0, end);
    }
    while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
    return text;
}

}  // namespace

InviteKeys inviteKeys(ByteView secret) {
    Bytes key = derive(kKeyLabel, secret);
    Bytes auth = derive(kAuthLabel, secret);
    InviteKeys out{SecretBytes::copyOf(key), base64Encode(auth)};
    sodium_memzero(key.data(), key.size());
    return out;
}

std::string secretToFragment(ByteView secret) {
    return calls::linkSecretToFragment(secret);
}

std::optional<SecretBytes> secretFromFragment(std::string_view fragment) {
    return calls::linkSecretFromFragment(fragment);
}

std::string inviteUrl(std::string_view webAppBase, std::string_view linkId, ByteView secret) {
    std::string base(webAppBase);
    if (base.empty() || base.back() != '/') base += '/';
    // Link ids are base64url, which needs no escaping in a query.
    return base + "?invite=" + std::string(linkId) + "#" + secretToFragment(secret);
}

std::optional<ParsedInvite> parseInvite(std::string_view text) {
    const std::string_view url = pastedUrl(text);
    if (url.empty()) return std::nullopt;
    const std::size_t hash = url.find('#');
    const std::string_view beforeHash = url.substr(0, hash);
    const std::string_view fragment = hash == std::string_view::npos ? std::string_view{} : url.substr(hash + 1);
    const std::size_t question = beforeHash.find('?');
    const std::string_view query = question == std::string_view::npos ? std::string_view{} : beforeHash.substr(question + 1);

    // URLSearchParams.get("invite"): the first parameter by that name.
    std::optional<std::string> linkId;
    for (std::size_t start = 0; !query.empty() && !linkId;) {
        const std::size_t amp = query.find('&', start);
        const std::string_view pair = query.substr(start, amp == std::string_view::npos ? std::string_view::npos : amp - start);
        const std::size_t eq = pair.find('=');
        if (!pair.empty() && formDecode(pair.substr(0, eq)) == "invite") {
            linkId = eq == std::string_view::npos ? std::string() : formDecode(pair.substr(eq + 1));
        }
        if (amp == std::string_view::npos) break;
        start = amp + 1;
    }
    if (!linkId || linkId->empty() || linkId->size() > 64 || !allBase64Url(*linkId)) return std::nullopt;
    if (fragment.size() != 43 || !allBase64Url(fragment)) return std::nullopt;
    return ParsedInvite{*linkId, std::string(fragment)};
}

NewLink newInviteLink(ByteView groupKey, std::string_view groupName, bool approval) {
    LinkRandomness random;
    random.secret = randomBytes(kSecretBytes);
    random.secretNonce = randomBytes(kAeadNonceBytes);
    random.previewNonce = randomBytes(kAeadNonceBytes);
    random.wrappedKeyNonce = randomBytes(kAeadNonceBytes);
    NewLink link = newInviteLink(groupKey, groupName, approval, random);
    sodium_memzero(random.secret.data(), random.secret.size());
    return link;
}

NewLink newInviteLink(ByteView groupKey, std::string_view groupName, bool approval, const LinkRandomness& fixed) {
    if (fixed.secret.size() != kSecretBytes) throw CryptoError("Wrong invite secret size");
    NewLink out{SecretBytes::copyOf(fixed.secret), {}};
    const InviteKeys keys = inviteKeys(out.secret);
    const Encrypted sealedSecret = encrypt(out.secret, groupKey, fixed.secretNonce);
    const Bytes preview = toBytes(previewJson(groupName));
    const Encrypted encryptedPreview = encrypt(preview, keys.key, fixed.previewNonce);
    out.input.auth = keys.auth;
    out.input.secretCiphertext = sealedSecret.ciphertext;
    out.input.secretNonce = sealedSecret.nonce;
    out.input.previewCiphertext = encryptedPreview.ciphertext;
    out.input.previewNonce = encryptedPreview.nonce;
    if (!approval) {
        const WrappedKey wrapped = wrapGroupKeyForLink(groupKey, out.secret, fixed.wrappedKeyNonce);
        out.input.wrappedKey = wrapped.wrappedKey;
        out.input.wrappedKeyNonce = wrapped.wrappedKeyNonce;
    }
    return out;
}

WrappedKey wrapGroupKeyForLink(ByteView groupKey, ByteView secret) {
    return wrapGroupKeyForLink(groupKey, secret, randomBytes(kAeadNonceBytes));
}

WrappedKey wrapGroupKeyForLink(ByteView groupKey, ByteView secret, ByteView nonce) {
    const Encrypted c = encrypt(groupKey, inviteKeys(secret).key, nonce);
    return {c.ciphertext, c.nonce};
}

std::optional<OpenedLink> openInviteLink(std::string_view linkId, std::string_view secretCiphertext, std::string_view secretNonce,
                                         ByteView groupKey, std::string_view webAppBase) {
    try {
        SecretBytes secret = decrypt(secretCiphertext, secretNonce, groupKey);
        std::string url = inviteUrl(webAppBase, linkId, secret);
        return OpenedLink{std::move(secret), std::move(url)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

OpenedPreview openInvitePreview(std::string_view previewCiphertext, std::string_view previewNonce,
                                const std::optional<std::string>& wrappedKey, const std::optional<std::string>& wrappedKeyNonce,
                                ByteView secret) {
    const InviteKeys keys = inviteKeys(secret);
    const SecretBytes plain = decrypt(previewCiphertext, previewNonce, keys.key);
    Json parsed;
    try {
        parsed = Json::parse(std::string_view(reinterpret_cast<const char*>(plain.data()), plain.size()));
    } catch (const Json::exception&) {
        throw CryptoError("The invite's preview isn't readable");
    }
    // JSON.parse("null").name throws on the web too.
    if (!parsed.is_object()) throw CryptoError("The invite's preview isn't readable");
    OpenedPreview out;
    const auto name = parsed.find("name");
    out.name = name != parsed.end() && name->is_string() ? name->get<std::string>() : "Group";
    if (wrappedKey && wrappedKeyNonce && !wrappedKey->empty() && !wrappedKeyNonce->empty()) {
        out.groupKey = decrypt(*wrappedKey, *wrappedKeyNonce, keys.key);
    }
    return out;
}

}  // namespace redvault::crypto::invites
