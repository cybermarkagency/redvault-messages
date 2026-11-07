#include "redvault/model/channels.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "redvault/crypto/channel_crypto.hpp"
#include "redvault/model/group_call_link.hpp"
#include "redvault/util/text.hpp"

namespace redvault::model::channels {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kParam = "channel=";

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

}  // namespace

bool isFollowing(const Followed& list, std::string_view id) {
    return std::any_of(list.begin(), list.end(), [&](const FollowedChannel& f) { return f.id == id; });
}

Followed follow(const Followed& list, const api::channels::Channel& channel, const std::optional<std::string>& secret) {
    if (isFollowing(list, channel.id)) return list;
    Followed out = list;
    out.push_back(FollowedChannel{channel.id, channel.name, channel.lastPostAt, secret && !secret->empty() ? secret : std::nullopt});
    return out;
}

Followed unfollow(const Followed& list, std::string_view id) {
    Followed out;
    for (const auto& f : list) {
        if (f.id != id) out.push_back(f);
    }
    return out;
}

Followed markSeen(const Followed& list, const api::channels::Channel& channel, const std::optional<std::string>& secret) {
    Followed out = list;
    for (auto& f : out) {
        if (f.id != channel.id) continue;
        f.name = channel.name;
        f.seenAt = channel.lastPostAt;
        if (secret && !secret->empty()) f.secret = secret;
    }
    return out;
}

Followed setMuted(const Followed& list, std::string_view id, bool muted) {
    Followed out = list;
    for (auto& f : out) {
        if (f.id == id) f.muted = muted;
    }
    return out;
}

bool hasUnread(const FollowedChannel& followed, const api::channels::Channel& channel) {
    if (!channel.lastPostAt) return false;
    if (!followed.seenAt) return true;
    // Like Date.parse on the web: a time that doesn't parse never counts as newer.
    const auto last = util::parseIsoMillis(*channel.lastPostAt);
    const auto seen = util::parseIsoMillis(*followed.seenAt);
    return last && seen && *last > *seen;
}

std::string toJson(const Followed& list) {
    Json out = Json::array();
    for (const auto& f : list) {
        Json entry{{"id", f.id}, {"name", f.name}, {"seenAt", f.seenAt ? Json(*f.seenAt) : Json(nullptr)}};
        if (f.secret) entry["secret"] = *f.secret;
        if (f.muted) entry["muted"] = true;
        out.push_back(std::move(entry));
    }
    return out.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Followed fromJson(std::string_view text) {
    Followed out;
    if (text.empty()) return out;
    const Json parsed = Json::parse(text, nullptr, false);
    if (!parsed.is_array()) return out;
    for (const auto& j : parsed) {
        if (!j.is_object()) continue;
        const auto id = j.find("id");
        if (id == j.end() || !id->is_string() || id->get<std::string>().empty() || isFollowing(out, id->get<std::string>())) continue;
        FollowedChannel f;
        f.id = id->get<std::string>();
        if (const auto name = j.find("name"); name != j.end() && name->is_string()) f.name = name->get<std::string>();
        if (const auto seen = j.find("seenAt"); seen != j.end() && seen->is_string()) f.seenAt = seen->get<std::string>();
        if (const auto secret = j.find("secret"); secret != j.end() && secret->is_string() && !secret->get<std::string>().empty()) {
            f.secret = secret->get<std::string>();
        }
        if (const auto muted = j.find("muted"); muted != j.end() && muted->is_boolean()) f.muted = muted->get<bool>();
        out.push_back(std::move(f));
    }
    return out;
}

std::string channelUrl(std::string_view webAppBase, std::string_view id, const std::optional<std::string>& secret) {
    std::string base(webAppBase);
    if (base.empty() || base.back() != '/') base += '/';
    std::string url = base + "?" + std::string(kParam) + std::string(id);
    if (secret && !secret->empty()) url += "#" + *secret;
    return url;
}

std::optional<ChannelLink> parseChannelLink(std::string_view text) {
    for (std::size_t at = text.find(kParam); at != std::string_view::npos; at = text.find(kParam, at + 1)) {
        if (at == 0 || (text[at - 1] != '?' && text[at - 1] != '&')) continue;
        const auto start = at + kParam.size();
        const auto id = text.substr(start, 36);
        if (!group_calls::isId(id)) continue;
        if (start + 36 < text.size()) {
            const char next = text[start + 36];
            if (std::isalnum(static_cast<unsigned char>(next)) != 0 || next == '-' || next == '_') continue;
        }
        ChannelLink out;
        out.id = std::string(id);
        std::transform(out.id.begin(), out.id.end(), out.id.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // The rest of this link (up to whitespace): its fragment, like location.hash, is the secret if it's one.
        std::size_t end = start + 36;
        while (end < text.size() && !isSpace(text[end])) ++end;
        const std::string_view rest = text.substr(start + 36, end - start - 36);
        if (const auto hash = rest.find('#'); hash != std::string_view::npos) {
            out.secret = crypto::channels::secretFromFragment(rest.substr(hash + 1));
        }
        return out;
    }
    return std::nullopt;
}

std::vector<std::string> uncountedViews(std::vector<std::string>& remembered, const std::vector<std::string>& postIds) {
    const std::unordered_set<std::string> known(remembered.begin(), remembered.end());
    std::unordered_set<std::string> taken;
    std::vector<std::string> fresh;
    for (const auto& id : postIds) {
        const std::string lower = util::toLowerAscii(id);
        if (known.contains(lower) || !taken.insert(lower).second) continue;
        fresh.push_back(lower);
    }
    remembered.insert(remembered.end(), fresh.begin(), fresh.end());
    if (remembered.size() > kMaxRememberedViews) {
        remembered.erase(remembered.begin(), remembered.end() - static_cast<std::ptrdiff_t>(kMaxRememberedViews));
    }
    return fresh;
}

}  // namespace redvault::model::channels
