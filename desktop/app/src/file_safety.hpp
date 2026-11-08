// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QString>

#include <filesystem>
#include <fstream>

#include "redvault/model/file_message.hpp"

namespace redvault::app::file_safety {

/// Only an inert type opens with one click; see model::file_message::opensInertly. Everything else is save-only.
inline bool isSafeToOpen(const QString& filename) {
    return model::file_message::opensInertly(filename.toStdString());
}

/**
 * Tags a file as downloaded from the internet (the Mark of the Web), so
 * SmartScreen and Office's Protected View treat it like any other download.
 * Best effort: needs NTFS; silently does nothing elsewhere.
 */
inline void markFromInternet(const std::filesystem::path& file) {
#ifdef _WIN32
    std::filesystem::path stream = file;
    stream += L":Zone.Identifier";
    std::ofstream out(stream, std::ios::binary | std::ios::trunc);
    out << "[ZoneTransfer]\r\nZoneId=3\r\n";
#else
    (void)file;
#endif
}

}  // namespace redvault::app::file_safety
