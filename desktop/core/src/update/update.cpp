// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/update/update.hpp"

#include <sodium.h>

#include <charconv>
#include <chrono>
#include <fstream>
#include <regex>

#include <nlohmann/json.hpp>

#include "redvault/crypto/base64.hpp"
#include "redvault/net/curl_transport.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/util/text.hpp"

namespace redvault::update {

namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view kKeyFormat = "redvault-release-key";

std::string requireString(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) throw UpdateError(std::string("Update manifest is missing ") + key);
    return it->get<std::string>();
}

crypto::ByteView view(const std::string& bytes) {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

}  // namespace

std::optional<Version> parseVersion(std::string_view text) {
    if (text.starts_with('v')) text.remove_prefix(1);
    Version out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto dot = text.find('.');
        const std::string_view part = i < 2 ? text.substr(0, dot) : text;
        if (part.empty() || part.size() > 6 || (i < 2 && dot == std::string_view::npos)) return std::nullopt;
        const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), out[i]);
        if (ec != std::errc{} || ptr != part.data() + part.size() || out[i] < 0) return std::nullopt;
        if (i < 2) text.remove_prefix(dot + 1);
    }
    return out;
}

ManifestCheck checkManifest(std::string_view manifestJson, crypto::ByteView publicKey, std::string_view currentVersion,
                            std::string_view allowedUrlPrefix, std::int64_t nowMillis) {
    crypto::ensureSodium();
    if (publicKey.size() != kPublicKeyBytes) throw UpdateError("No valid update key in this build");

    const Json envelope = Json::parse(manifestJson, nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object()) throw UpdateError("Update manifest isn't valid JSON");
    crypto::Bytes payload;
    crypto::Bytes signature;
    try {
        payload = crypto::base64Decode(requireString(envelope, "payload"));
        signature = crypto::base64Decode(requireString(envelope, "signature"));
    } catch (const crypto::CryptoError&) {
        throw UpdateError("Update manifest isn't valid base64");
    }
    // Nothing from the payload is looked at before the signature checks out.
    if (signature.size() != crypto_sign_BYTES ||
        crypto_sign_verify_detached(signature.data(), payload.data(), payload.size(), publicKey.data()) != 0) {
        throw UpdateError("Update manifest signature is invalid");
    }

    const Json body = Json::parse(payload.begin(), payload.end(), nullptr, false);
    if (body.is_discarded() || !body.is_object()) throw UpdateError("Signed update payload isn't valid JSON");
    if (requireString(body, "product") != kProduct) throw UpdateError("Update manifest is for a different product");

    Release release;
    release.version = requireString(body, "version");
    release.url = requireString(body, "url");
    release.sha256 = requireString(body, "sha256");
    release.notes = body.value("notes", "");
    release.published = body.value("published", "");
    ManifestCheck check;
    check.freshness = Freshness::NoExpiry;
    if (const auto expires = body.find("expires"); expires != body.end()) {
        const auto when = expires->is_string() ? util::parseIsoMillis(expires->get<std::string>()) : std::nullopt;
        if (!when) throw UpdateError("Update manifest has an invalid expiry");
        release.expires = expires->get<std::string>();
        check.freshness = nowMillis >= *when ? Freshness::Expired : Freshness::Current;
    }
    const auto size = body.find("size");
    if (size == body.end() || !size->is_number_unsigned()) throw UpdateError("Update manifest is missing size");
    release.size = size->get<std::uint64_t>();

    const auto offered = parseVersion(release.version);
    const auto current = parseVersion(currentVersion);
    if (!offered) throw UpdateError("Update manifest has an invalid version");
    if (!current) throw UpdateError("This build has an invalid version");
    static const std::regex kHex64("^[0-9a-f]{64}$");
    if (!std::regex_match(release.sha256, kHex64)) throw UpdateError("Update manifest has an invalid hash");
    if (release.size == 0 || release.size > 1024ULL * 1024 * 1024) throw UpdateError("Update manifest has an invalid size");
    // Defense in depth: even a validly signed manifest can only point at our own download folder.
    if (!release.url.starts_with(allowedUrlPrefix) || release.url.find("..") != std::string::npos ||
        release.url.find_first_of("?#\\ ") != std::string::npos) {
        throw UpdateError("Update manifest points outside the download folder");
    }

    // Only ever forward: an older (or the same) version is never offered, so a
    // replayed old manifest can't roll anyone back to a vulnerable release.
    if (*offered > *current) check.release = std::move(release);
    return check;
}

std::optional<Release> verifyManifest(std::string_view manifestJson, crypto::ByteView publicKey,
                                      std::string_view currentVersion, std::string_view allowedUrlPrefix) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return checkManifest(manifestJson, publicKey, currentVersion, allowedUrlPrefix, now).release;
}

bool updatesPermitted(std::string_view apiBase, bool torOnlyBuild) {
    const auto api = net::parseUrl(apiBase);
    if (!api) return false;
    if (net::isOnionHost(api->host)) return true;
    // Tor-only, or something that only looks like an onion address: never the normal internet.
    return !torOnlyBuild && !api->host.ends_with(".onion");
}

std::optional<UpdateRoute> updateRoute(std::string_view apiBase, bool torOnlyBuild, std::string_view clearManifestUrl,
                                       std::string_view clearDownloadPrefix) {
    const auto api = net::parseUrl(apiBase);
    const auto manifest = net::parseUrl(clearManifestUrl);
    const auto folder = net::parseUrl(clearDownloadPrefix);
    if (!api || !manifest || !folder || !updatesPermitted(apiBase, torOnlyBuild)) return std::nullopt;
    if (net::isOnionHost(api->host)) {
        // The onion service's copy of the same files (its nginx serves /redvaultmessages/desktop/), over Tor.
        // Plain http inside Tor: the onion address authenticates the server, and the manifest is signed anyway.
        const std::string origin = "http://" + api->host;
        return UpdateRoute{origin + manifest->path, origin + folder->path, true};
    }
    return UpdateRoute{std::string(clearManifestUrl), std::string(clearDownloadPrefix), false};
}

std::string installerUrl(const Release& release, const UpdateRoute& route, std::string_view clearDownloadPrefix) {
    if (!release.url.starts_with(clearDownloadPrefix)) return {};
    return route.downloadPrefix + release.url.substr(clearDownloadPrefix.size());
}

ManifestCheck fetchManifest(net::HttpTransport& transport, const UpdateSource& source) {
    const auto route = updateRoute(source.apiBase, source.torOnlyBuild, source.manifestUrl, source.allowedUrlPrefix);
    if (!route) return {};
    const auto response = transport.send(net::HttpRequest{"GET", route->manifestUrl, {}, {}});
    // No release published yet: nginx answers a missing file with the website's page (or a 404).
    // That means "nothing newer", not an error; anything that does look like a manifest still has to verify.
    const auto type = response.header("content-type").value_or("");
    if (response.status == 404 || (response.status == 200 && type.starts_with("text/html"))) return {};
    if (response.status != 200) throw UpdateError("The update server answered " + std::to_string(response.status));
    return checkManifest(response.body, source.publicKey, source.currentVersion, source.allowedUrlPrefix, source.nowMillis);
}

std::string signManifest(const Release& release, crypto::ByteView secretKey) {
    crypto::ensureSodium();
    if (secretKey.size() != kSecretKeyBytes) throw UpdateError("Invalid signing key");
    OrderedJson body;
    body["product"] = kProduct;
    body["version"] = release.version;
    body["url"] = release.url;
    body["sha256"] = release.sha256;
    body["size"] = release.size;
    body["notes"] = release.notes;
    body["published"] = release.published;
    if (!release.expires.empty()) body["expires"] = release.expires;
    const std::string payload = body.dump();

    crypto::Bytes signature(crypto_sign_BYTES);
    crypto_sign_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(payload.data()),
                         payload.size(), secretKey.data());
    OrderedJson envelope;
    envelope["payload"] = crypto::base64Encode(view(payload));
    envelope["signature"] = crypto::base64Encode(signature);
    return envelope.dump(2) + "\n";
}

bool installerMatches(const std::filesystem::path& file, const Release& release) {
    crypto::ensureSodium();
    std::error_code error;
    if (std::filesystem::file_size(file, error) != release.size || error) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    std::array<char, 64 * 1024> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<unsigned long long>(in.gcount());
        if (got > 0) crypto_hash_sha256_update(&state, reinterpret_cast<const unsigned char*>(buffer.data()), got);
    }
    if (in.bad()) return false;
    crypto::Bytes digest(crypto_hash_sha256_BYTES);
    crypto_hash_sha256_final(&state, digest.data());
    return crypto::toHex(digest) == release.sha256;
}

SigningKeyPair generateSigningKey() {
    crypto::ensureSodium();
    SigningKeyPair out{crypto::Bytes(crypto_sign_PUBLICKEYBYTES), crypto::SecretBytes(crypto_sign_SECRETKEYBYTES)};
    crypto_sign_keypair(out.publicKey.data(), out.secretKey.data());
    return out;
}

std::string sealSigningKey(const SigningKeyPair& keyPair, std::string_view passphrase) {
    // Same shape as the account key wrap: Argon2id-derived key, XChaCha20-Poly1305.
    const auto wrapped = crypto::wrapPrivateKey(keyPair.secretKey, passphrase);
    OrderedJson file;
    file["format"] = kKeyFormat;
    file["version"] = 1;
    file["publicKey"] = crypto::base64Encode(keyPair.publicKey);
    file["kdfSalt"] = crypto::base64Encode(wrapped.kdfSalt);
    file["nonce"] = crypto::base64Encode(wrapped.nonce);
    file["encryptedSecretKey"] = crypto::base64Encode(wrapped.encryptedPrivateKey);
    return file.dump(2) + "\n";
}

SigningKeyPair openSigningKey(std::string_view keyFileJson, std::string_view passphrase) {
    const Json file = Json::parse(keyFileJson, nullptr, false);
    if (file.is_discarded() || !file.is_object() || file.value("format", "") != kKeyFormat) {
        throw UpdateError("That isn't a Redvault release key file");
    }
    try {
        SigningKeyPair out;
        out.publicKey = crypto::base64Decode(requireString(file, "publicKey"));
        out.secretKey = crypto::unwrapPrivateKey(
            crypto::WrappedPrivateKey{crypto::base64Decode(requireString(file, "encryptedSecretKey")),
                                      crypto::base64Decode(requireString(file, "nonce")),
                                      crypto::base64Decode(requireString(file, "kdfSalt"))},
            passphrase);
        // The public key stored beside it must be the one this secret key signs for.
        if (out.secretKey.size() != kSecretKeyBytes || out.publicKey.size() != kPublicKeyBytes ||
            sodium_memcmp(out.secretKey.data() + 32, out.publicKey.data(), kPublicKeyBytes) != 0) {
            throw UpdateError("The release key file is damaged");
        }
        return out;
    } catch (const crypto::CryptoError&) {
        throw UpdateError("Wrong passphrase, or the release key file is damaged");
    }
}

}  // namespace redvault::update
