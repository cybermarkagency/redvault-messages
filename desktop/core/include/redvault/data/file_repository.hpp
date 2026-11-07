#pragma once

#include "redvault/data/transfer.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "redvault/api/api_client.hpp"
#include "redvault/data/chat_models.hpp"
#include "redvault/data/conversation_repository.hpp"
#include "redvault/model/file_message.hpp"

namespace redvault::data {

enum class FileSendStage { Encrypting, Uploading, Sending };
enum class FileDownloadStage { Downloading, Decrypting };

/**
 * Port of android/.../data/FileRepository.kt (sendDmFile/downloadDmFile in
 * ui/src/state/actions.ts and ui/src/lib/fileTransfer.ts). Only ciphertext
 * ever leaves the device, and plaintext is only ever written under
 * `cacheDir`, which clearCache() empties when the session ends. Both
 * directions stream through temporary ciphertext files, so a large file is
 * never held in memory whole.
 *
 * Only "proxy" transfer targets are followed: a path on our own API, so the
 * storage provider never sees the user's IP address or when they transfer
 * anything. A "direct" target (a presigned URL at the storage provider) is
 * refused outright.
 *
 * Failures a person can act on throw api::UserFacingError with the web
 * client's wording; a 401 ApiError passes through so the session can end.
 * Blocking; call from a worker thread.
 */
class FileRepository {
public:
    using Sleep = std::function<void(std::chrono::milliseconds)>;
    using SendStageFn = std::function<void(FileSendStage)>;
    using DownloadStageFn = std::function<void(FileDownloadStage)>;

    /// `sleep` paces retries; tests pass a no-op.
    FileRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<ConversationRepository> conversations,
                   std::filesystem::path cacheDir, Sleep sleep = {});

    /// Encrypts, uploads and sends a file from disk. Returns only once the message itself is stored.
    Message send(const ConversationMeta& meta, const std::filesystem::path& file, const SendStageFn& onStage = {});

    /// send() with an explicit display name and MIME type (a voice note recorded by the app, say).
    Message send(const ConversationMeta& meta, const std::filesystem::path& file, const std::string& displayName,
                 const std::string& mimeType, const SendStageFn& onStage = {},
                 const std::optional<std::string>& clientMessageId = std::nullopt);

    /**
     * For a device-key chat (protocol 3, docs/multi-device-threat-model.md 9.3):
     * encrypts, uploads and releases the file, because no server message will
     * point at it. Sends nothing: the caller puts the descriptor, with the file's
     * key, into the envelope. Never touches the conversation repository, so that
     * can be null on an instance used only for this and download().
     */
    model::FileDescriptor uploadForDeviceKeys(const std::string& conversationId, const std::filesystem::path& file,
                                              const SendStageFn& onStage = {});

    /**
     * Fetches a received file's ciphertext, verifies it and decrypts it into
     * `cacheDir`. Returns the plaintext file's path; nothing decrypted is
     * ever sent anywhere.
     */
    std::filesystem::path download(const model::FileDescriptor& descriptor, const DownloadStageFn& onStage = {});

    /// Your own file storage in use vs. your plan's limit.
    api::FileStorageUsageDto storageUsage();

    /// Removes every decrypted file and leftover transfer. Call when the session ends.
    void clearCache();

    /// A best guess from the extension; informational only (the descriptor's mimeType is never a security decision).
    static std::string guessMimeType(const std::filesystem::path& file);

    /// What a voice message is called and typed as inside its (encrypted) descriptor; the web plays audio/mp4.
    static inline const std::string kVoiceFilename = "Voice message.m4a";
    static inline const std::string kVoiceMimeType = "audio/mp4";

private:
    /// Encrypts and uploads; the file is complete on the server but nothing refers to it yet. Call inside mapErrors.
    model::FileDescriptor upload(const std::string& conversationId, const std::filesystem::path& file,
                                 const std::string& displayName, const std::string& mimeType, const SendStageFn& onStage);

    /**
     * Runs one transfer with retries on network errors, 5xx and 429.
     * `perform` sends the prepared request (URL, headers, cookie) and opens
     * its own streams, so every attempt starts from a clean file.
     */
    net::HttpResponse transfer(const api::TransferTargetDto& target, const std::string& method,
                               const std::function<net::HttpResponse(const net::HttpRequest&)>& perform);

    template <typename F>
    auto retryTransient(F&& block) -> decltype(block());

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<ConversationRepository> conversations_;
    std::filesystem::path cacheDir_;
    Sleep sleep_;
    std::optional<api::FileConfigDto> config_;
};

}  // namespace redvault::data
