#pragma once

#include <array>
#include <string>
#include <string_view>
#include <optional>
#include <variant>
#include <vector>

// Port of android/.../data/model/RichMessage.kt (itself ui/src/lib/richMessage.ts):
// replies and reactions carried inside the ordinary E2EE body, so the server
// learns nothing new. Byte-for-byte the same wire format (pinned by
// tests/rich_message_test.cpp); change all three together.
//
//   reply:    "\0rv-msg:" + {"type":"text","version":1,"text":…,"replyTo":{"messageId","senderId","preview"}}
//   reaction: "\0rv-reaction:" + {"type":"reaction","version":1,"messageId","emoji"}
//
// Plain (non-reply) text stays plain. Removing a reaction deletes the
// reaction message itself. Everything parsed is untrusted input.
//
// Desktop additions, as optional fields of the "text" type so clients that
// don't know them still show the text (they ignore unknown fields):
//
//   edit:      {"type":"text","version":1,"text":<new text>,"edits":<messageId>}
//              replaces the text of an earlier message by the SAME sender
//              (checked by the reader); older clients show it as a new message.
//   forwarded: {"type":"text","version":1,"text":…,"forwarded":true}
//   pin:       {…,"text":"📌 Pinned: <preview>","pin":{"messageId","preview"}}
//   unpin:     {…,"text":"Unpinned a message","unpin":<messageId>}
//   poll:      {…,"text":"📊 <question> …options…","poll":{"question","options":[…],"multiple":bool}}
//   vote:      {…,"text":"Voted: …","vote":{"pollId","options":[indexes]}}  (empty: vote taken back)
//   end poll:  {…,"text":"Ended the poll","endPoll":<pollId>}  (only its creator)
//   channel:   {…,"forwarded":true,"channelPost":{"channelId","postId","channelName"}}
//              a channel post forwarded into a chat; "text" is the post. Each
//              copy that's shown counts a view for the original post.
//   story reply: {…,"text":<reply or emoji>,"storyReply":{"authorId","storyId","kind","preview","reaction"}}
//              a reply or quick reaction to someone's story, sent to its author
//
// In each, "text" is what older apps show instead.

namespace redvault::model {

struct ReplyRef {
    std::string messageId;
    std::string senderId;
    std::string preview;

    bool operator==(const ReplyRef&) const = default;
};

struct PinRef {
    std::string messageId;
    std::string preview;

    bool operator==(const PinRef&) const = default;
};

struct Poll {
    std::string question;
    std::vector<std::string> options;
    bool multiple = false;

    bool operator==(const Poll&) const = default;
};

struct Vote {
    std::string pollId;
    std::vector<int> options;

    bool operator==(const Vote&) const = default;
};

/// The channel post a forwarded message came from: shown as "Forwarded from <channel>", and counted as a view of it.
struct ChannelPostRef {
    std::string channelId;
    std::string postId;
    std::string channelName;

    bool operator==(const ChannelPostRef&) const = default;
};

/// Which story a reply or reaction is about (never its key): kind is "image", "video" or "text".
struct StoryReplyRef {
    std::string authorId;
    std::string storyId;
    std::string kind;
    /// "Photo", "Video", or the start of a text story.
    std::string preview;
    bool reaction = false;

    bool operator==(const StoryReplyRef&) const = default;
};

namespace parsed {

struct Text {
    std::string text;
    std::optional<ReplyRef> replyTo;
    /// This is an edit: the new text of that (lowercase) message id.
    std::optional<std::string> edits;
    bool forwarded = false;
    std::optional<PinRef> pin;
    std::optional<std::string> unpin;
    std::optional<Poll> poll;
    std::optional<Vote> vote;
    std::optional<std::string> endPoll;
    std::optional<ChannelPostRef> channelPost;
    std::optional<StoryReplyRef> storyReply;

    bool operator==(const Text&) const = default;
};

struct Reaction {
    std::string messageId;
    std::string emoji;

    bool operator==(const Reaction&) const = default;
};

struct Invalid {
    bool operator==(const Invalid&) const = default;
};

}  // namespace parsed

using ParsedBody = std::variant<parsed::Text, parsed::Reaction, parsed::Invalid>;

namespace rich_message {

inline const std::string kTextPrefix = std::string("\0rv-msg:", 8);
inline const std::string kReactionPrefix = std::string("\0rv-reaction:", 13);

/// The quick reactions, offered first and shown first, in this order. Any
/// other single emoji is a valid reaction too (isReactionEmoji).
inline constexpr std::array<std::string_view, 6> kReactionEmoji = {
    "\xF0\x9F\x91\x8D",          // 👍
    "\xE2\x9D\xA4\xEF\xB8\x8F",  // ❤️
    "\xF0\x9F\x98\x82",          // 😂
    "\xF0\x9F\x98\xAE",          // 😮
    "\xF0\x9F\x98\xA2",          // 😢
    "\xF0\x9F\x99\x8F",          // 🙏
};

bool isStructured(std::string_view plaintext);

/// A short, safe quote: hidden characters stripped, whitespace collapsed, capped at 200 UTF-16 units like the web.
std::string makePreview(std::string_view text);

std::string serializeReply(std::string_view text, const ReplyRef& replyTo);

/// A new version of the text of `messageId` (one of your own messages).
std::string serializeEdit(std::string_view text, std::string_view messageId);

/// Text forwarded from another chat; `replyTo` is never carried over.
std::string serializeForwarded(std::string_view text);

/// Longest channel name a forwarded post carries (the server's MAX_CHANNEL_NAME), in UTF-16 units.
inline constexpr std::size_t kMaxChannelName = 80;

/// A channel post forwarded into a chat, keeping where it came from (also when forwarded on again).
std::string serializeChannelForward(std::string_view text, const ChannelPostRef& ref);

inline constexpr std::size_t kMaxPollOptions = 10;
inline constexpr std::size_t kMaxPollQuestion = 300;
inline constexpr std::size_t kMaxPollOption = 100;

std::string serializePin(std::string_view messageId, std::string_view preview);
std::string serializeUnpin(std::string_view messageId);
/// Throws std::invalid_argument unless there's a question and 2-10 distinct, non-empty options.
std::string serializePoll(std::string_view question, const std::vector<std::string>& options, bool multiple);
/// `optionLabels` only make the fallback text; an empty `options` takes the vote back.
std::string serializeVote(std::string_view pollId, const std::vector<int>& options, const std::vector<std::string>& optionLabels);
std::string serializeEndPoll(std::string_view pollId);
/// A reply (or quick reaction emoji) to a story, as a message to its author.
std::string serializeStoryReply(std::string_view text, const StoryReplyRef& ref);

/// Whether `emoji` is exactly one emoji: a pictograph (optionally with VS16,
/// a skin tone and tag characters), a flag or a keycap, or several of those
/// joined by ZWJ. The same rule as the web and Android apps.
bool isReactionEmoji(std::string_view emoji);

/// Throws std::invalid_argument unless `emoji` is one emoji (isReactionEmoji).
std::string serializeReaction(std::string_view messageId, std::string_view emoji);

ParsedBody parse(std::string_view plaintext);

}  // namespace rich_message

}  // namespace redvault::model
