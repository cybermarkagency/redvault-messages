// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "redvault/e2ev3/device.hpp"
#include "redvault/e2ev3/send_queue.hpp"
#include "redvault/model/file_message.hpp"
#include "redvault/model/rich_message.hpp"

// The protocol-3 chat path on the desktop: docs/multi-device-threat-model.md
// section 9 (reference: ui/src/lib/e2eV3/chat.ts). Text, files, reactions, edits
// and deletes as envelopes over the engine's Olm (one-to-one, with copies for the
// sender's other devices) and Megolm (groups), the receive loop with
// ack-after-persist, and a local history that lives only on this computer,
// encrypted under a key derived from the device's storage key. Behind
// E2E_V3_ENABLED; the app uses it for a chat whose members all have device keys,
// or that is pinned to them (docs/signal-integration/07-step1-default-path.md).

namespace redvault::e2ev3 {

class StoryStore;

/// What a reaction, edit, delete, vote, poll end, pin or receipt does (docs/signal-integration/08-part-a-chat-features.md).
struct ChatOp {
    std::string t;  // "reaction" | "edit" | "delete" | "vote" | "end" | "pin" | "receipt"
    std::string target;  // empty on a receipt (it has `targets`)
    std::string body;  // an edit's new text
    std::string emoji;  // a reaction; empty removes the person's reaction
    std::vector<int> choices;  // a vote: option indexes; empty takes the vote back
    bool pinned = false;  // a pin: true pins, false unpins
    std::vector<std::string> targets;  // a receipt: 1 to 100 message ids
    std::string status;  // a receipt: "delivered" | "read"
};

struct ChatReaction {
    std::string userId;
    std::string emoji;
};

/// A poll message (its question is the record's `text`). `votes`, `ended` and `ballots` are worked out by foldChat.
struct ChatPoll {
    std::vector<std::string> options;
    bool multi = false;
    std::vector<int> votes;  // per option
    bool ended = false;
    /// Each voter's counted choices, by user id (a vote taken back isn't listed).
    std::vector<std::pair<std::string, std::vector<int>>> ballots;
};

/// One person's receipt for a message: "read" beats "delivered".
struct ChatReceipt {
    std::string userId;
    std::string status;
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
    /// A receipt always names the author it goes to, in a group too.
    std::string to;
    // Part A: what a text, file or poll says beyond its body (stored).
    std::string replyTo;  // the message this answers ("" for none)
    std::string replyPreview;  // what it answers, for a device that never had it
    bool forwarded = false;
    int exp = 0;  // disappearing time in seconds, 0 for none
    std::optional<ChatPoll> poll;  // a poll: `text` is its question
    // Worked out by foldChat; never stored.
    bool pinned = false;
    std::vector<ChatReceipt> receipts;  // by user id, never the author
    std::optional<std::int64_t> expiresAt;  // ts + exp * 1000
    /// Part D: a text that replies or reacts to a story (stored). Never carries the story's key.
    std::optional<model::StoryReplyRef> storyReply;
};

/// When a record disappears (ts + exp * 1000), or nothing when it doesn't.
std::optional<std::int64_t> expiryOf(const ChatRecord& record);

/**
 * Turns stored records into what a chat shows, in any arrival order: only the
 * person who sent a message can edit or delete it, the latest edit and each
 * person's latest reaction count, a delete beats both, and an op whose message
 * hasn't arrived waits. Part A: replies, forwards, polls (each voter's latest
 * valid vote, up to the author's first end), pins (the latest op), receipts
 * (one per person other than the author, read beating delivered) and expiry:
 * with `nowMs`, a message whose expiresAt <= nowMs is left out. Oldest first by
 * ts, then arrival.
 */
std::vector<ChatRecord> foldChat(const std::vector<ChatRecord>& records, std::optional<std::int64_t> nowMs = std::nullopt);

/// This device's decrypted chat history and per-conversation v3 pins (9.7). One encrypted file, rewritten on each change.
class ChatHistory {
public:
    ChatHistory(std::filesystem::path file, crypto::Bytes key);
    /// The clock that decides expiry (ms; 0 or less: nothing expires). The wall clock unless a test sets another.
    void setClock(std::function<std::int64_t()> clock);
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
    /**
     * Erases every message whose disappearing time has come, in every conversation, as a delete
     * does: its text, file key, poll and reply preview, and the ops that refer to it (an emptied
     * record stays, so a late copy or op is never stored again). True when anything changed.
     */
    bool eraseExpired();
    /// The next time a stored message disappears (ms), if any does.
    std::optional<std::int64_t> nextExpiry() const;
    /// Whether this account already sent a receipt for `messageId` with `status` ("read" also covers "delivered").
    bool receiptSent(const std::string& conversationId, const std::string& messageId, const std::string& selfUserId,
                     const std::string& status) const;
    /// A chat that has used v3 stays on v3 until the person says otherwise (9.1 item 5).
    bool pinned(const std::string& conversationId) const;
    void pin(const std::string& conversationId);
    void unpin(const std::string& conversationId);
    /// Part C2: when this device first opened a community chat (kept, the first time wins). Nothing older ever arrives.
    void markJoined(const std::string& conversationId, std::int64_t atMs);
    std::optional<std::int64_t> joinedAt(const std::string& conversationId) const;

private:
    void load();
    void save() const;
    /// Erases deleted and expired messages (9.3, part A). True when anything changed.
    bool eraseGone(const std::string& conversationId);
    std::int64_t now() const;
    std::filesystem::path file_;
    std::function<std::int64_t()> clock_;
    crypto::Bytes key_;
    mutable std::mutex mutex_;
    std::vector<ChatRecord> records_;
    std::vector<std::string> pins_;
    std::map<std::string, std::int64_t> joined_;
};

/// What a text, file or poll envelope carries beyond its body (part A).
struct MessageExtras {
    std::string replyTo;  // a lowercase uuid, or "" for none
    std::string replyPreview;  // at most 200 characters
    bool forwarded = false;
    int exp = 0;  // 1 to 2,419,200 seconds, or 0 for none
    /// Part D (a text only): {authorId, storyId, kind, preview (at most 80 characters), reaction}.
    std::optional<model::StoryReplyRef> storyReply;
};

/// A text envelope (9.3).
struct Envelope {
    int v = 1;
    std::string t = "text";
    std::string id;
    std::string c;
    std::int64_t ts = 0;
    std::string body;
    MessageExtras extras;
};
std::string encodeEnvelope(const Envelope& envelope);
/// The envelope for a reaction, edit, delete, vote, end, pin or receipt. Refuses one that wouldn't decode.
std::string encodeOp(const std::string& conversationId, const ChatOp& op, const std::string& id, std::int64_t ts);
/// The envelope for a file. This is the only place the file's key travels.
std::string encodeFile(const std::string& conversationId, const model::FileDescriptor& file, const std::string& id, std::int64_t ts,
                       const MessageExtras& extras = {});
/// The envelope for a poll (its id is the poll's). Refuses one that wouldn't decode.
std::string encodePoll(const std::string& conversationId, const std::string& question, const std::vector<std::string>& options, bool multi,
                       const std::string& id, std::int64_t ts, const MessageExtras& extras = {});
/// What every client counts as a string's length: UTF-16 code units, like JavaScript and Kotlin.
std::size_t textLength(const std::string& utf8);
/// The most a reply preview may hold (characters); longer text is cut to it.
std::string previewText(const std::string& text);

/// Part B (docs/signal-integration/09-part-b-calls.md): a one-to-one call's offer, answer or restart (t "call").
/// Ephemeral: acknowledged and used, never stored in the history or folded, never queued for Tor.
struct CallSignal {
    std::string callId;
    std::string step;  // "offer" | "answer" | "restart-offer" | "restart-answer"
    std::string kind;  // "voice" | "video"
    std::string sdp;  // 1 to 49,152 UTF-16 units
    std::string to;  // the device it's meant for, or "*" for every device of the recipient
};

/// Part B: a group call's room key for one epoch (t "callkey"), sent pairwise. Ephemeral too.
struct CallKey {
    std::string callId;
    std::int64_t epoch = 0;  // 1 or more
    std::shared_ptr<const crypto::SecretBytes> key;  // exactly 32 bytes
};

/// Part D: a story's key K (t "storykey"), pairwise from a device of the story's author. Kept in the StoryStore only.
struct StoryKeyEnvelope {
    std::string storyId;
    std::shared_ptr<const crypto::SecretBytes> key;  // exactly 32 bytes
    std::int64_t expiresAt = 0;  // ms
};

/// Part D: a named answer to a story's sticker (t "storyanswer"), pairwise to the author's devices and the sender's own.
struct StoryAnswerEnvelope {
    std::string storyId;
    std::string stickerId;
    std::int64_t rev = 0;  // 1 or more
    bool final = false;
    Json payload;  // the answer JSON object
};

/// A call envelope older than this is refused (ms).
inline constexpr std::int64_t kCallMaxAgeMs = 120 * 1000;
/// The longest SDP a call envelope carries (UTF-16 units).
inline constexpr std::size_t kCallMaxSdp = 49152;

struct DecodedEnvelope {
    enum class Kind {
        Message,  // text, file or poll
        Op,  // reaction, edit, delete, vote, end, pin or receipt
        Call,  // part B: a call's offer, answer or restart (ephemeral)
        CallKey,  // part B: a group call's room key (ephemeral)
        StoryKey,  // part D: a story's key (kept in the story-key store, never in a chat)
        StoryAnswer,  // part D: a named sticker answer
        Newer,  // a newer envelope version: keep the item and ask for an update
        Ignore,  // an unknown type, or bound to another conversation: nothing to show, the item is acknowledged
    };
    Kind kind = Kind::Ignore;
    std::string id;
    std::int64_t ts = 0;
    std::string text;
    std::optional<model::FileDescriptor> file;
    std::optional<ChatOp> op;
    MessageExtras extras;
    std::optional<ChatPoll> poll;  // a poll: `text` is its question
    std::optional<CallSignal> call;  // Kind::Call
    std::optional<CallKey> callKey;  // Kind::CallKey
    std::optional<StoryKeyEnvelope> storyKey;  // Kind::StoryKey
    std::optional<StoryAnswerEnvelope> storyAnswer;  // Kind::StoryAnswer
    std::string reason;  // why it was ignored
};
/// Validates an envelope against the mailbox item that carried it (9.3).
DecodedEnvelope decodeEnvelope(const std::string& plaintext, const std::string& conversationId, std::int64_t nowMs = 0);
/// The envelope for a call's offer, answer or restart. Refuses one that wouldn't decode.
std::string encodeCall(const std::string& conversationId, const CallSignal& signal, const std::string& id, std::int64_t ts);
/// The envelope for a group call's room key. Refuses one that wouldn't decode.
std::string encodeCallKey(const std::string& conversationId, const CallKey& key, const std::string& id, std::int64_t ts);

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
/// A new message: text, a file already uploaded and released, or a poll (`text` is its question), with its extras.
struct OutgoingMessage {
    std::string text;
    std::optional<model::FileDescriptor> file;
    std::optional<ChatPoll> poll;  // options and multi
    MessageExtras extras;
};
/// Sends any new message (a reply, a forward, a poll, one that disappears). An empty `contactUserId` means a group.
SendOutcome sendChatMessage(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                            const OutgoingMessage& message, std::int64_t nowMs = 0);
/**
 * Forwards a file into this chat: `reupload` encrypts and uploads the plaintext again for this chat and
 * returns the new descriptor, which must not share the original's object or key (a descriptor is never
 * reused in another chat). Sent with fwd. `queue`: Tor is down or earlier sends wait, so it's queued.
 */
SendOutcome forwardChatFile(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                            const model::FileDescriptor& original,
                            const std::function<model::FileDescriptor(const model::FileDescriptor&)>& reupload, int exp = 0,
                            bool queue = false, std::int64_t nowMs = 0);
/**
 * A receipt (part A) for messages of one author: sent to that author's devices and this account's other
 * devices only (pairwise, in a group too), never to anyone else. Not kept when it fails.
 */
SendOutcome sendReceipt(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& authorUserId,
                        const std::vector<std::string>& messageIds, const std::string& status, std::int64_t nowMs = 0);
/// The same, waiting for Tor (docs/tor-by-default.md).
ChatRecord queueReceipt(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& authorUserId,
                        const std::vector<std::string>& messageIds, const std::string& status, std::int64_t nowMs = 0);
/// Sends a reaction, edit, delete, vote, end or pin. An empty `contactUserId` means a group. A failed one isn't kept: the person sees it didn't work.
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
/// The same for any new message (OutgoingMessage).
ChatRecord queueChatMessage(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                            const OutgoingMessage& message, std::int64_t nowMs = 0);
/// The same for a reaction, edit, delete, vote, end or pin (shown at once, like the others).
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

// --- Receipts (part A): at most 100 ids per envelope, at most once every 2 s per chat ---

/// Collects the receipts this device owes and hands them out in batches. Not thread-safe: one owner.
class ReceiptBatcher {
public:
    static constexpr std::int64_t kIntervalMs = 2000;
    static constexpr std::size_t kMaxIds = 100;
    /// A message of `authorUserId` was stored ("delivered") or shown ("read").
    void add(const std::string& conversationId, const std::string& authorUserId, const std::string& messageId, const std::string& status);
    struct Batch {
        std::string conversationId;
        std::string authorUserId;
        std::string status;
        std::vector<std::string> ids;
    };
    /// What's due at `nowMs`: for each chat whose last batch went at least 2 s ago, one envelope per author and
    /// status ("read" first), each at most 100 ids; what's left over waits for the next turn.
    std::vector<Batch> due(std::int64_t nowMs);
    /// When the next batch is due, if anything waits.
    std::optional<std::int64_t> nextDue() const;
    bool empty() const { return pending_.empty(); }

private:
    struct Pending {
        std::string messageId;
        std::string authorUserId;
        std::string status;
    };
    // conversation -> what's owed, in the order it came ("read" replaces "delivered" for the same message)
    std::map<std::string, std::vector<Pending>> pending_;
    std::map<std::string, std::int64_t> lastSent_;
};

/// A call envelope that arrived (part B): from a device in the sender's verified list, pairwise, meant for this device.
struct CallMessage {
    std::string conversationId;
    std::string fromUserId;
    std::string fromDeviceId;
    std::int64_t ts = 0;  // the sender's clock, ms
    std::optional<CallSignal> signal;
    std::optional<CallKey> key;
};

/**
 * Runs the receive loop (9.6) once; returns the records that are new, hidden ops included, oldest first.
 * Call envelopes (part B) are acknowledged and never stored: they go to `calls` when it's given (only those
 * sent pairwise by another account, meant for this device or "*"), and are dropped otherwise. Story keys and named
 * answers (part D) go into `stories` before they're acknowledged (pairwise only; dropped without a store).
 */
std::vector<ChatRecord> receiveChat(V3Device& device, ChatHistory& history, std::int64_t nowMs = 0, ReceiveStats* stats = nullptr,
                                    std::vector<CallMessage>* calls = nullptr, StoryStore* stories = nullptr);

// --- Calls over device keys (part B, docs/signal-integration/09-part-b-calls.md) ---

/**
 * Sends a call's offer, answer or restart to the contact's devices: every one when `signal.to` is "*", else only
 * that device. Never to this account's other devices, never queued for Tor: Tor down or the server out of reach
 * throws (the call fails). Throws when no device of theirs got it. Returns how many did.
 */
int sendCallSignal(V3Device& device, const std::string& conversationId, const std::string& contactUserId, const CallSignal& signal,
                   std::int64_t nowMs = 0);
/// Sends a group call's room key pairwise to every device of each of `userIds` (never this account's), never queued. Returns how many devices got it.
int sendCallKey(V3Device& device, const std::string& conversationId, const CallKey& key, const std::vector<std::string>& userIds,
                std::int64_t nowMs = 0);

/**
 * The checks that need to know the calls going on (part B): a call envelope is used only for a call the live
 * events told this app about, from that call's other party (one-to-one) or a member of the group (room keys), in
 * the right conversation, and a room key only for an epoch at least as new as the newest one taken. The first
 * offer or answer binds the call to the device that sent it. An envelope for a call not known yet (it can beat
 * the live event) is held in memory for up to 20 s, never used before the call is known. Not thread-safe: one owner.
 */
class CallGate {
public:
    static constexpr std::int64_t kHoldMs = 20 * 1000;
    static constexpr std::size_t kMaxHeld = 32;
    enum class Role { Caller, Callee };

    /// A one-to-one call the live events named: `peerUserId` is the other party.
    void expectCall(const std::string& callId, const std::string& conversationId, const std::string& peerUserId, Role role);
    /// A group call this app is in, with the group's members (who may send its room keys).
    void expectGroupCall(const std::string& callId, const std::string& conversationId, std::vector<std::string> members);
    void setMembers(const std::string& callId, std::vector<std::string> members);
    void forget(const std::string& callId);
    bool knows(const std::string& callId) const { return calls_.count(callId) > 0; }
    /// The other party's device the call is bound to (the one whose offer or answer was taken), or "".
    std::string peerDevice(const std::string& callId) const;
    /// The newest room-key epoch taken for a group call (0: none).
    std::int64_t newestEpoch(const std::string& callId) const;

    /// Checks what arrived (and what was held, now that more calls may be known). Returns what may be used, in order.
    /// `refused` (optional) gets one reason per message turned down, for tests and diagnostics (never the content).
    std::vector<CallMessage> admit(std::vector<CallMessage> messages, std::int64_t nowMs, std::vector<std::string>* refused = nullptr);

private:
    struct Entry {
        std::string conversationId;
        bool group = false;
        std::string peerUserId;
        Role role = Role::Callee;
        std::string peerDeviceId;
        std::vector<std::string> members;
        std::int64_t newestEpoch = 0;
    };
    /// "" when it may be used; otherwise why not.
    std::string check(const CallMessage& message, std::int64_t nowMs);
    std::map<std::string, Entry> calls_;
    std::vector<std::pair<std::int64_t, CallMessage>> held_;  // (when it arrived, the message)
};

}  // namespace redvault::e2ev3
