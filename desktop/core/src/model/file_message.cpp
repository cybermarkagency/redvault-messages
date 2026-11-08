// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/model/file_message.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <regex>

#include <nlohmann/json.hpp>

#include "redvault/crypto/file_crypto.hpp"
#include "redvault/util/text.hpp"

namespace redvault::model::file_message {

namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::size_t kMaxFilename = 150;
constexpr std::int64_t kMaxSafeInteger = 9'007'199'254'740'991LL;

bool isHidden(char32_t c) {
    // Control characters, bidi overrides/isolates, zero-width characters, BOM.
    return c <= 0x1F || (c >= 0x7F && c <= 0x9F) || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2060 && c <= 0x2069) || c == 0xFEFF;
}

bool matches(const std::string& text, const std::regex& pattern) {
    return std::regex_match(text, pattern);
}

const std::regex kUuid("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", std::regex::icase);
const std::regex kHex64("^[0-9a-f]{64}$");
const std::regex kMime("^[a-z0-9][a-z0-9!#$&^_.+-]{0,63}/[a-z0-9][a-z0-9!#$&^_.+-]{0,127}$", std::regex::icase);

// Peer-supplied values of any length are checked with loops, not std::regex: libstdc++'s matcher recurses once per
// character of an unbounded repeat, so a long enough value overflows the stack.

/// ^[A-Za-z0-9+/]+={0,2}$
bool isBase64Text(std::string_view text) {
    std::size_t end = text.size();
    for (int pad = 0; pad < 2 && end > 0 && text[end - 1] == '='; ++pad) --end;
    if (end == 0) return false;
    return std::all_of(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(end), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
    });
}

/// CON, PRN, AUX, NUL, CONIN$, CONOUT$, COM0-9 and LPT0-9 (also with superscript 1, 2 or 3), alone or before any extension.
bool isWindowsReserved(std::u32string_view name) {
    std::u32string stem(name.substr(0, std::min(name.find(U'.'), name.size())));
    while (!stem.empty() && stem.back() == U' ') stem.pop_back();  // "NUL .txt" is NUL too, as on Android
    if (stem.size() > 7) return false;
    for (auto& c : stem) {
        if (c >= U'A' && c <= U'Z') c = c - U'A' + U'a';
    }
    if (stem == U"con" || stem == U"prn" || stem == U"aux" || stem == U"nul" || stem == U"conin$" || stem == U"conout$") return true;
    if (stem.size() != 4 || !(stem.starts_with(U"com") || stem.starts_with(U"lpt"))) return false;
    const char32_t digit = stem[3];
    return (digit >= U'0' && digit <= U'9') || digit == U'\x00B9' || digit == U'\x00B2' || digit == U'\x00B3';
}

void dropTrailingDotsAndSpaces(std::u32string& name) {
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
}

std::optional<std::string> str(const Json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

std::optional<std::int64_t> integer(const Json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) return std::nullopt;
    return it->get<std::int64_t>();
}

std::optional<std::int64_t> size(const Json& object, const char* key) {
    const auto value = integer(object, key);
    if (!value || *value < 0 || *value > kMaxSafeInteger) return std::nullopt;
    return value;
}

}  // namespace

bool isFileMessage(std::string_view plaintext) {
    return plaintext.starts_with(kPrefix);
}

std::string serialize(const FileDescriptor& d) {
    OrderedJson body;
    body["type"] = d.type;
    body["version"] = d.version;
    body["objectId"] = d.objectId;
    body["fileKey"] = d.fileKey;
    body["filename"] = d.filename;
    body["mimeType"] = d.mimeType;
    body["originalSize"] = d.originalSize;
    body["encryptedSize"] = d.encryptedSize;
    body["ciphertextSha256"] = d.ciphertextSha256;
    OrderedJson encryption;
    encryption["algorithm"] = d.encryption.algorithm;
    encryption["version"] = d.encryption.version;
    encryption["header"] = d.encryption.header;
    encryption["chunkSize"] = d.encryption.chunkSize;
    body["encryption"] = std::move(encryption);
    return kPrefix + body.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::optional<FileDescriptor> parse(std::string_view plaintext) {
    if (!isFileMessage(plaintext)) return std::nullopt;
    const Json d = Json::parse(plaintext.substr(kPrefix.size()), nullptr, false);
    if (d.is_discarded() || !d.is_object()) return std::nullopt;
    const auto encIt = d.find("encryption");
    if (encIt == d.end() || !encIt->is_object()) return std::nullopt;
    const Json& enc = *encIt;

    const auto objectId = str(d, "objectId");
    if (!objectId || !matches(*objectId, kUuid)) return std::nullopt;
    const auto fileKey = str(d, "fileKey");
    if (!fileKey || !isBase64Text(*fileKey)) return std::nullopt;
    const auto filename = str(d, "filename");
    if (!filename || util::utf16Length(util::decodeUtf8(*filename)) > 1024) return std::nullopt;
    const auto mimeType = str(d, "mimeType");
    if (!mimeType) return std::nullopt;
    const auto originalSize = size(d, "originalSize");
    const auto encryptedSize = size(d, "encryptedSize");
    if (!originalSize || !encryptedSize) return std::nullopt;
    const auto sha = str(d, "ciphertextSha256");
    if (!sha || !matches(*sha, kHex64)) return std::nullopt;
    const auto header = str(enc, "header");
    if (!header || !isBase64Text(*header)) return std::nullopt;
    if (str(d, "type") != "file" || integer(d, "version") != crypto::files::kFormatVersion) return std::nullopt;
    if (str(enc, "algorithm") != crypto::files::kAlgorithm ||
        integer(enc, "version") != crypto::files::kFormatVersion) {
        return std::nullopt;
    }
    if (integer(enc, "chunkSize") != static_cast<std::int64_t>(crypto::files::kChunkBytes)) return std::nullopt;

    return FileDescriptor{
        .type = "file",
        .version = crypto::files::kFormatVersion,
        .objectId = util::toLowerAscii(*objectId),
        .fileKey = *fileKey,
        .filename = sanitizeFilename(*filename),
        .mimeType = matches(*mimeType, kMime) ? util::toLowerAscii(*mimeType) : "application/octet-stream",
        .originalSize = *originalSize,
        .encryptedSize = *encryptedSize,
        .ciphertextSha256 = *sha,
        .encryption =
            FileEncryptionInfo{
                .algorithm = std::string(crypto::files::kAlgorithm),
                .version = crypto::files::kFormatVersion,
                .header = *header,
                .chunkSize = static_cast<int>(crypto::files::kChunkBytes),
            },
    };
}

bool opensInertly(std::string_view filename) {
    // Not svg, html, xml or anything else that's really a program in a viewer. wmv and wma can carry scripts.
    static constexpr std::array<std::string_view, 23> kInert = {
        "jpg", "jpeg", "png", "gif", "webp", "bmp", "heic", "avif",  // pictures
        "mp3", "m4a", "aac", "ogg", "oga", "opus", "wav", "flac",    // audio
        "mp4", "m4v", "mov", "webm", "mkv",                          // video
        "txt", "pdf",
    };
    const std::string name = sanitizeFilename(filename);
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    const std::string extension = util::toLowerAscii(std::string_view(name).substr(dot + 1));
    return std::find(kInert.begin(), kInert.end(), extension) != kInert.end();
}

std::string sanitizeFilename(std::string_view name) {
    std::u32string clean = util::decodeUtf8(name);
    // Last path component only.
    if (const auto slash = clean.find_last_of(U"/\\"); slash != std::u32string::npos) clean.erase(0, slash + 1);
    std::erase_if(clean, isHidden);
    for (auto& c : clean) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') c = '_';
    }
    clean = util::trim(util::collapseWhitespace(clean));
    clean.erase(0, clean.find_first_not_of(U'.') == std::u32string::npos ? clean.size() : clean.find_first_not_of(U'.'));
    dropTrailingDotsAndSpaces(clean);

    if (isWindowsReserved(clean)) {
        clean.insert(clean.begin(), U'_');
    }
    if (util::utf16Length(clean) > kMaxFilename) {
        const auto dot = clean.find_last_of(U'.');
        std::u32string ext;
        if (dot != std::u32string::npos && dot > 0 && util::utf16Length(std::u32string_view(clean).substr(dot)) <= 16) {
            ext = clean.substr(dot);
        }
        clean = util::takeUtf16(clean, kMaxFilename - util::utf16Length(ext)) + ext;
        // Cutting can leave a trailing space or dot, which Windows drops when it creates the file.
        dropTrailingDotsAndSpaces(clean);
    }
    return clean.empty() ? "file" : util::encodeUtf8(clean);
}

std::string formatBytes(std::int64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    static constexpr const char* kUnits[] = {"KB", "MB", "GB"};
    double value = static_cast<double>(bytes) / 1024.0;
    std::size_t unit = 0;
    while (value >= 1024 && unit < 2) {
        value /= 1024;
        ++unit;
    }
    char buffer[32];
    if (value < 10) {
        std::snprintf(buffer, sizeof buffer, "%.1f %s", value, kUnits[unit]);
    } else {
        std::snprintf(buffer, sizeof buffer, "%lld %s", static_cast<long long>(std::llround(value)), kUnits[unit]);
    }
    return buffer;
}

}  // namespace redvault::model::file_message
