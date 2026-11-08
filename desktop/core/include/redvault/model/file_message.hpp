// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Port of android/.../data/model/FileDescriptor.kt (ui/src/lib/fileMessage.ts):
// the descriptor that rides inside an ordinary E2EE message, so the file
// key, real filename, type and size are only ever readable by the chat's
// participants. Everything parsed from a received descriptor is untrusted
// input from the other participant.

namespace redvault::model {

struct FileEncryptionInfo {
    std::string algorithm;
    int version = 0;
    /// base64 secretstream header.
    std::string header;
    int chunkSize = 0;

    bool operator==(const FileEncryptionInfo&) const = default;
};

struct FileDescriptor {
    std::string type = "file";
    int version = 0;
    std::string objectId;
    /// base64: the 256-bit file key.
    std::string fileKey;
    std::string filename;
    std::string mimeType;
    std::int64_t originalSize = 0;
    std::int64_t encryptedSize = 0;
    /// hex SHA-256 of the ciphertext; the recipient checks it before trusting anything.
    std::string ciphertextSha256;
    FileEncryptionInfo encryption;

    bool operator==(const FileDescriptor&) const = default;
};

namespace file_message {

/// A NUL can't be typed into the composer, so a text message can't impersonate a file.
inline const std::string kPrefix = std::string("\0rv-file:", 9);

bool isFileMessage(std::string_view plaintext);

std::string serialize(const FileDescriptor& descriptor);

/// Parses and strictly validates a received descriptor; empty for anything malformed.
std::optional<FileDescriptor> parse(std::string_view plaintext);

/**
 * Makes a sender-supplied filename safe to show and to save under: keeps
 * only the last path component (no traversal), strips control, bidi and
 * zero-width characters (so "invoice<U+202E>gpj.exe" can't render as
 * "invoiceexe.jpg"), leading dots and Windows-reserved names, and caps the
 * length while keeping the extension.
 *
 * Unlike Android it doesn't NFC-normalize first (no ICU in the core); that
 * only affects how a decomposed name is stored, not its safety.
 */
std::string sanitizeFilename(std::string_view name);

/**
 * Whether a file someone sent may be opened with one click. On a desktop "open"
 * can mean "run", and the list of what can run is long and keeps growing, so this
 * is an allow-list: pictures, audio, video, plain text and PDF. Everything else,
 * and a name with no extension, can only be saved. It looks at the sanitized name
 * because that's the name the file gets on disk, and the operating system decides
 * what to do from there.
 */
bool opensInertly(std::string_view filename);

/// "512 B", "3.4 KB", "12 MB", like the Android client.
std::string formatBytes(std::int64_t bytes);

}  // namespace file_message

}  // namespace redvault::model
