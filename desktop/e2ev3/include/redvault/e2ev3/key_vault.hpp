// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "redvault/crypto/bytes.hpp"

// Where a device's 32-byte storage key lives (docs/multi-device-threat-model.md
// section 5). The engine's state is always encrypted under that key; this
// protects the key itself, so the state file alone is useless.
//
//   Windows  DPAPI (CryptProtectData, current user). Anything running as the
//            same Windows user can unprotect it: that's the limit.
//   Linux    The Secret Service (GNOME Keyring, KWallet) through libsecret,
//            loaded at run time so the app starts without it.
//   Fallback A file only the user can read (0600), when there is no Secret
//            Service. Anything that can read the user's files can read the
//            key, so Settings shows this plainly.
//
// Nothing here ever logs or returns a key except to the caller that asked.

namespace redvault::e2ev3 {

enum class Protection { Dpapi, SecretService, File };

class KeyVault {
public:
    virtual ~KeyVault() = default;
    virtual Protection protection() const = 0;
    /// Keeps `secret` under `id` (any text; "<userId>/<deviceId>"), replacing an earlier one.
    virtual void put(const std::string& id, crypto::ByteView secret) = 0;
    /// The secret, or nothing when there is none or it can't be unprotected any more (a reset Windows profile, a new keyring).
    virtual std::optional<crypto::SecretBytes> get(const std::string& id) = 0;
    virtual void erase(const std::string& id) = 0;
};

/// Windows only (null elsewhere): DPAPI blobs in files under `dir`.
std::unique_ptr<KeyVault> makeDpapiVault(const std::filesystem::path& dir);
/// Linux only (null elsewhere, and when libsecret or a running Secret Service can't be found).
std::unique_ptr<KeyVault> makeSecretServiceVault();
/// Always available: files under `dir`, readable by the user only.
std::unique_ptr<KeyVault> makeFileVault(const std::filesystem::path& dir);
/// The best this machine has: DPAPI on Windows, else the Secret Service, else a file. `dir` is for the file-based kinds.
std::unique_ptr<KeyVault> makeDefaultVault(const std::filesystem::path& dir);

/// One sentence for Settings about what protects the key; for File it states the weaker guarantee.
std::string describeProtection(Protection protection);
/// Whether Settings should show this protection as a warning.
inline bool isWeakProtection(Protection protection) { return protection == Protection::File; }

}  // namespace redvault::e2ev3
