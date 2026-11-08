// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/data/sign_in.hpp"

#include <mutex>
#include <set>
#include <utility>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

namespace {

constexpr const char* kDowngradeRefused = crypto::kSignInDetailsWrong;

std::mutex pinsMutex;
std::set<std::string> memoryPins;

SignInPins& pins() {
    static SignInPins current{
        [](const std::string& handle) {
            const std::lock_guard lock(pinsMutex);
            return memoryPins.count(handle) > 0;
        },
        [](const std::string& handle) {
            const std::lock_guard lock(pinsMutex);
            memoryPins.insert(handle);
        },
    };
    return current;
}

/// The server ignores case in usernames, so a pin has to as well.
std::string trimmedHandle(const std::string& handle) {
    return util::encodeUtf8(util::trim(util::decodeUtf8(handle)));
}

std::string pinName(const std::string& handle) {
    return util::toLowerAscii(trimmedHandle(handle));
}

/// Pins from before this were kept under the spelling that was typed, so that one still counts.
bool isPinned(const std::string& handle) {
    return pins().isPinned(pinName(handle)) || pins().isPinned(trimmedHandle(handle));
}

}  // namespace

crypto::KdfSettings kdfSettings(const std::optional<std::int64_t>& ops, const std::optional<std::int64_t>& mem) {
    return ops && mem ? crypto::KdfSettings{*ops, *mem} : crypto::kKdfInteractive;
}

void setSignInPins(SignInPins value) {
    pins() = std::move(value);
}

void pinSignInKeys(const std::string& handle) {
    pins().pin(pinName(handle));
}

void moveSignInPin(const std::string& oldHandle, const std::string& newHandle) {
    if (isPinned(oldHandle)) pinSignInKeys(newHandle);
}

Credential credentialFor(api::ApiClient& api, const std::string& handle, const std::string& password) {
    const auto pre = api.preLogin({.username = handle});
    const auto salt = crypto::base64Decode(pre.kdfSalt);
    std::string signInKey;
    try {
        // Refused before anything is derived or sent, if the server asks for settings out of range.
        signInKey = crypto::deriveSignInKey(password, salt, kdfSettings(pre.kdfOps, pre.kdfMem));
    } catch (const crypto::UnsafeKdfError& e) {
        throw api::UserFacingError(e.what());
    }
    if (pre.authVersion == 2) return Credential{.password = std::move(signInKey), .authVersion = 2, .upgradeAuthKey = std::nullopt};
    if (isPinned(handle)) throw api::UserFacingError(kDowngradeRefused);
    if (pre.upgrade) return Credential{.password = password, .authVersion = std::nullopt, .upgradeAuthKey = std::move(signInKey)};
    return Credential{.password = password, .authVersion = std::nullopt, .upgradeAuthKey = std::nullopt};
}

std::string passwordProof(api::ApiClient& api, const std::string& handle, const std::string& password) {
    return credentialFor(api, handle, password).password;
}

Credential newCredential(api::ApiClient& api, const std::string& handle, const std::string& password, crypto::ByteView kdfSalt) {
    const auto pre = api.preLogin({.username = handle});
    if (pre.authVersion == 2 || pre.upgrade) {
        return Credential{.password = crypto::deriveSignInKey(password, kdfSalt, crypto::kKdfCurrent), .authVersion = 2, .upgradeAuthKey = std::nullopt};
    }
    if (isPinned(handle)) throw api::UserFacingError(kDowngradeRefused);
    return Credential{.password = password, .authVersion = std::nullopt, .upgradeAuthKey = std::nullopt};
}

}  // namespace redvault::data
