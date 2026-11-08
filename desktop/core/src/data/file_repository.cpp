// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/data/file_repository.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>
#include <thread>
#include <utility>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/file_crypto.hpp"
#include "redvault/util/private_files.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

namespace fs = std::filesystem;
namespace file_message = model::file_message;
using api::ApiError;
using api::TransferInterrupted;
using api::UserFacingError;

namespace {

constexpr int kMaxAttempts = 4;

// Refusals the server words for the user (plan, storage, rate limits): shown as-is.
constexpr std::array<std::string_view, 5> kQuotaCodes = {
    "pro_required", "upload_quota_exceeded", "too_many_pending_uploads", "rate_limited", "storage_limit_exceeded"};

/// UTF-8 <-> path without going through the ANSI code page on Windows.
fs::path pathFromUtf8(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8FromPath(const fs::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string megabytes(std::int64_t bytes) {
    return std::to_string(bytes / (1024 * 1024));
}

void removeQuietly(const fs::path& path) {
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

bool contains(const std::string& haystack, std::string_view needle) {
    return util::toLowerAscii(haystack).find(needle) != std::string::npos;
}

/// Turns failures into the web client's user-facing wording (fileErrorMessage in actions.ts).
template <typename F>
auto mapErrors(const std::string& fallback, F&& block) -> decltype(block()) {
    try {
        return block();
    } catch (const UserFacingError&) {
        throw;
    } catch (const crypto::files::FileDecryptError&) {
        throw UserFacingError("Unable to decrypt this file. It may be corrupted or tampered with.");
    } catch (const ApiError& e) {
        if (e.status() == 401) throw;  // lets the session end itself
        const std::string& code = e.code();
        if (code == "file_deleted") throw UserFacingError("File deleted.");
        if (code == "file_expired") throw UserFacingError("File expired.");
        if (code == "upload_expired") throw UserFacingError("Upload took too long. Try again.");
        if (std::find(kQuotaCodes.begin(), kQuotaCodes.end(), code) != kQuotaCodes.end()) {
            throw UserFacingError(e.what());
        }
        if (code == "file_sharing_disabled") throw UserFacingError("File sharing isn't available on this server.");
        if (code == "network_error") throw TransferInterrupted("Network interrupted. Try again.");
        if (code == "tor_unavailable") throw TransferInterrupted("Tor isn't connected. The file waits until it is.");
        if (e.status() == 400 && contains(e.what(), "too large")) throw UserFacingError("File too large.");
        if (e.status() == 404) throw UserFacingError("File not found.");
        throw UserFacingError(fallback);
    } catch (const net::TorUnavailableError&) {
        throw TransferInterrupted("Tor isn't connected. The file waits until it is.");
    } catch (const net::NetworkError&) {
        throw TransferInterrupted("Network interrupted. Try again.");
    } catch (const std::ios_base::failure&) {
        throw UserFacingError(fallback);
    } catch (const fs::filesystem_error&) {
        throw UserFacingError(fallback);
    }
}

}  // namespace

FileRepository::FileRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<ConversationRepository> conversations,
                               fs::path cacheDir, Sleep sleep)
    : api_(std::move(api)),
      conversations_(std::move(conversations)),
      cacheDir_(std::move(cacheDir)),
      sleep_(sleep ? std::move(sleep) : Sleep([](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); })) {}

template <typename F>
auto FileRepository::retryTransient(F&& block) -> decltype(block()) {
    // API steps retry server hiccups; the final send is idempotent server-side, so a retry can't duplicate it.
    int attempt = 0;
    while (true) {
        try {
            return block();
        } catch (const ApiError& e) {
            const bool transient = e.code() == "network_error" || e.status() >= 500 ||
                                   (e.status() == 409 && e.code() == "upload_incomplete");
            if (!transient || ++attempt >= kMaxAttempts) throw;
            sleep_(std::chrono::milliseconds(400LL << (attempt - 1)));
        }
    }
}

Message FileRepository::send(const ConversationMeta& meta, const fs::path& file, const SendStageFn& onStage) {
    return send(meta, file, utf8FromPath(file.filename()), guessMimeType(file), onStage);
}

Message FileRepository::send(const ConversationMeta& meta, const fs::path& file, const std::string& displayName,
                             const std::string& mimeType, const SendStageFn& onStage,
                             const std::optional<std::string>& clientMessageId) {
    return mapErrors("Upload failed. Try again.", [&] {
        const model::FileDescriptor descriptor = upload(meta.id, file, displayName, mimeType, onStage);
        if (onStage) onStage(FileSendStage::Sending);
        return retryTransient([&] { return conversations_->sendFile(meta, descriptor, clientMessageId); });
    });
}

model::FileDescriptor FileRepository::uploadForDeviceKeys(const std::string& conversationId, const fs::path& file,
                                                          const SendStageFn& onStage) {
    return mapErrors("Upload failed. Try again.", [&] {
        model::FileDescriptor descriptor =
            upload(conversationId, file, utf8FromPath(file.filename()), guessMimeType(file), onStage);
        retryTransient([&] { return api_->releaseFile(descriptor.objectId); });
        if (onStage) onStage(FileSendStage::Sending);
        return descriptor;
    });
}

model::FileDescriptor FileRepository::upload(const std::string& conversationId, const fs::path& file,
                                             const std::string& displayName, const std::string& mimeType,
                                             const SendStageFn& onStage) {
    if (!config_) config_ = api_->fileConfig();
    const auto& limits = *config_;
    const auto tooLarge = [&] {
        return UserFacingError("File too large. The limit is " + megabytes(limits.maxFileSize) + " MB.");
    };
    std::error_code sizeError;
    const auto sizeHint = fs::file_size(file, sizeError);
    if (!sizeError && static_cast<std::int64_t>(sizeHint) > limits.maxFileSize) throw tooLarge();

    if (onStage) onStage(FileSendStage::Encrypting);
    const fs::path outgoing = cacheDir_ / "outgoing";
    util::makePrivateDir(outgoing);
    const fs::path ciphertext = outgoing / util::randomUuid();
    struct Cleanup {
        fs::path path;
        ~Cleanup() { removeQuietly(path); }
    } cleanup{ciphertext};

    crypto::files::EncryptedFile encrypted;
    {
        std::ifstream input(file, std::ios::binary);
        if (!input) throw UserFacingError("Couldn't read that file.");
        util::makePrivateFile(ciphertext);
        std::ofstream output(ciphertext, std::ios::binary | std::ios::trunc);
        if (!output) throw UserFacingError("Upload failed. Try again.");
        encrypted = crypto::files::encrypt(input, output);
        output.flush();
        if (!output || input.bad()) throw UserFacingError("Couldn't read that file.");
    }
    if (static_cast<std::int64_t>(encrypted.plaintextSize) > limits.maxFileSize) throw tooLarge();

    if (onStage) onStage(FileSendStage::Uploading);
    const auto auth = retryTransient([&] {
        return api_->requestFileUpload({.conversationId = conversationId,
                                        .encryptedSize = static_cast<std::int64_t>(encrypted.encryptedSize),
                                        .ciphertextSha256 = encrypted.ciphertextSha256});
    });
    if (auth.upload.method != "PUT") throw UserFacingError("Upload failed. Try again.");
    transfer(auth.upload, "PUT", [&](const net::HttpRequest& request) {
        std::ifstream body(ciphertext, std::ios::binary);
        if (!body) throw UserFacingError("Upload failed. Try again.");
        return api_->transport().sendStreaming(request, {.upload = &body, .uploadSize = encrypted.encryptedSize});
    });
    retryTransient([&] { return api_->completeFileUpload(auth.objectId); });

    return model::FileDescriptor{
        .type = "file",
        .version = crypto::files::kFormatVersion,
        .objectId = util::toLowerAscii(auth.objectId),
        .fileKey = crypto::base64Encode(encrypted.key),
        .filename = file_message::sanitizeFilename(displayName),
        // Informational only, never a security decision.
        .mimeType = mimeType,
        .originalSize = static_cast<std::int64_t>(encrypted.plaintextSize),
        .encryptedSize = static_cast<std::int64_t>(encrypted.encryptedSize),
        .ciphertextSha256 = encrypted.ciphertextSha256,
        .encryption =
            model::FileEncryptionInfo{
                .algorithm = std::string(crypto::files::kAlgorithm),
                .version = crypto::files::kFormatVersion,
                .header = crypto::base64Encode(encrypted.header),
                .chunkSize = static_cast<int>(crypto::files::kChunkBytes),
            },
    };
}

fs::path FileRepository::download(const model::FileDescriptor& descriptor, const DownloadStageFn& onStage) {
    return mapErrors("Download failed. Try again.", [&] {
        if (onStage) onStage(FileDownloadStage::Downloading);
        const auto target = retryTransient([&] { return api_->fileDownload(descriptor.objectId); }).download;
        if (target.method != "GET") throw UserFacingError("Download failed. Try again.");

        // One folder per download, so two files with the same name never collide.
        const fs::path folder = cacheDir_ / "decrypted" / util::randomUuid();
        util::makePrivateDir(folder);
        const fs::path ciphertext = folder / ".ciphertext";
        const fs::path output = folder / pathFromUtf8(file_message::sanitizeFilename(descriptor.filename));
        try {
            transfer(target, "GET", [&](const net::HttpRequest& request) {
                util::makePrivateFile(ciphertext);
                std::ofstream sink(ciphertext, std::ios::binary | std::ios::trunc);
                if (!sink) throw UserFacingError("Download failed. Try again.");
                return api_->transport().sendStreaming(request, {.download = &sink});
            });

            if (onStage) onStage(FileDownloadStage::Decrypting);
            {
                std::ifstream input(ciphertext, std::ios::binary);
                util::makePrivateFile(output);
                std::ofstream out(output, std::ios::binary | std::ios::trunc);
                if (!input || !out) throw UserFacingError("Download failed. Try again.");
                const auto key = crypto::base64Decode(descriptor.fileKey);
                crypto::files::decrypt(input, out, key, crypto::base64Decode(descriptor.encryption.header),
                                       static_cast<std::uint64_t>(descriptor.encryptedSize),
                                       descriptor.ciphertextSha256);
                out.flush();
                if (!out) throw UserFacingError("Download failed. Try again.");
            }
            fs::remove(ciphertext);
            return output;
        } catch (...) {
            // Never leave partially decrypted output (or ciphertext) behind.
            removeQuietly(folder);
            throw;
        }
    });
}

api::FileStorageUsageDto FileRepository::storageUsage() {
    return api_->fileStorageUsage();
}

void FileRepository::clearCache() {
    removeQuietly(cacheDir_ / "decrypted");
    removeQuietly(cacheDir_ / "outgoing");
}

net::HttpResponse FileRepository::transfer(const api::TransferTargetDto& target, const std::string& method,
                                           const std::function<net::HttpResponse(const net::HttpRequest&)>& perform) {
    return transferTarget(*api_, target, method, "/v1/files/", perform, sleep_);
}

std::string FileRepository::guessMimeType(const fs::path& file) {
    static const std::array<std::pair<std::string_view, std::string_view>, 24> kTypes = {{
        {".jpg", "image/jpeg"},        {".jpeg", "image/jpeg"},     {".png", "image/png"},
        {".gif", "image/gif"},         {".webp", "image/webp"},     {".heic", "image/heic"},
        {".pdf", "application/pdf"},   {".txt", "text/plain"},      {".csv", "text/csv"},
        {".zip", "application/zip"},   {".mp4", "video/mp4"},       {".mov", "video/quicktime"},
        {".webm", "video/webm"},       {".mp3", "audio/mpeg"},      {".m4a", "audio/mp4"},
        {".ogg", "audio/ogg"},         {".wav", "audio/wav"},       {".json", "application/json"},
        {".doc", "application/msword"},
        {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".xls", "application/vnd.ms-excel"},
        {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {".ppt", "application/vnd.ms-powerpoint"},
        {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    }};
    const std::string extension = util::toLowerAscii(utf8FromPath(file.extension()));
    for (const auto& [ext, type] : kTypes) {
        if (ext == extension) return std::string(type);
    }
    return "application/octet-stream";
}

}  // namespace redvault::data
