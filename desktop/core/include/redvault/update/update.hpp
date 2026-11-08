// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "redvault/crypto/bytes.hpp"
#include "redvault/net/http.hpp"

// Signed auto-update for the desktop app.
//
// The server hosts a small manifest (latest.json) next to the installers:
//
//   {"payload": "<base64 of the release JSON>", "signature": "<base64 Ed25519 signature of those bytes>"}
//
// The release JSON says which version is current, where its installer is,
// and the installer's SHA-256 and size. Only the holder of the offline
// release key can sign it; the app carries the matching public key and
// trusts nothing it can't verify: not the server, not TLS alone. It only
// ever moves forward (no downgrades), only downloads from the expected
// folder, and only runs an installer whose hash and size match.
//
// A signature says who wrote a manifest, not when. Someone between the app and
// the server could keep serving an old one, and the app would go on saying it's
// up to date, so the release JSON carries a signed `expires` time and the app
// warns once it's passed. A warning, not a refusal: a stale manifest still can't
// move anyone backwards, and refusing would only leave people on an older version.
// Manifests signed before this field existed have no date to go on: still
// accepted, with the same warning. The app's own clock decides, so this doesn't
// help on a computer whose clock is wrong, and a blocked manifest looks like
// being offline.

namespace redvault::update {

inline constexpr std::string_view kProduct = "redvault-desktop";
inline constexpr std::size_t kPublicKeyBytes = 32;
inline constexpr std::size_t kSecretKeyBytes = 64;

class UpdateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Version = std::array<int, 3>;

/// "1.2.3" (optionally "v1.2.3"). Empty for anything else.
std::optional<Version> parseVersion(std::string_view text);

struct Release {
    std::string version;
    std::string url;
    /// Lowercase hex SHA-256 of the installer.
    std::string sha256;
    std::uint64_t size = 0;
    std::string notes;
    /// ISO-8601 UTC, informational.
    std::string published;
    /// ISO-8601 UTC. After this, the manifest may be old news. Empty in manifests from before this field.
    std::string expires;
};

enum class Freshness { Current, Expired, NoExpiry };

struct ManifestCheck {
    /// What verifyManifest returns: the release, if it's newer than this build.
    std::optional<Release> release;
    Freshness freshness = Freshness::Current;
};

/**
 * Verifies a manifest and returns the release it describes if it's newer
 * than `currentVersion`, or empty if this build is already up to date.
 * Throws UpdateError if anything doesn't check out: bad signature, wrong
 * product, malformed fields, or an installer URL outside `allowedUrlPrefix`.
 */
std::optional<Release> verifyManifest(std::string_view manifestJson, crypto::ByteView publicKey,
                                      std::string_view currentVersion, std::string_view allowedUrlPrefix);

/// verifyManifest, and how fresh the manifest is at `nowMillis`. An expiry that can't be read is an UpdateError.
ManifestCheck checkManifest(std::string_view manifestJson, crypto::ByteView publicKey, std::string_view currentVersion,
                            std::string_view allowedUrlPrefix, std::int64_t nowMillis);

/**
 * Where this app checks for and downloads updates (docs/tor-by-default.md). The
 * manifest and installers are the same files for everyone (there is one app):
 * - talking to the normal server: the clear-internet manifest and download folder;
 * - talking to an onion service (every Tor-only build): that onion service's copy
 *   of the same files, over Tor, and never the normal internet. The signed manifest
 *   still names the published (clear-internet) folder; installerUrl moves it over.
 * A Tor-only build whose server isn't a v3 onion service, an address ending in
 * .onion that isn't one, or an address that can't be read: no route, nothing is sent.
 */
struct UpdateRoute {
    std::string manifestUrl;
    /// Where the installer is fetched from: `clearDownloadPrefix`, or the onion service's copy of that folder.
    std::string downloadPrefix;
    bool viaTor = false;
};
std::optional<UpdateRoute> updateRoute(std::string_view apiBase, bool torOnlyBuild, std::string_view clearManifestUrl,
                                       std::string_view clearDownloadPrefix);

/// Whether this app may check for updates at all (updateRoute has an answer).
bool updatesPermitted(std::string_view apiBase, bool torOnlyBuild);

struct UpdateSource {
    /// The published (clear-internet) manifest; over Tor, its copy on the onion service is fetched instead (updateRoute).
    std::string manifestUrl;
    crypto::Bytes publicKey;
    std::string currentVersion;
    /// The published download folder the signed installer URL must be in.
    std::string allowedUrlPrefix;
    std::string apiBase;
    bool torOnlyBuild = false;
    std::int64_t nowMillis = 0;
};

/// Fetches and checks the manifest along updateRoute. Sends nothing at all without a route. Throws net::NetworkError or UpdateError.
ManifestCheck fetchManifest(net::HttpTransport& transport, const UpdateSource& source);

/// Where to download `release` from on `route`: its signed URL moved into `route.downloadPrefix`. Empty if it isn't in `clearDownloadPrefix`.
std::string installerUrl(const Release& release, const UpdateRoute& route, std::string_view clearDownloadPrefix);

/// Builds and signs a manifest for `release` (used by the release tool, never by the app).
std::string signManifest(const Release& release, crypto::ByteView secretKey);

/// Whether the downloaded installer is exactly the one the manifest describes (size, then SHA-256).
bool installerMatches(const std::filesystem::path& file, const Release& release);

struct SigningKeyPair {
    crypto::Bytes publicKey;
    crypto::SecretBytes secretKey;
};

SigningKeyPair generateSigningKey();

/**
 * The release key at rest: the Ed25519 secret key encrypted under a
 * passphrase (Argon2id + XChaCha20-Poly1305, the same primitives as the
 * account key), so a copied key file alone can't sign anything.
 */
std::string sealSigningKey(const SigningKeyPair& keyPair, std::string_view passphrase);

/// Throws UpdateError on a wrong passphrase or a damaged file.
SigningKeyPair openSigningKey(std::string_view keyFileJson, std::string_view passphrase);

}  // namespace redvault::update
