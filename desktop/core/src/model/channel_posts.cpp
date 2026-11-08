// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/model/channel_posts.hpp"

#include <array>
#include <cmath>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "redvault/model/text_format.hpp"
#include "redvault/util/text.hpp"

namespace redvault::model::channel_posts {

namespace {

// Insertion order matters: the oldest entries are the ones forgotten, as with the web's localStorage objects.
using OrderedJson = nlohmann::ordered_json;

constexpr std::array<double, 3> kSpeeds = {1.0, 1.5, 2.0};
constexpr std::size_t kMaxPreview = 140;

OrderedJson readObject(std::string_view json) {
    OrderedJson parsed = OrderedJson::parse(json, nullptr, false);
    return parsed.is_object() ? parsed : OrderedJson::object();
}

/// Keeps the most recent `max` entries (the last ones added).
void cap(OrderedJson& all, std::size_t max) {
    while (all.size() > max) all.erase(all.begin());
}

std::string dump(const OrderedJson& json) {
    return json.dump(-1, ' ', false, OrderedJson::error_handler_t::replace);
}

/// Math.round: halves go up.
long long jsRound(double value) {
    return static_cast<long long>(std::floor(value + 0.5));
}

}  // namespace

bool isPhotoMime(std::string_view mime) {
    static constexpr std::array<std::string_view, 8> kPhotos = {"image/jpeg", "image/png",  "image/gif",  "image/webp",
                                                                "image/avif", "image/heic", "image/heif", "image/bmp"};
    const std::string lower = util::toLowerAscii(mime);
    for (const auto photo : kPhotos) {
        if (lower == photo) return true;
    }
    return false;
}

std::string mediaKindFor(std::string_view mime) {
    const std::string lower = util::toLowerAscii(mime);
    if (isPhotoMime(lower)) return "photo";
    if (lower.starts_with("video/")) return "video";
    if (lower.starts_with("audio/")) return "audio";
    return "file";
}

std::string kindLabel(std::string_view kind) {
    if (kind == "photo") return "Photo";
    if (kind == "video") return "Video";
    if (kind == "audio") return "Audio";
    if (kind == "voice") return "Voice clip";
    if (kind == "round") return "Round video";
    return "File";
}

std::string formatDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) return "0:00";
    const long long s = static_cast<long long>(std::floor(seconds));
    const long long h = s / 3600;
    const long long m = (s % 3600) / 60;
    const long long rest = s % 60;
    char buffer[48];
    if (h > 0) {
        std::snprintf(buffer, sizeof buffer, "%lld:%02lld:%02lld", h, m, rest);
    } else {
        std::snprintf(buffer, sizeof buffer, "%lld:%02lld", m, rest);
    }
    return buffer;
}

double nextSpeed(double current) {
    for (std::size_t i = 0; i < kSpeeds.size(); ++i) {
        if (kSpeeds[i] == current) return kSpeeds[(i + 1) % kSpeeds.size()];
    }
    // indexOf -1 on the web: back to the first.
    return kSpeeds[0];
}

std::optional<RememberedVote> rememberedVote(std::string_view json, std::string_view postId) {
    const OrderedJson all = readObject(json);
    const auto it = all.find(std::string(postId));
    if (it == all.end() || !it->is_object()) return std::nullopt;
    RememberedVote vote;
    if (const auto mine = it->find("mine"); mine != it->end() && mine->is_array()) {
        for (const auto& option : *mine) {
            if (option.is_number_integer()) vote.mine.push_back(option.get<int>());
        }
    }
    if (const auto correct = it->find("correct"); correct != it->end() && correct->is_number_integer()) vote.correct = correct->get<int>();
    if (const auto why = it->find("explanation"); why != it->end() && why->is_string()) vote.explanation = why->get<std::string>();
    return vote;
}

std::string rememberVote(std::string_view json, std::string_view postId, const std::optional<RememberedVote>& vote,
                         const std::optional<api::channels::Poll>& poll) {
    OrderedJson all = readObject(json);
    const std::string key(postId);
    if (vote && !vote->mine.empty()) {
        OrderedJson entry{{"mine", vote->mine}};
        std::optional<int> correct = vote->correct;
        std::optional<std::string> explanation = vote->explanation;
        if (poll && poll->quiz) {
            correct = poll->correct;
            explanation = poll->explanation;
        }
        if (correct || (poll && poll->quiz)) entry["correct"] = correct ? OrderedJson(*correct) : OrderedJson(nullptr);
        if (explanation || (poll && poll->quiz)) entry["explanation"] = explanation ? OrderedJson(*explanation) : OrderedJson(nullptr);
        all[key] = std::move(entry);
    } else {
        all.erase(key);
    }
    cap(all, kMaxRememberedVotes);
    return dump(all);
}

double savedPosition(std::string_view json, std::string_view mediaId) {
    const OrderedJson all = readObject(json);
    const auto it = all.find(std::string(mediaId));
    return it != all.end() && it->is_number() ? it->get<double>() : 0.0;
}

std::string savePosition(std::string_view json, std::string_view mediaId, double seconds) {
    OrderedJson all = readObject(json);
    const std::string key(mediaId);
    if (std::isfinite(seconds) && seconds > 0) {
        all[key] = jsRound(seconds);
    } else {
        all.erase(key);
    }
    cap(all, kMaxRememberedPositions);
    return dump(all);
}

std::vector<int> percents(const api::channels::Poll& poll) {
    std::vector<int> out;
    for (std::size_t i = 0; i < poll.options.size(); ++i) {
        const int count = i < poll.counts.size() ? poll.counts[i] : 0;
        out.push_back(poll.voters > 0 ? static_cast<int>(jsRound(static_cast<double>(count) / poll.voters * 100)) : 0);
    }
    return out;
}

std::string postPreview(const api::channels::Post* post) {
    if (post == nullptr) return "New post";
    std::u32string text = util::trim(util::collapseWhitespace(util::decodeUtf8(text_format::toPlain(post->body))));
    if (!text.empty()) {
        if (util::utf16Length(text) > kMaxPreview) text = util::takeUtf16(text, kMaxPreview - 1) + U"…";
        return util::encodeUtf8(text);
    }
    if (post->poll) return std::string(post->poll->quiz ? "Quiz: " : "Poll: ") + post->poll->question;
    if (post->checklist) return "Checklist: " + post->checklist->title;
    if (post->media.empty()) return "New post";
    const std::string& kind = post->media.front().kind;
    if (kind == "photo") return "Photo";
    if (kind == "video") return "Video";
    if (kind == "voice") return "Voice message";
    if (kind == "round") return "Video message";
    if (kind == "audio") return "Audio";
    if (kind == "file") return "File";
    return "New post";
}

WatchOutcome watch(const channels::FollowedChannel& followed, const api::channels::Channel& channel,
                   std::map<std::string, std::string>& notified, std::string_view currentChannelId) {
    if (!channels::hasUnread(followed, channel)) return {};
    WatchOutcome out{true, false};
    const auto last = notified.find(followed.id);
    bool isNew = channel.lastPostAt.has_value();
    if (isNew && last != notified.end()) {
        // Like Date.parse: a time that doesn't parse never counts as newer.
        const auto newest = util::parseIsoMillis(*channel.lastPostAt);
        const auto before = util::parseIsoMillis(last->second);
        isNew = newest && before && *newest > *before;
    }
    if (!isNew || followed.muted || currentChannelId == followed.id) return out;
    notified[followed.id] = *channel.lastPostAt;
    out.notify = true;
    return out;
}

std::string notifiedToJson(const std::map<std::string, std::string>& notified) {
    OrderedJson out = OrderedJson::object();
    for (const auto& [id, at] : notified) out[id] = at;
    return dump(out);
}

std::map<std::string, std::string> notifiedFromJson(std::string_view json) {
    std::map<std::string, std::string> out;
    const OrderedJson all = readObject(json);
    for (const auto& [id, at] : all.items()) {
        if (at.is_string()) out[id] = at.get<std::string>();
    }
    return out;
}

}  // namespace redvault::model::channel_posts
