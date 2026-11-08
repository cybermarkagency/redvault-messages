// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/model/rich_message.hpp"

#include <algorithm>
#include <regex>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "redvault/util/text.hpp"

namespace redvault::model::rich_message {

namespace {

using Json = nlohmann::json;
// Field order matters for byte-identical output with the web and Android.
using OrderedJson = nlohmann::ordered_json;

constexpr std::size_t kMaxText = 65'536;
constexpr std::size_t kMaxPreview = 200;

bool isHidden(char32_t c) {
    // Kotlin: [\u0000-\u0008 \u000b-\u001f \u007f-\u009f \u200b-\u200f \u202a-\u202e \u2060-\u2069 \ufeff]
    return c <= 0x08 || (c >= 0x0B && c <= 0x1F) || (c >= 0x7F && c <= 0x9F) || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2069) || c == 0xFEFF;
}

bool isUuid(const std::string& text) {
    static const std::regex kUuid("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", std::regex::icase);
    return std::regex_match(text, kUuid);
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

std::string dump(const OrderedJson& json) {
    // Replace, not throw, if a caller hands over invalid UTF-8.
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// Reactions: exactly one emoji, by the same grammar as
// ui/src/lib/richMessage.ts and RichMessage.kt (change all three together).
constexpr std::size_t kMaxEmojiCodePoints = 16;
constexpr char32_t kZwj = 0x200D;
constexpr char32_t kVs16 = 0xFE0F;
constexpr char32_t kKeycap = 0x20E3;

bool isRegional(char32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }
bool isSkinTone(char32_t c) { return c >= 0x1F3FB && c <= 0x1F3FF; }
bool isTag(char32_t c) { return c >= 0xE0020 && c <= 0xE007F; }

/// The emoji blocks, plus the older symbols that are drawn as emoji.
bool isPictograph(char32_t c) {
    static constexpr std::pair<char32_t, char32_t> kRanges[] = {
        {0x00A9, 0x00A9}, {0x00AE, 0x00AE}, {0x203C, 0x203C}, {0x2049, 0x2049}, {0x2122, 0x2122}, {0x2139, 0x2139},
        {0x2194, 0x2199}, {0x21A9, 0x21AA}, {0x231A, 0x231B}, {0x2328, 0x2328}, {0x23CF, 0x23CF}, {0x23E9, 0x23F3},
        {0x23F8, 0x23FA}, {0x24C2, 0x24C2}, {0x25AA, 0x25AB}, {0x25B6, 0x25B6}, {0x25C0, 0x25C0}, {0x25FB, 0x25FE},
        {0x2600, 0x27BF}, {0x2934, 0x2935}, {0x2B05, 0x2B07}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
        {0x3030, 0x3030}, {0x303D, 0x303D}, {0x3297, 0x3297}, {0x3299, 0x3299}, {0x1F000, 0x1FAFF},
    };
    if (isRegional(c) || isSkinTone(c)) return false;
    return std::any_of(std::begin(kRanges), std::end(kRanges), [c](const auto& r) { return c >= r.first && c <= r.second; });
}

}  // namespace

bool isReactionEmoji(std::string_view emoji) {
    const std::u32string cps = util::decodeUtf8(emoji);  // invalid UTF-8 becomes U+FFFD, which fails below
    const std::size_t n = cps.size();
    if (n == 0 || n > kMaxEmojiCodePoints) return false;
    // A keycap: 0-9, # or *, an optional VS16, then U+20E3.
    if ((cps[0] >= U'0' && cps[0] <= U'9') || cps[0] == U'#' || cps[0] == U'*') {
        return (n == 2 && cps[1] == kKeycap) || (n == 3 && cps[1] == kVs16 && cps[2] == kKeycap);
    }
    std::size_t i = 0;
    const auto at = [&](std::size_t k) -> char32_t { return k < n ? cps[k] : 0; };
    // One element: a flag (two regional indicators), or a pictograph with an
    // optional VS16, then an optional skin tone, then any tag characters.
    const auto element = [&]() {
        if (isRegional(at(i))) {
            if (!isRegional(at(i + 1))) return false;
            i += 2;
            return true;
        }
        if (!isPictograph(at(i))) return false;
        ++i;
        if (at(i) == kVs16) ++i;
        if (isSkinTone(at(i))) ++i;
        while (isTag(at(i))) ++i;
        return true;
    };
    if (!element()) return false;
    while (i < n) {
        if (cps[i] != kZwj) return false;
        ++i;
        if (!element()) return false;
    }
    return true;
}

bool isStructured(std::string_view plaintext) {
    return plaintext.starts_with(kTextPrefix) || plaintext.starts_with(kReactionPrefix);
}

std::string makePreview(std::string_view text) {
    std::u32string clean = util::decodeUtf8(text);
    std::erase_if(clean, isHidden);
    clean = util::trim(util::collapseWhitespace(clean));
    if (util::utf16Length(clean) > kMaxPreview) clean = util::takeUtf16(clean, kMaxPreview - 1) + U"\u2026";
    return util::encodeUtf8(clean);
}

std::string serializeReply(std::string_view text, const ReplyRef& replyTo) {
    OrderedJson body;
    body["type"] = "text";
    body["version"] = 1;
    body["text"] = std::string(text);
    body["replyTo"] = OrderedJson{{"messageId", replyTo.messageId},
                                  {"senderId", replyTo.senderId},
                                  {"preview", makePreview(replyTo.preview)}};
    return kTextPrefix + dump(body);
}

std::string serializeEdit(std::string_view text, std::string_view messageId) {
    OrderedJson body;
    body["type"] = "text";
    body["version"] = 1;
    body["text"] = std::string(text);
    body["edits"] = std::string(messageId);
    return kTextPrefix + dump(body);
}

std::string serializeForwarded(std::string_view text) {
    OrderedJson body;
    body["type"] = "text";
    body["version"] = 1;
    body["text"] = std::string(text);
    body["forwarded"] = true;
    return kTextPrefix + dump(body);
}

namespace {

OrderedJson textBody(std::string_view fallback) {
    OrderedJson body;
    body["type"] = "text";
    body["version"] = 1;
    body["text"] = std::string(fallback);
    return body;
}

std::string trimmed(std::string_view text) {
    return util::encodeUtf8(util::trim(util::decodeUtf8(text)));
}

/// makePreview(name).slice(0, MAX_CHANNEL_NAME), as the web app cleans a channel's name.
std::string channelNameOf(std::string_view name) {
    return util::encodeUtf8(util::takeUtf16(util::decodeUtf8(makePreview(name)), kMaxChannelName));
}

}  // namespace

std::string serializeChannelForward(std::string_view text, const ChannelPostRef& ref) {
    auto body = textBody(text);
    body["forwarded"] = true;
    body["channelPost"] = OrderedJson{{"channelId", util::toLowerAscii(ref.channelId)},
                                      {"postId", util::toLowerAscii(ref.postId)},
                                      {"channelName", channelNameOf(ref.channelName)}};
    return kTextPrefix + dump(body);
}

std::string serializePin(std::string_view messageId, std::string_view preview) {
    const std::string quote = makePreview(preview);
    auto body = textBody("\xF0\x9F\x93\x8C Pinned: " + quote);  // 📌
    body["pin"] = OrderedJson{{"messageId", std::string(messageId)}, {"preview", quote}};
    return kTextPrefix + dump(body);
}

std::string serializeUnpin(std::string_view messageId) {
    auto body = textBody("Unpinned a message");
    body["unpin"] = std::string(messageId);
    return kTextPrefix + dump(body);
}

std::string serializePoll(std::string_view question, const std::vector<std::string>& options, bool multiple) {
    const std::string q = trimmed(question);
    if (q.empty() || util::utf16Length(util::decodeUtf8(q)) > kMaxPollQuestion) throw std::invalid_argument("Poll question");
    std::vector<std::string> clean;
    for (const auto& option : options) {
        const std::string o = trimmed(option);
        if (o.empty()) continue;
        if (util::utf16Length(util::decodeUtf8(o)) > kMaxPollOption) throw std::invalid_argument("Poll option too long");
        if (std::find(clean.begin(), clean.end(), o) != clean.end()) throw std::invalid_argument("Duplicate poll option");
        clean.push_back(o);
    }
    if (clean.size() < 2 || clean.size() > kMaxPollOptions) throw std::invalid_argument("A poll needs 2 to 10 options");
    std::string fallback = "\xF0\x9F\x93\x8A Poll: " + q;  // 📊
    for (std::size_t i = 0; i < clean.size(); ++i) fallback += "\n" + std::to_string(i + 1) + ". " + clean[i];
    auto body = textBody(fallback);
    body["poll"] = OrderedJson{{"question", q}, {"options", clean}, {"multiple", multiple}};
    return kTextPrefix + dump(body);
}

std::string serializeVote(std::string_view pollId, const std::vector<int>& options, const std::vector<std::string>& optionLabels) {
    std::string fallback = options.empty() ? "Took back a poll vote" : "Voted: ";
    for (std::size_t i = 0; i < options.size(); ++i) {
        const int index = options[i];
        if (i > 0) fallback += ", ";
        fallback += index >= 0 && static_cast<std::size_t>(index) < optionLabels.size() ? optionLabels[index] : std::to_string(index + 1);
    }
    auto body = textBody(fallback);
    body["vote"] = OrderedJson{{"pollId", std::string(pollId)}, {"options", options}};
    return kTextPrefix + dump(body);
}

std::string serializeEndPoll(std::string_view pollId) {
    auto body = textBody("Ended the poll");
    body["endPoll"] = std::string(pollId);
    return kTextPrefix + dump(body);
}

std::string serializeStoryReply(std::string_view text, const StoryReplyRef& ref) {
    OrderedJson body = textBody(text);
    OrderedJson story;
    story["authorId"] = util::toLowerAscii(ref.authorId);
    story["storyId"] = util::toLowerAscii(ref.storyId);
    story["kind"] = ref.kind;
    story["preview"] = util::encodeUtf8(util::takeUtf16(util::decodeUtf8(makePreview(ref.preview)), 80));
    story["reaction"] = ref.reaction;
    body["storyReply"] = std::move(story);
    return kTextPrefix + dump(body);
}

std::string serializeReaction(std::string_view messageId, std::string_view emoji) {
    if (!isReactionEmoji(emoji)) throw std::invalid_argument("Unsupported reaction");
    OrderedJson body;
    body["type"] = "reaction";
    body["version"] = 1;
    body["messageId"] = std::string(messageId);
    body["emoji"] = std::string(emoji);
    return kReactionPrefix + dump(body);
}

ParsedBody parse(std::string_view plaintext) {
    const bool isReaction = plaintext.starts_with(kReactionPrefix);
    std::string_view body = plaintext;
    if (isReaction) {
        body.remove_prefix(kReactionPrefix.size());
    } else if (plaintext.starts_with(kTextPrefix)) {
        body.remove_prefix(kTextPrefix.size());
    }

    const Json d = Json::parse(body, nullptr, false);
    if (d.is_discarded() || !d.is_object()) return parsed::Invalid{};
    if (integer(d, "version") != 1) return parsed::Invalid{};

    if (isReaction) {
        const auto messageId = str(d, "messageId");
        if (!messageId || !isUuid(*messageId)) return parsed::Invalid{};
        const auto emoji = str(d, "emoji");
        if (!emoji || !isReactionEmoji(*emoji)) {
            return parsed::Invalid{};
        }
        if (str(d, "type") != "reaction") return parsed::Invalid{};
        return parsed::Reaction{util::toLowerAscii(*messageId), *emoji};
    }

    if (str(d, "type") != "text") return parsed::Invalid{};
    const auto text = str(d, "text");
    if (!text || util::utf16Length(util::decodeUtf8(*text)) > kMaxText) return parsed::Invalid{};

    parsed::Text out;
    out.text = *text;
    if (d.contains("edits")) {
        const auto target = str(d, "edits");
        if (!target || !isUuid(*target)) return parsed::Invalid{};
        out.edits = util::toLowerAscii(*target);
    }
    if (d.contains("forwarded")) {
        if (!d["forwarded"].is_boolean()) return parsed::Invalid{};
        out.forwarded = d["forwarded"].get<bool>();
    }
    if (d.contains("pin")) {
        const auto& p = d["pin"];
        if (!p.is_object()) return parsed::Invalid{};
        const auto messageId = str(p, "messageId");
        const auto preview = str(p, "preview");
        if (!messageId || !isUuid(*messageId) || !preview) return parsed::Invalid{};
        out.pin = PinRef{util::toLowerAscii(*messageId), makePreview(*preview)};
    }
    if (d.contains("unpin")) {
        const auto target = str(d, "unpin");
        if (!target || !isUuid(*target)) return parsed::Invalid{};
        out.unpin = util::toLowerAscii(*target);
    }
    if (d.contains("poll")) {
        const auto& p = d["poll"];
        if (!p.is_object()) return parsed::Invalid{};
        const auto question = str(p, "question");
        if (!question || question->empty() || util::utf16Length(util::decodeUtf8(*question)) > kMaxPollQuestion) {
            return parsed::Invalid{};
        }
        const auto options = p.find("options");
        if (options == p.end() || !options->is_array() || options->size() < 2 || options->size() > kMaxPollOptions) {
            return parsed::Invalid{};
        }
        Poll poll{*question, {}, false};
        for (const auto& option : *options) {
            if (!option.is_string()) return parsed::Invalid{};
            const auto label = option.get<std::string>();
            if (label.empty() || util::utf16Length(util::decodeUtf8(label)) > kMaxPollOption) return parsed::Invalid{};
            poll.options.push_back(label);
        }
        if (p.contains("multiple")) {
            if (!p["multiple"].is_boolean()) return parsed::Invalid{};
            poll.multiple = p["multiple"].get<bool>();
        }
        out.poll = std::move(poll);
    }
    if (d.contains("vote")) {
        const auto& v = d["vote"];
        if (!v.is_object()) return parsed::Invalid{};
        const auto pollId = str(v, "pollId");
        const auto options = v.find("options");
        if (!pollId || !isUuid(*pollId) || options == v.end() || !options->is_array() || options->size() > kMaxPollOptions) {
            return parsed::Invalid{};
        }
        Vote vote{util::toLowerAscii(*pollId), {}};
        for (const auto& option : *options) {
            if (!option.is_number_integer()) return parsed::Invalid{};
            const auto index = option.get<std::int64_t>();
            if (index < 0 || index >= static_cast<std::int64_t>(kMaxPollOptions)) return parsed::Invalid{};
            if (std::find(vote.options.begin(), vote.options.end(), static_cast<int>(index)) == vote.options.end()) {
                vote.options.push_back(static_cast<int>(index));
            }
        }
        out.vote = std::move(vote);
    }
    if (d.contains("endPoll")) {
        const auto target = str(d, "endPoll");
        if (!target || !isUuid(*target)) return parsed::Invalid{};
        out.endPoll = util::toLowerAscii(*target);
    }
    if (d.contains("channelPost")) {
        const auto& c = d["channelPost"];
        if (!c.is_object()) return parsed::Invalid{};
        const auto channelId = str(c, "channelId");
        const auto postId = str(c, "postId");
        const auto channelName = str(c, "channelName");
        if (!channelId || !isUuid(*channelId) || !postId || !isUuid(*postId) || !channelName) return parsed::Invalid{};
        std::string name = channelNameOf(*channelName);
        if (name.empty()) return parsed::Invalid{};
        out.channelPost = ChannelPostRef{util::toLowerAscii(*channelId), util::toLowerAscii(*postId), std::move(name)};
    }
    if (d.contains("storyReply")) {
        const auto& r = d["storyReply"];
        if (!r.is_object()) return parsed::Invalid{};
        const auto authorId = str(r, "authorId");
        const auto storyId = str(r, "storyId");
        const auto kind = str(r, "kind");
        const auto preview = str(r, "preview");
        if (!authorId || !isUuid(*authorId) || !storyId || !isUuid(*storyId)) return parsed::Invalid{};
        if (kind != "image" && kind != "video" && kind != "text") return parsed::Invalid{};
        if (!preview || !r.contains("reaction") || !r["reaction"].is_boolean()) return parsed::Invalid{};
        out.storyReply = StoryReplyRef{util::toLowerAscii(*authorId), util::toLowerAscii(*storyId), *kind,
                                       util::encodeUtf8(util::takeUtf16(util::decodeUtf8(makePreview(*preview)), 80)), r["reaction"].get<bool>()};
    }
    if (!d.contains("replyTo")) return out;
    const auto& r = d["replyTo"];
    if (!r.is_object()) return parsed::Invalid{};
    const auto messageId = str(r, "messageId");
    if (!messageId || !isUuid(*messageId)) return parsed::Invalid{};
    const auto senderId = str(r, "senderId");
    if (!senderId || !isUuid(*senderId)) return parsed::Invalid{};
    const auto preview = str(r, "preview");
    if (!preview) return parsed::Invalid{};
    out.replyTo = ReplyRef{util::toLowerAscii(*messageId), util::toLowerAscii(*senderId), makePreview(*preview)};
    return out;
}

}  // namespace redvault::model::rich_message
