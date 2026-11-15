// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

// Where the desktop app looks for updates, and the public half of the
// offline release key that must have signed them. Public on purpose: it
// can only verify. `redvault_release keygen` writes the key line below;
// while it's empty, the app never checks for or installs updates.

namespace redvault::app::update_config {

inline constexpr const char* kManifestUrl = "https://www.redvaultsystems.com/redvaultmessages/desktop/latest.json";
/// Installers are only ever downloaded from here.
inline constexpr const char* kDownloadPrefix = "https://www.redvaultsystems.com/redvaultmessages/desktop/";
/// Linux: one manifest per way the app is installed (`redvault_release sign-linux` writes both).
inline constexpr const char* kLinuxAppImageManifestUrl = "https://www.redvaultsystems.com/redvaultmessages/desktop/linux-appimage-latest.json";
inline constexpr const char* kLinuxDebManifestUrl = "https://www.redvaultsystems.com/redvaultmessages/desktop/linux-deb-latest.json";

// redvault-release-public-key
inline constexpr const char* kPublicKeyB64 = "4GOi/rqNFDftNAHc8vCPKL7cukGzVAVXHLUD2ckyWi0=";

}  // namespace redvault::app::update_config
