// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QString>

#include <string>

namespace redvault::app {

/**
 * A picture file as a profile or channel photo: upright, a centered square,
 * 128 px, JPEG at 85%, base64 (what the upload endpoints take). Throws
 * api::UserFacingError for anything that isn't a usable image. Blocks: call
 * it off the UI thread.
 */
std::string avatarJpegBase64(const QString& path);

}  // namespace redvault::app
