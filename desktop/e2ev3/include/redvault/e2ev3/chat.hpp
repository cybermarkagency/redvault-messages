// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "redvault/e2ev3/device.hpp"
#include "redvault/e2ev3/send_queue.hpp"
#include "redvault/model/file_message.hpp"

// The protocol-3 chat path on the desktop: docs/multi-device-threat-model.md
// section 9 (reference: ui/src/lib/e2eV3/chat.ts). Text, files, reactions, edits
// and deletes as envelopes over the engine's Olm (one-to-one, with copies for the
// sender's other devices) and Megolm (groups), the receive loop with
// ack-after-persist, and a local history that lives only on this computer,
// encrypted under a key derived from the device's storage key. Behind
// E2E_V3_ENABLED; the app uses it for a chat whose members all have device keys,
// or that is pinned to them (docs/signal-integration/07-step1-default-path.md).

namespace redvault::e2ev3 {

/// What a reaction, edit or delete does to the message `target`.
struct ChatOp {
    std::string t;  // "reaction" | "edit" | "delete"
    std::string target;
    std::string body;  // an edit's new text
    std::string emoji;  // a reaction; empty removes the person's reaction
};

struct ChatReaction {
    std::string userId;
    std::string emoji;
};

struct ChatRecord {
    std::string conversationId;
    std::string id;  // the envelope's client-generated uuid
    std::string from;  // sender's user id
    std::string fromDeviceId;
    std::int64_t ts = 0;  // sender's clock (clamped), ms
    std::int64_t arrivedAt = 0;
    std::string text;
    std::string direction;  // "in" | "out"
    std::string status;  // "sent" | "failed" | "received" | "queued" (waiting for Tor, docs/tor-by-default.md)
    /// A file message: `text` is then empty.
    std::optional<model::FileDescriptor> file;
    /// Set on a reaction, edit or delete. These stay out of what a chat shows (foldChat).
    std::optional<ChatOp> op;
    // Worked out by foldChat from the ops that apply to this message; never stored.
    bool edited = false;
    bool deleted = false;
    std::vector<ChatReaction> reactions;  // by user id, so the order doesn't depend on arrival
    /// A queued one-to-one send: the contact it goes to (empty in a group, and on everything else).
    std::string to;
};

/**
 * Turns stored records into what a chat shows, in any arrival order: only the
 * person who sent a message can edit or delete it, the latest edit and each
 * person's latest reaction count, a delete beats both, and an op whose message
 * hasn't arrived waits. Oldest first by ts, then arrival.
 */
std::vector<ChatRecord> foldChat(const std::vector<ChatRecord>& records);

/// This device's decrypted chat history and per-conversation v3 pins (9.7). One encrypted file, rewritten on each change.
class ChatHistory {
public:
    ChatHistory(std::filesystem::path file, crypto::Bytes key);
    /**
     * False when `(conversationId, id)` is already there (a retry or an own-device copy).
     * Each write also erases messages their sender deleted (9.3): the text and file key,
     * and the edits and reactions that referred to them. A delete that arrived first
     * means the message's text never reaches the disk at all.
     */
    bool add(const ChatRecord& record);
    bool setStatus(const std::string& conversationId, const std::string& id, const std::string& status);
    /// Drops one record (a failed message the person retried).
    bool remove(const std::string& conversationId, const std::string& id);
    /// What the chat shows: foldChat over the stored records.
    std::vector<ChatRecord> list(const std::string& conversationId) const;
    /// The stored records as they are, hidden ops included. Oldest first by ts, then arrival.
    std::vector<ChatRecord> records(const std::string& conversationId) const;
    /// Every "queued" record in every conversation, in the order they were sent.
    std::vector<ChatRecord> queued() const;
    /// A chat that has used v3 stays on v3 until the person says otherwise (9.1 item 5).
    bool pinned(const std::string& conversationId) const;
    void pin(const std::string& conversationId);
    void unpin(const std::string& conversationId);

private:
    void load();
    void save() const;
    void eraseDeleted(const std::string& conversationId);
    std::filesystem::path file_;
    crypto::Bytes key_;
    mutable std::mutex mutex_;
    std::vector<ChatRecord> records_;
    std::vector<std::string> pins_;
};

/// A text envelope (9.3).
struct Envelope {
    int v = 1;
    std::string t = "text";
    std::string id;
    std::string c;
    std::int64_t ts = 0;
    std::string body;
};
std::string encodeEnvelope(const Envelope& envelope);
/// The envelope for a reaction, edit or delete.
std::string encodeOp(const std::string& conversationId, const ChatOp& op, const std::string& id, std::int64_t ts);
/// The envelope for a file. This is the only place the file's key travels.
std::string encodeFile(const std::string& conversationId, const model::FileDescriptor& file, const std::string& id, std::int64_t ts);

struct DecodedEnvelope {
    enum class Kind {
        Message,  // text or file
        Op,  // reaction, edit or delete
        Newer,  // a newer envelope version: keep the item and ask for an update
        Ignore,  // an unknown type, or bound to another conversation: nothing to show, the item is acknowledged
    };
    Kind kind = Kind::Ignore;
    std::string id;
    std::int64_t ts = 0;
    std::string text;
    std::optional<model::FileDescriptor> file;
    std::optional<ChatOp> op;
    std::string reason;  // why it was ignored
};
/// Validates an envelope against the mailbox item that carried it (9.3).
DecodedEnvelope decodeEnvelope(const std::string& plaintext, const std::string& conversationId, std::int64_t nowMs = 0);

struct SendOutcome {
    std::string id;
    int delivered = 0;
    /// Devices that got no copy (one-to-one only): for example a Signal device whose keys the server didn't hand out.
    int unreachable = 0;
    /// Tor went down during the send: the message is "queued" and goes with sendQueuedChat (nothing was lost or sent twice).
    bool queued = false;
};

/// What a receive pass left on the server, for the chat to explain (M5).
struct ReceiveStats {
    int held = 0;  // kept on the server for any reason
    int heldForKeys = 0;  // of those: the sender's keys changed (compare safety numbers)
};

/// Sends `text` in a one-to-one chat (9.4). Records it as sent (or failed, then rethrows), and pins the chat to v3.
SendOutcome sendChat(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                     const std::string& text, std::int64_t nowMs = 0);
/// Sends `text` in a group (9.5): lists, membership, room keys to devices that lack them, then one fan-out.
SendOutcome sendGroupChat(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& text,
                          std::int64_t nowMs = 0);
/// Sends a file already uploaded and released. An empty `contactUserId` means a group. Recorded as sent or failed, like text.
SendOutcome sendChatFile(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                         const model::FileDescriptor& file, std::int64_t nowMs = 0);
/// Sends a reaction, edit or delete. An empty `contactUserId` means a group. A failed one isn't kept: the person sees it didn't work.
SendOutcome sendChatOp(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                       const ChatOp& op, std::int64_t nowMs = 0);

// --- Waiting for Tor (docs/tor-by-default.md, rule 5) ---

/// Tor isn't connected: `text` waits as "queued" in the (encrypted) history; nothing is sent or encrypted for anyone yet.
/// An empty `contactUserId` means a group. sendQueuedChat sends it, encrypted once, in order. Pins the chat to v3.
ChatRecord queueChat(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                     const std::string& text, std::int64_t nowMs = 0);
/// The same for a file already uploaded (its key stays in the encrypted history only).
ChatRecord queueChatFile(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                         const model::FileDescriptor& file, std::int64_t nowMs = 0);
/// The same for a reaction, edit or delete (shown at once, like the others).
ChatRecord queueChatOp(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                       const ChatOp& op, std::int64_t nowMs = 0);

struct QueueDrain {
    int sent = 0;
    /// Refused for good by the server, or by the engine (shown as failed; a failed op is dropped).
    int failed = 0;
    /// Tor or the server isn't reachable yet: the rest waits, in order.
    bool waiting = false;
};
/// What waited for Tor, oldest first: the already-encrypted requests in `server`'s queue exactly as they were
/// (never encrypted again), then each "queued" record, encrypted now. Stops, in order, at the first one that can't leave.
QueueDrain sendQueuedChat(V3Device& device, ChatHistory& history, QueueingV3Server& server);
/// Whether anything waits: a queued request or a queued record.
bool hasQueuedChat(const ChatHistory& history, const QueueingV3Server& server);

/// Runs the receive loop (9.6) once; returns the records that are new, hidden ops included, oldest first.
std::vector<ChatRecord> receiveChat(V3Device& device, ChatHistory& history, std::int64_t nowMs = 0, ReceiveStats* stats = nullptr);

}  // namespace redvault::e2ev3
