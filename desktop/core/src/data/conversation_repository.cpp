#include "redvault/data/conversation_repository.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/commitment.hpp"
#include "redvault/crypto/community_crypto.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/model/file_message.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

using crypto::base64Decode;
using crypto::base64Encode;
namespace rich_message = model::rich_message;
namespace file_message = model::file_message;

namespace {

std::int64_t parseMs(const std::string& iso) {
    return util::parseIsoMillis(iso).value_or(0);
}

/// One spelling per value: base64 can hide bits in its last character, and they decode to nothing.
std::string canonicalBase64(const std::string& text) {
    static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out = text.substr(0, text.find_last_not_of('=') + 1);
    if (out.empty()) return out;
    const auto last = kAlphabet.find(out.back());
    if (last == std::string_view::npos) return out;
    const std::size_t spare = out.size() % 4 == 2 ? 4 : out.size() % 4 == 3 ? 2 : 0;
    out.back() = kAlphabet[(last >> spare) << spare];
    return out;
}

/**
 * Every send gets a fresh nonce, so identical bytes under another id are a replay: they show once, the first time
 * they appear, oldest first, so a replay can't push the original out. Both the nonce and the ciphertext have to
 * match: two damaged messages that share a nonce are still two messages, and each shows its own error.
 *
 * The epoch counts too: each has its own key, so the same bytes under another epoch can't open (and a signature
 * covers the epoch). They can only show as an error, like any other junk, never as a readable second copy.
 */
std::vector<const api::MessageDto*> withoutReplays(std::vector<const api::MessageDto*> messages) {
    std::vector<const api::MessageDto*> oldestFirst = messages;
    std::stable_sort(oldestFirst.begin(), oldestFirst.end(), [](const api::MessageDto* a, const api::MessageDto* b) {
        return parseMs(a->createdAt) < parseMs(b->createdAt);
    });
    std::unordered_set<std::string> seen;
    std::unordered_set<const api::MessageDto*> repeats;
    for (const auto* message : oldestFirst) {
        const std::string send = canonicalBase64(*message->nonce) + ":" + canonicalBase64(*message->ciphertext) + ":" + std::to_string(message->keyEpoch);
        if (!seen.insert(send).second) repeats.insert(message);
    }
    std::erase_if(messages, [&](const api::MessageDto* message) { return repeats.contains(message); });
    return messages;
}

std::optional<std::int64_t> parseMs(const std::optional<std::string>& iso) {
    if (!iso) return std::nullopt;
    return util::parseIsoMillis(*iso);
}

/// Leading NULs are reserved for the structured-message markers; then the usual trim.
std::string cleanOutgoingText(const std::string& text) {
    std::string_view view = text;
    while (!view.empty() && view.front() == '\0') view.remove_prefix(1);
    return util::encodeUtf8(util::trim(util::decodeUtf8(view)));
}

struct DecodedReaction {
    std::string targetId;
    std::string emoji;
    std::int64_t createdAtMs = 0;
};

/// A later version of another message's text; applied only if it comes from that message's sender.
struct DecodedEdit {
    std::string id;
    std::string targetId;
    std::string senderId;
    std::string text;
    std::int64_t createdAtMs = 0;
};

/// A vote, or a poll's end, folded into the poll it's about.
struct DecodedPollEvent {
    std::string pollId;
    std::string senderId;
    std::int64_t createdAtMs = 0;
    bool end = false;
    std::vector<int> options;
};

/// A pin or unpin, kept as a notice in the timeline and folded into the pinned list.
struct PinEvent {
    std::string targetId;
    std::string preview;
    std::string senderId;
    std::int64_t createdAtMs = 0;
    bool pin = true;
};

/// An unverified message trying to act on another one (edit, react, vote, pin): dropped.
struct DecodedHidden {};

using Decoded = std::variant<Message, DecodedReaction, DecodedEdit, DecodedPollEvent, DecodedHidden>;

Decoded decode(const api::MessageDto& dto, const ConversationMeta& meta, const std::string& selfUserId,
               const std::unordered_set<std::string>& theirRead, const std::unordered_set<std::string>& theirDelivered,
               const KeyRing* ring, SenderCheck check, const crypto::community::KeyRing* communityRing) {
    const bool fromMe = dto.senderId == selfUserId;
    // The key of the epoch it was sent under, and the sender's signature.
    std::optional<std::string> plaintext;
    if (check != SenderCheck::Invalid) {
        try {
            if (meta.community) {
                // Communities: each message names the epoch whose key encrypted it.
                if (communityRing != nullptr) {
                    plaintext = crypto::community::decryptForCommunity(*communityRing, dto.keyEpoch, base64Decode(*dto.ciphertext),
                                                                       base64Decode(*dto.nonce));
                }
            } else {
                const crypto::SecretBytes* key = ring != nullptr ? ring->key(dto.keyEpoch) : &meta.conversationKey;
                if (key != nullptr) {
                    plaintext = crypto::decryptWithConversationKey(base64Decode(*dto.ciphertext), base64Decode(*dto.nonce), *key);
                }
            }
        } catch (const std::exception&) {
            // Shown as undecryptable below.
        }
    }
    const bool unverified = check == SenderCheck::Unverified;

    // A file only counts if the server also lists that exact object as this
    // message's attachment, so a descriptor can't point at some other object
    // and a pasted one doesn't render as a file.
    std::optional<model::FileDescriptor> descriptor;
    const api::AttachmentDto* attachment = nullptr;
    std::optional<model::ParsedBody> structured;
    if (plaintext) {
        descriptor = file_message::parse(*plaintext);
        if (descriptor) {
            for (const auto& candidate : dto.attachments) {
                if (util::toLowerAscii(candidate.objectId) == descriptor->objectId) {
                    attachment = &candidate;
                    break;
                }
            }
        }
        if (rich_message::isStructured(*plaintext)) structured = rich_message::parse(*plaintext);
    }
    if (structured && unverified) {
        const auto* acting = std::get_if<model::parsed::Text>(&*structured);
        // Edits, reactions, votes and pins only count when we know who sent them.
        if (std::holds_alternative<model::parsed::Reaction>(*structured) ||
            (acting != nullptr && (acting->edits || acting->vote || acting->endPoll || acting->pin || acting->unpin))) {
            return DecodedHidden{};
        }
    }
    if (structured) {
        if (const auto* reaction = std::get_if<model::parsed::Reaction>(&*structured)) {
            return DecodedReaction{reaction->messageId, reaction->emoji, parseMs(dto.createdAt)};
        }
        if (const auto* edit = std::get_if<model::parsed::Text>(&*structured); edit != nullptr && edit->edits) {
            return DecodedEdit{dto.id, *edit->edits, dto.senderId, edit->text, parseMs(dto.createdAt)};
        }
        if (const auto* event = std::get_if<model::parsed::Text>(&*structured); event != nullptr && (event->vote || event->endPoll)) {
            DecodedPollEvent out{event->vote ? event->vote->pollId : *event->endPoll, dto.senderId, parseMs(dto.createdAt),
                                 !event->vote, {}};
            if (event->vote) out.options = event->vote->options;
            return out;
        }
    }

    Message message;
    message.id = dto.id;
    message.fromMe = fromMe;
    message.senderId = dto.senderId;
    message.unverifiedSender = unverified;
    const auto* text = structured ? std::get_if<model::parsed::Text>(&*structured) : nullptr;
    if (check == SenderCheck::Invalid) {
        message.kind = MessageKind::Undecryptable;
        message.text = "Couldn't verify who sent this message.";
    } else if (!plaintext) {
        message.kind = MessageKind::Undecryptable;
        message.text = "Couldn't decrypt this message.";
    } else if (attachment != nullptr) {
        message.kind = MessageKind::File;
        message.text = descriptor->filename;
        message.file = descriptor;
        message.fileStatus = attachment->status;
    } else if (text != nullptr && text->poll) {
        message.kind = MessageKind::Poll;
        message.text = text->poll->question;
        message.poll = PollView{text->poll->question, text->poll->options, text->poll->multiple, false,
                                std::vector<int>(text->poll->options.size(), 0), {}, 0};
    } else if (text != nullptr && (text->pin || text->unpin)) {
        message.kind = MessageKind::Notice;
        message.notice = text->pin ? "pin" : "unpin";
        message.noticeTarget = text->pin ? text->pin->messageId : *text->unpin;
        message.text = text->pin ? text->pin->preview : std::string();
    } else if (text != nullptr) {
        message.text = text->text;
        message.forwarded = text->forwarded;
        message.channelPost = text->channelPost;
        message.storyReply = text->storyReply;
    } else if ((structured && std::holds_alternative<model::parsed::Invalid>(*structured)) ||
               file_message::isFileMessage(*plaintext)) {
        message.text = "Unreadable message.";
    } else {
        message.text = *plaintext;
    }
    if (text != nullptr && text->replyTo) {
        message.replyTo = ReplyView{text->replyTo->messageId, text->replyTo->preview,
                                    text->replyTo->senderId == util::toLowerAscii(selfUserId)};
    }
    message.createdAtMs = parseMs(dto.createdAt);
    message.deleteAtMs = parseMs(dto.deleteAt);
    message.seen = fromMe && theirRead.contains(dto.id);
    message.delivered = fromMe && (theirDelivered.contains(dto.id) || theirRead.contains(dto.id));
    if (fromMe) {
        message.ciphertextB64 = dto.ciphertext;
        message.nonceB64 = dto.nonce;
    }
    return message;
}

}  // namespace

MessageSecurity buildSecurity(const std::string& messageId, crypto::ByteView ciphertext, crypto::ByteView nonce,
                              const std::optional<api::VerificationDto>& record) {
    if (!record || record->protocolVersion != crypto::commitment::kProtocolVersion) {
        return MessageSecurity{};
    }
    const std::string local = crypto::commitment::computeHex(messageId, ciphertext, nonce);
    return MessageSecurity{
        .verification = crypto::commitment::matches(local, record->commitment) ? VerificationStatus::Verified
                                                                                : VerificationStatus::Failed,
        .publicTransactionId = record->publicTransactionId,
        .protocolVersion = record->protocolVersion,
        .localCommitment = local,
        .networkCommitment = record->commitment,
    };
}

ConversationRepository::ConversationRepository(std::shared_ptr<api::ApiClient> api, HiddenIds hiddenIds,
                                               std::shared_ptr<MessageKeys> keys, std::shared_ptr<CommunityKeyStore> communityKeys)
    : api_(std::move(api)),
      hiddenIds_(std::move(hiddenIds)),
      keys_(std::move(keys)),
      communityKeys_(std::move(communityKeys)) {}

std::shared_ptr<const KeyRing> ConversationRepository::refreshKeys(const api::ConversationDto& dto) {
    // A community's epoch keys are the CommunityKeyStore's, not this ring.
    if (!keys_ || isCommunity(dto)) return nullptr;
    if (dto.isGroup && dto.rekeyRequired) {
        try {
            keys_->rotate(dto.id);
        } catch (const std::exception&) {
            // Tried again at the next send.
        }
        if (auto ring = keys_->ringFor(dto.id); ring && !ring->rekeyRequired) return ring;
    }
    return keys_->ringFromDto(dto);
}

bool ConversationRepository::isCommunity(const api::ConversationDto& dto) const {
    return dto.community.has_value() && !(keys_ && keys_->isPinned(dto.id));
}

std::optional<std::string> ConversationRepository::keyProblem(const std::string& conversationId) const {
    return keys_ ? keys_->keyProblem(conversationId) : std::nullopt;
}

std::vector<api::ConversationDto> ConversationRepository::listConversations() {
    return api_->listConversations(communityKeys_ != nullptr);
}

std::vector<api::ConversationDto> ConversationRepository::listRequests() {
    return api_->listConversationRequests();
}

void ConversationRepository::respondToRequest(const std::string& conversationId, bool accept) {
    api_->respondToConversationRequest(conversationId, accept);
}

std::optional<ConversationMeta> ConversationRepository::hydrateCommunity(const api::ConversationDto& dto,
                                                                         const SelfIdentity& identity) {
    if (!communityKeys_) return std::nullopt;  // never listed without the keys; nothing to read it with
    const auto& info = *dto.community;
    const auto ring = communityKeys_->ensure(info.id, dto.id, info.keyEpoch, info.rekeyRequired);

    ConversationMeta meta;
    meta.id = dto.id;
    meta.fetchedAtMs = util::nowMillis();
    meta.isGroup = true;
    meta.handle = util::trim(util::decodeUtf8(info.name)).empty() ? std::string("Community") : info.name;
    meta.myRole = dto.role;
    meta.community = CommunityLink{info.id, info.name, info.kind, info.role, info.canPost, info.status, info.hasAvatar};
    // The current epoch's key (or the newest held) stands in as the chat's key, as on the web.
    const crypto::SecretBytes* key = ring->key(ring->currentEpoch);
    if (key == nullptr && !ring->keys.empty()) key = &ring->keys.rbegin()->second;
    meta.conversationKey = key != nullptr ? crypto::SecretBytes::copyOf(*key) : crypto::generateConversationKey();
    meta.members.push_back(GroupMemberView{identity.userId, identity.handle, identity.avatarUrl, dto.role,
                                           base64Encode(identity.publicKey), true});
    return meta;
}

void ConversationRepository::addCommunitySenders(ConversationMeta& meta, const std::vector<Message>& messages) {
    if (!meta.community) return;
    std::unordered_set<std::string> known;
    for (const auto& member : meta.members) known.insert(member.userId);
    for (const auto& message : messages) {
        if (message.senderId.empty() || !known.insert(message.senderId).second) continue;
        std::optional<api::UserResult> user;
        bool cached = false;
        {
            std::lock_guard lock(profilesMutex_);
            if (const auto it = profiles_.find(message.senderId); it != profiles_.end()) {
                user = it->second;
                cached = true;
            }
        }
        if (!cached) {
            try {
                user = api_->getUser(message.senderId);
            } catch (const api::ApiError& e) {
                if (e.status() == 401) throw;
                if (e.status() != 404) continue;  // offline, say: looked up again next time
            }
            std::lock_guard lock(profilesMutex_);
            profiles_[message.senderId] = user;
        }
        if (user) {
            meta.members.push_back(GroupMemberView{message.senderId, user->username, user->avatarUrl, "member", user->publicKey, false});
        }
    }
}

std::optional<ConversationMeta> ConversationRepository::hydrateMeta(const api::ConversationDto& dto,
                                                                    const SelfIdentity& identity) {
    if (isCommunity(dto)) return hydrateCommunity(dto, identity);
    const auto ring = refreshKeys(dto);
    // With a ring, what it holds is all there is: a refused key is never opened again here. An empty key opens and seals nothing.
    crypto::SecretBytes conversationKey;
    if (!ring) {
        conversationKey = crypto::unsealConversationKey(base64Decode(dto.wrappedKey), identity.publicKey, identity.privateKey);
    } else if (const auto* key = ring->currentKey()) {
        conversationKey = crypto::SecretBytes::copyOf(*key);
    }
    const auto participants = api_->listParticipants(dto.id);

    ConversationMeta meta;
    meta.id = dto.id;
    meta.fetchedAtMs = util::nowMillis();

    if (dto.isGroup) {
        // A group from before key notices gets its first one here. Best effort: it never stands in the way of
        // opening the group, nothing was pinned if it failed, and the next load tries again. Only at epoch 0: a later
        // key with no notice is something this computer can't vouch for, and members with pins would believe it.
        // The next rekey gives it a notice.
        if (ring && !dto.keyNotice && !ring->keyProblem && !conversationKey.empty() && ring->currentEpoch == 0) {
            std::vector<std::string> ids;
            for (const auto& participant : participants) ids.push_back(participant.userId);
            try {
                keys_->publishNotice(dto.id, ring->currentEpoch, conversationKey, ids);
            } catch (const api::ApiError& e) {
                if (e.status() == 401) throw;
            } catch (const std::exception&) {
            }
        }
        std::optional<std::string> name;
        if (dto.nameCiphertext && dto.nameNonce) {
            try {
                name = crypto::decryptWithConversationKey(base64Decode(*dto.nameCiphertext),
                                                          base64Decode(*dto.nameNonce), conversationKey);
            } catch (const crypto::CryptoError&) {
                // Falls back to the default name.
            }
        }
        for (const auto& participant : participants) {
            try {
                const auto user = api_->getUser(participant.userId);
                meta.members.push_back(GroupMemberView{participant.userId, user.username, user.avatarUrl,
                                                       participant.role, user.publicKey,
                                                       participant.userId == identity.userId});
            } catch (const api::ApiError&) {
                // A member that can't be resolved any more is left out, as on Android.
            }
        }
        const bool blank = !name || util::trim(util::decodeUtf8(*name)).empty();
        meta.handle = blank ? kDefaultGroupName : *name;
        meta.isGroup = true;
        meta.myRole = dto.role;
        meta.conversationKey = std::move(conversationKey);
        return meta;
    }

    const auto other = std::find_if(participants.begin(), participants.end(),
                                    [&](const api::ParticipantDto& p) { return p.userId != identity.userId; });
    if (other == participants.end()) return std::nullopt;
    const auto user = api_->getUser(other->userId);
    meta.otherUserId = other->userId;
    meta.handle = user.username;
    meta.avatarUrl = user.avatarUrl;
    meta.otherPublicKey = base64Decode(user.publicKey);
    meta.otherPublicKeyB64 = user.publicKey;
    meta.safetyNumber = crypto::safetyNumberDigits(identity.publicKey, meta.otherPublicKey);
    meta.conversationKey = std::move(conversationKey);
    return meta;
}

std::vector<api::MessageDto> ConversationRepository::fetchLatest(const std::string& conversationId, int window,
                                                                 bool& hasOlder) {
    window = std::max(window, 1);
    std::vector<api::MessageDto> out;
    std::unordered_set<std::string> seen;
    std::optional<std::string> before;
    // Messages already held from the oldest millisecond fetched so far: the next page repeats them.
    int overlap = 0;
    hasOlder = false;
    while (static_cast<int>(out.size()) < window) {
        const int limit = std::min(kMaxPage, window - static_cast<int>(out.size()) + overlap);
        auto page = api_->listMessages(conversationId, limit, before);
        const bool full = static_cast<int>(page.size()) >= limit;
        std::size_t fresh = 0;
        for (auto& dto : page) {
            if (seen.insert(dto.id).second) {
                out.push_back(std::move(dto));
                ++fresh;
            }
        }
        hasOlder = full;
        // A short page is the start of the chat; a page with nothing new would only loop.
        if (!full || fresh == 0) break;
        // One millisecond later than the oldest: the server compares finer than
        // milliseconds, so this re-asks for that oldest moment (dropped above as
        // duplicates) rather than skipping a message created in the same millisecond.
        std::int64_t oldest = std::numeric_limits<std::int64_t>::max();
        for (const auto& dto : out) oldest = std::min(oldest, parseMs(dto.createdAt));
        overlap = static_cast<int>(
            std::count_if(out.begin(), out.end(), [&](const api::MessageDto& dto) { return parseMs(dto.createdAt) == oldest; }));
        before = util::formatIsoMillis(oldest + 1);
    }
    // The overlap can bring in a few more than asked for; keep the newest `window`.
    std::stable_sort(out.begin(), out.end(), [](const api::MessageDto& a, const api::MessageDto& b) {
        return parseMs(a.createdAt) > parseMs(b.createdAt);
    });
    if (static_cast<int>(out.size()) > window) out.resize(static_cast<std::size_t>(window));
    return out;
}

MessagesResult ConversationRepository::fetchMessages(const ConversationMeta& meta, const std::string& selfUserId,
                                                     int window) {
    bool hasOlder = false;
    const auto dtos = fetchLatest(meta.id, window, hasOlder);
    const auto receipts = api_->listReceipts(meta.id);
    std::shared_ptr<const KeyRing> ring;
    if (keys_) {
        std::vector<std::string> senders;
        for (const auto& dto : dtos) {
            if (!dto.deletedAt) senders.push_back(dto.senderId);
        }
        std::vector<std::string> signedSenders;
        for (const auto& dto : dtos) {
            if (!dto.deletedAt && dto.signature) signedSenders.push_back(dto.senderId);
        }
        keys_->noteSigned(signedSenders);
        keys_->resolveSenderKeys(selfUserId, senders);
        ring = keys_->ringFor(meta.id);
        // A message under a key epoch this session hasn't opened yet: someone just rotated.
        const bool missing = ring && !ring->keyProblem && std::any_of(dtos.begin(), dtos.end(), [&](const api::MessageDto& dto) {
                                 return !dto.deletedAt && ring->key(dto.keyEpoch) == nullptr;
                             });
        if (missing) {
            try {
                if (auto fresh = keys_->refresh(meta.id)) ring = fresh;
            } catch (const std::exception&) {
            }
        }
    }

    std::vector<const api::MessageDto*> live;
    // Deleted for everyone: a pin of one of these drops out of the pinned list.
    std::unordered_set<std::string> deletedIds;
    for (const auto& dto : dtos) {
        if (!dto.deletedAt && dto.ciphertext && dto.nonce) live.push_back(&dto);
        if (dto.deletedAt) deletedIds.insert(util::toLowerAscii(dto.id));
    }
    live = withoutReplays(std::move(live));

    std::unordered_set<std::string> mine;
    for (const auto& receipt : receipts) {
        if (receipt.userId == selfUserId) mine.insert(receipt.messageId);
    }

    // Read/delivered once EVERY other participant has read/received it (for
    // a DM, just the one other person; "read" implies delivered).
    const auto others = meta.otherUserIds(selfUserId);
    const std::unordered_set<std::string> otherSet(others.begin(), others.end());
    std::unordered_map<std::string, std::unordered_set<std::string>> readBy;
    std::unordered_map<std::string, std::unordered_set<std::string>> deliveredBy;
    for (const auto& receipt : receipts) {
        if (!otherSet.contains(receipt.userId)) continue;
        deliveredBy[receipt.messageId].insert(receipt.userId);
        if (receipt.status == "read") readBy[receipt.messageId].insert(receipt.userId);
    }
    auto everyone = [&](const std::unordered_map<std::string, std::unordered_set<std::string>>& by) {
        std::unordered_set<std::string> out;
        if (otherSet.empty()) return out;
        for (const auto& [messageId, users] : by) {
            if (users.size() >= otherSet.size()) out.insert(messageId);
        }
        return out;
    };
    const auto theirRead = everyone(readBy);
    const auto theirDelivered = everyone(deliveredBy);

    const auto hidden = hiddenIds_ ? hiddenIds_(selfUserId) : std::set<std::string>{};

    // Reactions arrive as their own messages; fold them into the message
    // they react to and never show them on their own.
    struct EmojiTally {
        std::unordered_set<std::string> senders;
        std::optional<std::string> myId;
        std::int64_t firstAtMs = 0;
    };
    std::unordered_set<std::string> reactionIds;
    std::unordered_map<std::string, std::map<std::string, EmojiTally>> reactions;
    // Edits are folded into the message they edit, in the order they were sent.
    std::vector<DecodedEdit> edits;
    std::vector<DecodedPollEvent> pollEvents;
    std::vector<PinEvent> pinEvents;

    // Communities: the epoch keys held. A message under an epoch newer than
    // the ring knows means the key just rotated: fetch the new one now rather
    // than show it as unreadable until the next refresh.
    std::shared_ptr<const crypto::community::KeyRing> communityRing;
    if (meta.community && communityKeys_) {
        communityRing = communityKeys_->ring(meta.id);
        std::int64_t newest = -1;
        for (const auto* dto : live) {
            newest = std::max<std::int64_t>(newest, dto->keyEpoch);
        }
        if (!communityRing || (newest > communityRing->currentEpoch && communityRing->key(newest) == nullptr)) {
            try {
                communityRing = communityKeys_->load(meta.community->id, meta.id);
            } catch (const api::ApiError& e) {
                if (e.status() == 401) throw;  // otherwise read with what's held
            }
        }
    }

    MessagesResult result;
    result.hasOlder = hasOlder;
    for (const auto* dto : live) {
        // A community's messages aren't signed; every other chat is checked against the sender's pinned key.
        const SenderCheck check = keys_ && !meta.community ? keys_->checkSender(*dto, meta.id) : SenderCheck::Legacy;
        auto decoded = decode(*dto, meta, selfUserId, theirRead, theirDelivered, ring.get(), check, communityRing.get());
        if (std::holds_alternative<DecodedHidden>(decoded)) {
            reactionIds.insert(dto->id);
            continue;
        }
        if (auto* reaction = std::get_if<DecodedReaction>(&decoded)) {
            reactionIds.insert(dto->id);
            auto& tally = reactions[reaction->targetId][reaction->emoji];
            if (tally.senders.empty() || reaction->createdAtMs < tally.firstAtMs) tally.firstAtMs = reaction->createdAtMs;
            tally.senders.insert(dto->senderId);
            if (dto->senderId == selfUserId) tally.myId = dto->id;
            continue;
        }
        if (auto* edit = std::get_if<DecodedEdit>(&decoded)) {
            reactionIds.insert(dto->id);  // like a reaction: never shown or counted on its own
            edits.push_back(std::move(*edit));
            continue;
        }
        if (auto* event = std::get_if<DecodedPollEvent>(&decoded)) {
            reactionIds.insert(dto->id);
            pollEvents.push_back(std::move(*event));
            continue;
        }
        if (const auto& shown = std::get<Message>(decoded); shown.kind == MessageKind::Notice) {
            // Pins are notices: shown in the timeline, never counted as unread.
            reactionIds.insert(dto->id);
            pinEvents.push_back(PinEvent{shown.noticeTarget, shown.text, dto->senderId, shown.createdAtMs, shown.notice == "pin"});
        }
        auto& message = std::get<Message>(decoded);
        if (!hidden.contains(message.id)) result.messages.push_back(std::move(message));
    }

    std::stable_sort(edits.begin(), edits.end(),
                     [](const DecodedEdit& a, const DecodedEdit& b) { return a.createdAtMs < b.createdAtMs; });
    for (const auto& edit : edits) {
        const auto target = std::find_if(result.messages.begin(), result.messages.end(),
                                         [&](const Message& m) { return util::toLowerAscii(m.id) == edit.targetId; });
        // Only the sender may change a message, and only its text; an edit of
        // a message older than what's loaded (or of a deleted one) has nothing to apply to.
        if (target == result.messages.end() || target->senderId != edit.senderId || target->kind != MessageKind::Text) {
            continue;
        }
        const std::int64_t previousAt = target->editedAtMs.value_or(target->createdAtMs);
        target->earlierVersions.push_back(EditVersion{target->text, previousAt});
        target->text = edit.text;
        target->editedAtMs = edit.createdAtMs;
        target->editMessageIds.push_back(edit.id);
    }

    // Polls: each person's latest vote counts, until the poll's creator ends it.
    std::stable_sort(pollEvents.begin(), pollEvents.end(),
                     [](const DecodedPollEvent& a, const DecodedPollEvent& b) { return a.createdAtMs < b.createdAtMs; });
    for (auto& message : result.messages) {
        if (message.kind != MessageKind::Poll || !message.poll) continue;
        auto& poll = *message.poll;
        const std::string id = util::toLowerAscii(message.id);
        std::optional<std::int64_t> endedAt;
        for (const auto& event : pollEvents) {
            if (event.end && event.pollId == id && event.senderId == message.senderId) {
                endedAt = event.createdAtMs;
                break;
            }
        }
        poll.closed = endedAt.has_value();
        std::map<std::string, std::vector<int>> latest;
        for (const auto& event : pollEvents) {
            if (event.end || event.pollId != id || (endedAt && event.createdAtMs > *endedAt)) continue;
            std::vector<int> picked;
            for (const int option : event.options) {
                if (option >= 0 && static_cast<std::size_t>(option) < poll.options.size()) picked.push_back(option);
                if (!poll.multiple && !picked.empty()) break;
            }
            latest[event.senderId] = std::move(picked);
        }
        for (const auto& [voter, picked] : latest) {
            if (picked.empty()) continue;
            ++poll.voters;
            for (const int option : picked) ++poll.counts[static_cast<std::size_t>(option)];
            if (voter == selfUserId) poll.mine = picked;
        }
    }

    // Pins, oldest first: pinning again moves a message to the end; only the newest kMaxPins stay.
    std::stable_sort(pinEvents.begin(), pinEvents.end(),
                     [](const PinEvent& a, const PinEvent& b) { return a.createdAtMs < b.createdAtMs; });
    for (const auto& event : pinEvents) {
        std::erase_if(result.pinned, [&](const PinnedView& p) { return p.messageId == event.targetId; });
        if (!event.pin) continue;
        result.pinned.push_back(PinnedView{event.targetId, event.preview, event.senderId, event.createdAtMs});
        if (result.pinned.size() > kMaxPins) result.pinned.erase(result.pinned.begin());
    }
    // A pinned message that's gone (deleted, or hidden on this computer) drops out once it's known to be gone.
    std::erase_if(result.pinned, [&](const PinnedView& p) { return hidden.contains(p.messageId) || deletedIds.contains(p.messageId); });

    // The quick six first, in their order; any other emoji after them, in the order they were first used.
    const auto quickIndex = [](const std::string& emoji) {
        const auto it = std::find(rich_message::kReactionEmoji.begin(), rich_message::kReactionEmoji.end(), emoji);
        return static_cast<std::size_t>(it - rich_message::kReactionEmoji.begin());
    };
    for (auto& message : result.messages) {
        const auto it = reactions.find(util::toLowerAscii(message.id));
        if (it == reactions.end()) continue;
        std::vector<std::pair<const std::string*, const EmojiTally*>> ordered;
        for (const auto& [emoji, tally] : it->second) ordered.emplace_back(&emoji, &tally);
        std::stable_sort(ordered.begin(), ordered.end(), [&](const auto& a, const auto& b) {
            return std::tuple(quickIndex(*a.first), a.second->firstAtMs) < std::tuple(quickIndex(*b.first), b.second->firstAtMs);
        });
        for (const auto& [emoji, tally] : ordered) {
            message.reactions.push_back(ReactionView{*emoji, static_cast<int>(tally->senders.size()), tally->myId.has_value(), tally->myId});
        }
    }
    std::stable_sort(result.messages.begin(), result.messages.end(),
                     [](const Message& a, const Message& b) { return a.createdAtMs < b.createdAtMs; });

    // Reactions and edits don't count as unread messages.
    for (const auto* dto : live) {
        if (dto->senderId != selfUserId && !mine.contains(dto->id) && !reactionIds.contains(dto->id)) ++result.unread;
    }
    return result;
}

void ConversationRepository::markRead(const std::string& conversationId, bool readReceipts) {
    api_->markRead(conversationId, {.status = readReceipts ? "read" : "delivered"});
}

Message ConversationRepository::send(const ConversationMeta& meta, const std::string& text,
                                     const std::optional<ReplyTarget>& replyTo,
                                     const std::optional<std::string>& clientMessageId, const PaidSend& paid) {
    const std::string trimmed = cleanOutgoingText(text);
    if (trimmed.empty()) throw std::invalid_argument("Empty message");
    const std::string body = replyTo ? rich_message::serializeReply(trimmed, replyTo->ref) : trimmed;
    Message message = sendEnvelope(meta, body, std::nullopt, clientMessageId, paid);
    message.kind = MessageKind::Text;
    message.text = trimmed;
    if (replyTo) {
        message.replyTo =
            ReplyView{replyTo->ref.messageId, rich_message::makePreview(replyTo->ref.preview), replyTo->fromMe};
    }
    return message;
}

void ConversationRepository::sendEdit(const ConversationMeta& meta, const std::string& messageId,
                                      const std::string& text) {
    const std::string trimmed = cleanOutgoingText(text);
    if (trimmed.empty()) throw std::invalid_argument("Empty message");
    sendEnvelope(meta, rich_message::serializeEdit(trimmed, util::toLowerAscii(messageId)), std::nullopt);
}

Message ConversationRepository::sendForwarded(const ConversationMeta& meta, const std::string& text) {
    const std::string trimmed = cleanOutgoingText(text);
    if (trimmed.empty()) throw std::invalid_argument("Empty message");
    Message message = sendEnvelope(meta, rich_message::serializeForwarded(trimmed), std::nullopt);
    message.kind = MessageKind::Text;
    message.text = trimmed;
    message.forwarded = true;
    return message;
}

Message ConversationRepository::sendChannelForward(const ConversationMeta& meta, const std::string& text,
                                                  const model::ChannelPostRef& ref) {
    const std::string trimmed = cleanOutgoingText(text);
    if (trimmed.empty()) throw std::invalid_argument("Empty message");
    const std::string body = rich_message::serializeChannelForward(trimmed, ref);
    Message message = sendEnvelope(meta, body, std::nullopt);
    message.kind = MessageKind::Text;
    message.text = trimmed;
    message.forwarded = true;
    if (const auto parsed = rich_message::parse(body); const auto* t = std::get_if<model::parsed::Text>(&parsed)) {
        message.channelPost = t->channelPost;
    }
    return message;
}

Message ConversationRepository::sendStoryReply(const ConversationMeta& meta, const std::string& text, const model::StoryReplyRef& ref) {
    const std::string trimmed = cleanOutgoingText(text);
    if (trimmed.empty()) throw std::invalid_argument("Empty message");
    Message message = sendEnvelope(meta, rich_message::serializeStoryReply(trimmed, ref), std::nullopt);
    message.kind = MessageKind::Text;
    message.text = trimmed;
    message.storyReply = ref;
    return message;
}

void ConversationRepository::sendPin(const ConversationMeta& meta, const std::string& messageId, const std::string& preview) {
    sendEnvelope(meta, rich_message::serializePin(util::toLowerAscii(messageId), preview), std::nullopt);
}

void ConversationRepository::sendUnpin(const ConversationMeta& meta, const std::string& messageId) {
    sendEnvelope(meta, rich_message::serializeUnpin(util::toLowerAscii(messageId)), std::nullopt);
}

void ConversationRepository::sendPoll(const ConversationMeta& meta, const std::string& question,
                                      const std::vector<std::string>& options, bool multiple) {
    sendEnvelope(meta, rich_message::serializePoll(question, options, multiple), std::nullopt);
}

void ConversationRepository::sendVote(const ConversationMeta& meta, const Message& poll, const std::vector<int>& options) {
    if (!poll.poll) throw std::invalid_argument("Not a poll");
    sendEnvelope(meta, rich_message::serializeVote(util::toLowerAscii(poll.id), options, poll.poll->options), std::nullopt);
}

void ConversationRepository::sendEndPoll(const ConversationMeta& meta, const std::string& pollId) {
    sendEnvelope(meta, rich_message::serializeEndPoll(util::toLowerAscii(pollId)), std::nullopt);
}

void ConversationRepository::sendReaction(const ConversationMeta& meta, const std::string& messageId,
                                          const std::string& emoji) {
    sendEnvelope(meta, rich_message::serializeReaction(messageId, emoji), std::nullopt);
}

Message ConversationRepository::sendFile(const ConversationMeta& meta, const model::FileDescriptor& descriptor,
                                         const std::optional<std::string>& clientMessageId) {
    Message message = sendEnvelope(meta, file_message::serialize(descriptor), std::vector<std::string>{descriptor.objectId},
                                   clientMessageId);
    message.kind = MessageKind::File;
    message.text = descriptor.filename;
    message.file = descriptor;
    message.fileStatus = "available";
    return message;
}

namespace {

/// An own message as just sent, with its stage-3 check.
Message sentMessage(const api::MessageDto& dto, const crypto::AeadCiphertext& encrypted, const std::string& ciphertextB64,
                    const std::string& nonceB64) {
    Message message;
    message.id = dto.id;
    message.fromMe = true;
    message.senderId = dto.senderId;
    message.createdAtMs = parseMs(dto.createdAt);
    message.deleteAtMs = parseMs(dto.deleteAt);
    message.security = buildSecurity(dto.id, encrypted.ciphertext, encrypted.nonce, dto.verification);
    message.ciphertextB64 = ciphertextB64;
    message.nonceB64 = nonceB64;
    return message;
}

}  // namespace

Message ConversationRepository::sendEnvelope(const ConversationMeta& meta, const std::string& plaintext,
                                             const std::optional<std::vector<std::string>>& attachments,
                                             const std::optional<std::string>& clientMessageId, const PaidSend& paid) {
    // A community has its own epoch keys and its messages are not signed (see sendToCommunity).
    if (meta.community && communityKeys_) return sendToCommunity(meta, plaintext, attachments, clientMessageId);
    // Encrypted with the conversation's current key (epoch) and signed. When the
    // server says the group needs a new key (someone left) or the key changed,
    // this rotates or re-reads the keys and sends again.
    constexpr int kMaxKeyRefreshes = 3;
    for (int attempt = 1;; ++attempt) {
        const auto ring = keys_ ? keys_->ringFor(meta.id) : nullptr;
        // Nothing goes out under a key that was refused, whatever asked to send.
        if (ring && ring->keyProblem) throw api::UserFacingError(*ring->keyProblem);
        const int epoch = ring ? ring->currentEpoch : 0;
        const crypto::SecretBytes* key = ring ? ring->key(epoch) : &meta.conversationKey;
        if (key == nullptr) throw api::UserFacingError(KeyTrust::kGroupKeyUnopened);
        const auto encrypted = crypto::encryptWithConversationKey(plaintext, *key);
        const std::string ciphertextB64 = base64Encode(encrypted.ciphertext);
        const std::string nonceB64 = base64Encode(encrypted.nonce);
        api::SendMessageRequest request{.ciphertext = ciphertextB64, .nonce = nonceB64, .attachments = attachments};
        request.clientMessageId = clientMessageId;
        request.entitlementId = paid.entitlementId;
        request.safetyContact = paid.safetyContact;
        if (keys_) {
            request.keyEpoch = epoch;
            request.signature = keys_->signatureFor(meta.id, epoch, encrypted.nonce, encrypted.ciphertext);
        }
        api::MessageDto dto;
        try {
            dto = api_->sendMessage(meta.id, request);
        } catch (const api::ApiError& e) {
            if (!keys_ || attempt >= kMaxKeyRefreshes) throw;
            if (e.code() == "group_rekey_required") {
                keys_->rotate(meta.id);
                continue;
            }
            if (e.code() == "stale_key_epoch") {
                keys_->refresh(meta.id);
                continue;
            }
            throw;
        }

    Message message;
    message.id = dto.id;
    message.fromMe = true;
    message.senderId = dto.senderId;
    message.createdAtMs = parseMs(dto.createdAt);
    message.deleteAtMs = parseMs(dto.deleteAt);
    // An outbox retry whose first attempt did arrive: the server answers with
    // the copy it stored then (encrypted with another nonce). Verify that
    // copy, after checking it really holds this plaintext.
    if (clientMessageId && dto.ciphertext && dto.nonce && *dto.ciphertext != ciphertextB64) {
        const crypto::Bytes storedCiphertext = base64Decode(*dto.ciphertext);
        const crypto::Bytes storedNonce = base64Decode(*dto.nonce);
        bool same = false;
        try {
            const auto storedRing = keys_ ? keys_->ringFor(meta.id) : nullptr;
            const crypto::SecretBytes* storedKey =
                storedRing ? storedRing->key(dto.keyEpoch) : &meta.conversationKey;
            same = storedKey != nullptr &&
                   crypto::decryptWithConversationKey(storedCiphertext, storedNonce, *storedKey) == plaintext;
        } catch (const std::exception&) {
            same = false;
        }
        if (same) {
            message.security = buildSecurity(dto.id, storedCiphertext, storedNonce, dto.verification);
            message.ciphertextB64 = *dto.ciphertext;
            message.nonceB64 = *dto.nonce;
            return message;
        }
        // Not ours after all: report it rather than trusting the stored copy.
        MessageSecurity failed;
        failed.verification = VerificationStatus::Failed;
        message.security = failed;
        return message;
    }
    message.security = buildSecurity(dto.id, encrypted.ciphertext, encrypted.nonce, dto.verification);
    message.ciphertextB64 = ciphertextB64;
    message.nonceB64 = nonceB64;
    return message;
    }
}

Message ConversationRepository::sendToCommunity(const ConversationMeta& meta, const std::string& plaintext,
                                                const std::optional<std::vector<std::string>>& attachments,
                                                const std::optional<std::string>& clientMessageId) {
    const std::string& communityId = meta.community->id;
    const auto reload = [&] { return communityKeys_->load(communityId, meta.id); };
    for (int attempt = 0;; ++attempt) {
        auto ring = communityKeys_->ring(meta.id);
        if (!ring) ring = reload();
        if (!ring->hasCurrentKey()) {
            // Maybe only this copy of the keys is behind: look once more before giving up.
            ring = reload();
            if (!ring->hasCurrentKey()) throw api::UserFacingError("You don't have the community's current key yet.");
        }
        const auto sealed = crypto::community::encryptForCommunity(*ring, plaintext);
        const std::string ciphertextB64 = base64Encode(sealed.body.ciphertext);
        const std::string nonceB64 = base64Encode(sealed.body.nonce);
        try {
            api::SendMessageRequest request{.ciphertext = ciphertextB64, .nonce = nonceB64, .attachments = attachments};
            request.keyEpoch = static_cast<int>(sealed.keyEpoch);
            request.clientMessageId = clientMessageId;
            const auto dto = api_->sendMessage(meta.id, request);
            return sentMessage(dto, sealed.body, ciphertextB64, nonceB64);
        } catch (const api::ApiError& e) {
            if (attempt >= 2) throw;
            if (e.code() == "rekey_required") {
                // Someone left: make the next epoch, sealed only to who remains. If another app got
                // there first, or this member can't rotate, the reloaded keys say what to use.
                try {
                    communityKeys_->rotate(communityId, meta.id);
                } catch (const api::ApiError& rotateError) {
                    if (rotateError.status() == 401) throw;
                    reload();
                } catch (const crypto::CryptoError&) {
                    reload();
                }
            } else if (e.code() == "stale_key_epoch") {
                reload();
            } else {
                throw;
            }
        }
    }
}

MessageSecurity ConversationRepository::verify(const Message& message) {
    if (!message.ciphertextB64 || !message.nonceB64) return MessageSecurity{};
    try {
        const auto record = api_->getMessageVerification(message.id);
        return buildSecurity(message.id, base64Decode(*message.ciphertextB64), base64Decode(*message.nonceB64), record);
    } catch (const std::exception&) {
        return MessageSecurity{};
    }
}

void ConversationRepository::deleteMessage(const std::string& messageId) {
    api_->deleteMessage(messageId);
}

void ConversationRepository::setDisappearing(const std::string& conversationId, const Disappearing& duration) {
    api_->setDisappearing(conversationId, duration.seconds);
}

}  // namespace redvault::data
