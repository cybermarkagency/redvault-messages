// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "redvault/crypto/bytes.hpp"

namespace redvault::data {

/**
 * One message waiting for Tor (docs/tor-routing-plan.md, "fail-closed
 * outbox"). Its `id` is the clientMessageId sent with it, so however often it
 * is retried the server stores it once (db/migrations/033).
 */
struct OutboxItem {
    enum class Kind { Text, File };

    std::string id;
    std::string conversationId;
    Kind kind = Kind::Text;
    /// Text: the message. File: the name shown for it.
    std::string text;
    /// Text replies: the message replied to, its sender, its preview, and whether it was ours.
    std::optional<std::string> replyToId;
    std::string replyToSender;
    std::string replyPreview;
    bool replyFromMe = false;
    /// File: its type.
    std::string mimeType;
    std::int64_t createdAtMs = 0;
    /// Strictly increasing in the order things were queued (several can share a millisecond): the send order.
    std::int64_t order = 0;
    int attempts = 0;
    std::string lastError;
    /// The server refused it for good (blocked, chat gone, too large): not retried until the person says so.
    bool held = false;
};

/**
 * Messages and files queued while Tor is unavailable, sealed on disk so they
 * survive a restart without ever sitting there readable:
 * - each item is a JSON record sealed with XChaCha20-Poly1305 (the item's id
 *   is bound in as associated data, so files can't be swapped around);
 * - a queued file is a sealed copy (libsodium secretstream, 64 KiB chunks), so
 *   the original can move or change after it was queued;
 * - the key comes from deriveOutboxKey: only a signed-in session can read it.
 * Nothing is ever sent from here: the app drains it through the normal
 * (Tor-only) send path once the gateway reports it is connected.
 *
 * Thread-safe.
 */
class Outbox {
public:
    Outbox(std::filesystem::path directory, crypto::SecretBytes key);

    /// A new random clientMessageId (128 bits, base64url): what the server's /^[A-Za-z0-9_-]{16,64}$/ accepts.
    static std::string newClientMessageId();

    OutboxItem addText(const std::string& conversationId, const std::string& text,
                       const std::optional<std::string>& replyToId = std::nullopt, const std::string& replyPreview = {},
                       bool replyFromMe = false, const std::string& replyToSender = {});
    /// Seals a copy of `source`. Throws std::runtime_error if it can't be read.
    OutboxItem addFile(const std::string& conversationId, const std::filesystem::path& source,
                       const std::string& displayName, const std::string& mimeType);

    /// Oldest first. Records that fail to open (tampered, or another account's key) are left alone and skipped.
    std::vector<OutboxItem> items() const;
    std::size_t count() const;
    std::size_t countFor(const std::string& conversationId) const;

    /// Decrypts a queued file's sealed copy to `target`, for sending. Throws if it was tampered with.
    void openFile(const OutboxItem& item, const std::filesystem::path& target) const;

    /// `hold`: the failure is permanent; the item waits for release() or remove().
    void recordAttempt(const std::string& id, const std::string& error, bool hold = false);
    /// Try a held item again.
    void release(const std::string& id);
    /// Sent (or given up on): the record and any sealed file are deleted.
    void remove(const std::string& id);

private:
    std::filesystem::path itemPath(const std::string& id) const;
    std::filesystem::path filePath(const std::string& id) const;
    void write(const OutboxItem& item) const;
    std::optional<OutboxItem> read(const std::filesystem::path& path) const;

    std::filesystem::path directory_;
    crypto::SecretBytes key_;
    mutable std::mutex mutex_;
};

/// The outbox key: crypto_kdf_derive_from_key(32, 1, "rvoutbx1", account private key). Same on every client.
crypto::SecretBytes deriveOutboxKey(const crypto::SecretBytes& accountPrivateKey);

}  // namespace redvault::data
