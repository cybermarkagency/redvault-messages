#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/chat_models.hpp"
#include "redvault/data/message_keys.hpp"
#include "redvault/data/community_keys.hpp"
#include "redvault/model/rich_message.hpp"

namespace redvault::data {

struct MessagesResult {
    /// Oldest first.
    std::vector<Message> messages;
    int unread = 0;
    /// The server may hold older messages than these (the last page asked for came back full).
    bool hasOlder = false;
    /// At most kMaxPins, oldest first.
    std::vector<PinnedView> pinned;
};

/// What a reply points at: `ref` travels in the message; `fromMe` is for showing it right away.
struct ReplyTarget {
    model::ReplyRef ref;
    bool fromMe = false;
};

/** To someone who charges for messages (api/earnings_api.hpp): the paid message a send uses, or the free safety contact. */
struct PaidSend {
    std::optional<std::string> entitlementId;
    bool safetyContact = false;
};

/**
 * Port of android/.../data/ConversationRepository.kt (the DM and group half
 * of ui/src/state/actions.ts): unseal conversation keys, fetch and decrypt
 * messages, compute read state from receipts, send, and the stage-3
 * commitment check. Blocking; call from a worker thread.
 */
class ConversationRepository {
public:
    /// "Delete for me": ids this device hides for an account. Optional.
    using HiddenIds = std::function<std::set<std::string>(const std::string& selfUserId)>;

    /**
     * `keys`: group key epochs and message signatures (docs/group-keys-and-signatures.md); optional for tests.
     * With `communityKeys`, communities are listed too and read with their epoch keys; without it (the CLI,
     * say) they're never asked for, like an app that predates them.
     */
    explicit ConversationRepository(std::shared_ptr<api::ApiClient> api, HiddenIds hiddenIds = {},
                                    std::shared_ptr<MessageKeys> keys = nullptr,
                                    std::shared_ptr<CommunityKeyStore> communityKeys = nullptr);

    /**
     * Opens every key the conversation carries for this session, on each load
     * (a group's key can change at any time). Someone left or was removed:
     * gives the group a new key first (any member may). Null without `keys`, and for a community
     * (whose epoch keys are the CommunityKeyStore's).
     */
    std::shared_ptr<const KeyRing> refreshKeys(const api::ConversationDto& dto);

    /// A chat this computer pinned isn't a community, whatever the server says now: communities skip those checks.
    bool isCommunity(const api::ConversationDto& dto) const;

    /// Why this chat's key was refused, when it was (see KeyTrust): nothing can be sent there, and the chat says so.
    std::optional<std::string> keyProblem(const std::string& conversationId) const;

    std::vector<api::ConversationDto> listConversations();
    /// Message requests: 1:1 chats a non-friend started with you, not accepted or ignored yet.
    std::vector<api::ConversationDto> listRequests();
    void respondToRequest(const std::string& conversationId, bool accept);

    /**
     * Unseals a chat's key and resolves who's in it. Empty if it isn't a
     * well-formed 1:1 (e.g. the other participant already left).
     *
     * A community: its epoch keys are loaded (or reused while the epoch
     * hasn't moved), its name is the community's public name, and its
     * members are only you until addCommunitySenders() adds who wrote what's
     * loaded. Throws ApiError when its keys can't be fetched (removed, say).
     */
    std::optional<ConversationMeta> hydrateMeta(const api::ConversationDto& dto, const SelfIdentity& identity);

    /// A community: adds whoever wrote these messages to its members (profiles looked up once per session).
    void addCommunitySenders(ConversationMeta& meta, const std::vector<Message>& messages);

    /// The epoch keys, when this repository reads communities.
    std::shared_ptr<CommunityKeyStore> communityKeys() const { return communityKeys_; }

    /// How many of a chat's latest messages are fetched at first, and how many more each "load older" adds.
    static constexpr int kPageSize = 50;
    /// The most the server returns in one request.
    static constexpr int kMaxPage = 200;

    /**
     * Fetches and decrypts a chat's latest `window` messages (paging back as
     * needed). `unread` counts incoming messages this user has no receipt
     * for yet; own messages are marked seen/delivered once every other
     * participant has read/received them. Reactions are folded into the
     * message they react to.
     */
    MessagesResult fetchMessages(const ConversationMeta& meta, const std::string& selfUserId, int window = kPageSize);

    /// "read" is visible to the sender; "delivered" only keeps this user's own unread count right.
    void markRead(const std::string& conversationId, bool readReceipts);

    /**
     * A plain message, or a reply carrying its quote inside the encrypted body.
     * Stage 1: encrypt locally (the plaintext never reaches the network).
     * Stage 2: relay the ciphertext. Stage 3: recompute the commitment and
     * compare it with the server's. Throws std::invalid_argument if empty.
     */
    Message send(const ConversationMeta& meta, const std::string& text,
                 const std::optional<ReplyTarget>& replyTo = std::nullopt,
                 const std::optional<std::string>& clientMessageId = std::nullopt, const PaidSend& paid = {});

    /**
     * A new version of one of your own messages, sent as its own encrypted
     * message that readers fold into the original (older apps show it as a
     * new message). Throws std::invalid_argument if empty.
     */
    void sendEdit(const ConversationMeta& meta, const std::string& messageId, const std::string& text);

    /// Text forwarded from another chat, marked as forwarded. Throws std::invalid_argument if empty.
    Message sendForwarded(const ConversationMeta& meta, const std::string& text);

    /// A channel post forwarded into a chat, end-to-end encrypted like any message; it keeps which post it was. Throws std::invalid_argument if empty.
    Message sendChannelForward(const ConversationMeta& meta, const std::string& text, const model::ChannelPostRef& ref);
    /// A reply (or quick reaction) to someone's story, sent to them marked with which story. Throws std::invalid_argument if empty.
    Message sendStoryReply(const ConversationMeta& meta, const std::string& text, const model::StoryReplyRef& ref);

    /// Pinning keeps the newest kMaxPins; pinning another drops the oldest.
    static constexpr std::size_t kMaxPins = 3;
    void sendPin(const ConversationMeta& meta, const std::string& messageId, const std::string& preview);
    void sendUnpin(const ConversationMeta& meta, const std::string& messageId);
    /// Throws std::invalid_argument for a poll that isn't well-formed (see rich_message::serializePoll).
    void sendPoll(const ConversationMeta& meta, const std::string& question, const std::vector<std::string>& options,
                  bool multiple);
    /// Your vote replaces any earlier one; empty `options` takes it back.
    void sendVote(const ConversationMeta& meta, const Message& poll, const std::vector<int>& options);
    void sendEndPoll(const ConversationMeta& meta, const std::string& pollId);

    /// Reacts to a message by sending a small encrypted reaction message.
    void sendReaction(const ConversationMeta& meta, const std::string& messageId, const std::string& emoji);

    /**
     * Sends a file's descriptor as an ordinary E2EE message listing its
     * object as the attachment. Safe to retry: re-sending the same
     * attachment returns the original message instead of a duplicate.
     */
    /// `clientMessageId`: the outbox's id, so a retry never posts the file twice.
    Message sendFile(const ConversationMeta& meta, const model::FileDescriptor& descriptor,
                     const std::optional<std::string>& clientMessageId = std::nullopt);

    /// Re-verifies an own message loaded from a poll: fetches the server's record fresh and recomputes locally.
    MessageSecurity verify(const Message& message);

    /// Deletes your own message for real: the server clears its ciphertext.
    void deleteMessage(const std::string& messageId);

    void setDisappearing(const std::string& conversationId, const Disappearing& duration);

    static inline const std::string kDefaultGroupName = "Group";

private:
    /// The latest `window` raw messages (newest first), deduplicated across pages.
    std::vector<api::MessageDto> fetchLatest(const std::string& conversationId, int window, bool& hasOlder);

    Message sendEnvelope(const ConversationMeta& meta, const std::string& plaintext,
                         const std::optional<std::vector<std::string>>& attachments,
                         const std::optional<std::string>& clientMessageId = std::nullopt, const PaidSend& paid = {});

    /**
     * Communities: always the group key of the current epoch. If someone left
     * since the keys were loaded, the server answers rekey_required: this app
     * rotates the key (sealing a new one only to who remains) and sends under
     * it; stale_key_epoch means another app already did, so it reloads the
     * keys and tries again (three attempts in all).
     */
    Message sendToCommunity(const ConversationMeta& meta, const std::string& plaintext,
                            const std::optional<std::vector<std::string>>& attachments,
                            const std::optional<std::string>& clientMessageId = std::nullopt);

    std::optional<ConversationMeta> hydrateCommunity(const api::ConversationDto& dto, const SelfIdentity& identity);

    std::shared_ptr<api::ApiClient> api_;
    HiddenIds hiddenIds_;
    std::shared_ptr<MessageKeys> keys_;
    std::shared_ptr<CommunityKeyStore> communityKeys_;
    // Profiles of people seen in community messages, looked up once (a community can have a thousand members).
    std::mutex profilesMutex_;
    std::map<std::string, std::optional<api::UserResult>> profiles_;
};

/// Stage-3 check: the local commitment over the envelope vs. the server's record.
MessageSecurity buildSecurity(const std::string& messageId, crypto::ByteView ciphertext, crypto::ByteView nonce,
                              const std::optional<api::VerificationDto>& record);

}  // namespace redvault::data
