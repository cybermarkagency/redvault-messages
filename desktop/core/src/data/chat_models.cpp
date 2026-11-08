// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/data/chat_models.hpp"

#include <algorithm>

namespace redvault::data {

const std::array<Disappearing, 5>& Disappearing::all() {
    static const std::array<Disappearing, 5> kAll = {{
        {std::nullopt, "Off"},
        {30, "30 seconds"},
        {60, "1 minute"},
        {3600, "1 hour"},
        {1, "Instantly"},
    }};
    return kAll;
}

Disappearing Disappearing::fromSeconds(std::optional<int> seconds) {
    const auto& all = Disappearing::all();
    const auto it = std::find_if(all.begin(), all.end(), [&](const Disappearing& d) { return d.seconds == seconds; });
    return it != all.end() ? *it : all[0];
}

std::vector<std::string> ConversationMeta::otherUserIds(const std::string& selfUserId) const {
    if (!isGroup) return {otherUserId};
    std::vector<std::string> out;
    for (const auto& member : members) {
        if (!member.isSelf && member.userId != selfUserId) out.push_back(member.userId);
    }
    return out;
}

std::string ConversationMeta::handleOf(const std::string& userId) const {
    for (const auto& member : members) {
        if (member.userId == userId) return member.handle;
    }
    return "Former member";
}

}  // namespace redvault::data
