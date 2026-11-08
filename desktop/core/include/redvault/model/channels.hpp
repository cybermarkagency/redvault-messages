// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/api/channel_api.hpp"

// Channels on this computer, as the web app does it (ui/src/lib/channels.ts,
// lib/localPrefs.ts followedChannels, lib/channelViews.ts): following them
// (the server never learns who follows what, so the list, and which posts
// were already seen, live only here), private channels' links, and which
// posts this device already counted a view for. The app keeps the follows
// in its per-account local settings as the JSON toJson() writes.

namespace redvault::model::channels {

struct FollowedChannel {
    std::string id;
    /// As last seen, so the list reads well even offline or after the channel is gone.
    std::string name;
    /// The newest post already seen (a post's createdAt), for the unread dot.
    std::optional<std::string> seenAt;
    /// A private channel's link secret (base64url), which is what lets this computer read it.
    std::optional<std::string> secret;
    /// No notifications for its new posts (still counted on the Channels badge).
    bool muted = false;

    bool operator==(const FollowedChannel&) const = default;
};

using Followed = std::vector<FollowedChannel>;

bool isFollowing(const Followed& list, std::string_view id);

/// Following starts with everything already posted counted as seen. A private channel's secret comes along.
Followed follow(const Followed& list, const api::channels::Channel& channel, const std::optional<std::string>& secret = std::nullopt);

Followed unfollow(const Followed& list, std::string_view id);

/// Everything in the channel up to its newest post has been seen; also keeps its name (and a newer secret) current.
Followed markSeen(const Followed& list, const api::channels::Channel& channel, const std::optional<std::string>& secret = std::nullopt);

/// Notifications for its new posts on or off (the web app's `muted`).
Followed setMuted(const Followed& list, std::string_view id, bool muted);

/// The channel has a post newer than the newest one seen.
bool hasUnread(const FollowedChannel& followed, const api::channels::Channel& channel);

/// [{"id","name","seenAt"[,"secret"][,"muted"]}], the web app's shape; seenAt is null until there's a post.
std::string toJson(const Followed& list);
/// Anything unreadable gives an empty list rather than an error (it's only a preference).
Followed fromJson(std::string_view text);

/// A channel's shareable link: <web app base>?channel=<id>, plus #<secret> for a private one.
std::string channelUrl(std::string_view webAppBase, std::string_view id, const std::optional<std::string>& secret = std::nullopt);

struct ChannelLink {
    /// Lowercase.
    std::string id;
    /// A private channel's secret, when the link's fragment is one.
    std::optional<std::string> secret;

    bool operator==(const ChannelLink&) const = default;
};

/// The channel a link points to (…?channel=<id>, with #<secret> for a private one), or nullopt for anything else.
std::optional<ChannelLink> parseChannelLink(std::string_view text);

// --- View counts: each device counts a post once (ui/src/lib/channelViews.ts) ---

/// Remember at most this many posts; the oldest are forgotten first.
inline constexpr std::size_t kMaxRememberedViews = 5000;

/**
 * Which of `postIds` this device hasn't counted yet (lowercased, each once,
 * in order), adding them to `remembered` (which keeps the newest
 * kMaxRememberedViews). The caller reports them in batches of
 * api::channels::kMaxViewsPerReport.
 */
std::vector<std::string> uncountedViews(std::vector<std::string>& remembered, const std::vector<std::string>& postIds);

}  // namespace redvault::model::channels
