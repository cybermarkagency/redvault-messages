#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/api/channel_api.hpp"
#include "redvault/model/channels.hpp"

// Channel posts on this computer, as the web app does it
// (ui/src/lib/channelPosts.ts, lib/channelWatch.ts): what kind of media a
// file is, how long things are, what this computer remembers (your votes,
// where you stopped in a podcast, which new posts were already notified),
// and the notification logic for followed channels. The server keeps no
// follower list, so the app checks each followed channel itself (public
// reads, no cookie). Nothing remembered here leaves this computer; the app
// keeps it in its local settings as the JSON these functions read and write.

namespace redvault::model::channel_posts {

/// Pictures that go into a post as photos (the server's PHOTO_MIMES; not SVG, which can carry script).
bool isPhotoMime(std::string_view mime);

/// How a picked file goes into a post: pictures as photos (at full resolution), videos, audio (podcasts), anything else as a file.
std::string mediaKindFor(std::string_view mime);

/// How an upload or attachment is called: "Photo", "Video", "File", "Audio", "Voice clip", "Round video".
std::string kindLabel(std::string_view kind);

/// "3:05", or "1:02:03" for an hour or more.
std::string formatDuration(double seconds);

/// The playback speeds offered, in order (1×, 1.5×, 2×), and the one after `current`.
double nextSpeed(double current);

// --- What this computer remembers ---

/// Your vote, and for a quiz the right answer you were shown.
struct RememberedVote {
    std::vector<int> mine;
    std::optional<int> correct;
    std::optional<std::string> explanation;

    bool operator==(const RememberedVote&) const = default;
};

inline constexpr std::size_t kMaxRememberedVotes = 2000;
inline constexpr std::size_t kMaxRememberedPositions = 500;

std::optional<RememberedVote> rememberedVote(std::string_view json, std::string_view postId);
/**
 * The remembered votes with this one set (a quiz's right answer and
 * explanation taken from `poll`), or forgotten when `vote` is empty or has
 * no options. Keeps the most recent kMaxRememberedVotes. Returns the new JSON.
 */
std::string rememberVote(std::string_view json, std::string_view postId, const std::optional<RememberedVote>& vote,
                         const std::optional<api::channels::Poll>& poll = std::nullopt);

/// Where you stopped in a podcast or long audio, in seconds (0 when you'd finished or never started).
double savedPosition(std::string_view json, std::string_view mediaId);
/// Remembers the position (whole seconds), or forgets it at 0. Keeps the most recent kMaxRememberedPositions. Returns the new JSON.
std::string savePosition(std::string_view json, std::string_view mediaId, double seconds);

// --- Polls as shown ---

/// Each option's share of the voters in whole percent (Math.round), all 0 before anyone voted.
std::vector<int> percents(const api::channels::Poll& poll);

// --- Notifications for followed channels ---

/// How often the app checks followed channels (and also whenever its window comes back to the front).
inline constexpr int kCheckEveryMs = 2 * 60 * 1000;

/// A notification's text for a post: its text without the markers (140 characters at most), or what it carries.
std::string postPreview(const api::channels::Post* post);

struct WatchOutcome {
    /// It has posts you haven't seen: it counts on the Channels badge.
    bool unread = false;
    /// Notify of its newest post now (`notified` now remembers it).
    bool notify = false;

    bool operator==(const WatchOutcome&) const = default;
};

/**
 * One followed channel, as just fetched. Notifies once per newest post:
 * never for a muted channel or the one open right now (`currentChannelId`),
 * and those aren't remembered as notified, as on the web.
 */
WatchOutcome watch(const channels::FollowedChannel& followed, const api::channels::Channel& channel,
                   std::map<std::string, std::string>& notified, std::string_view currentChannelId);

/// channel id -> the newest post time already notified, as {"<id>":"<iso>"}.
std::string notifiedToJson(const std::map<std::string, std::string>& notified);
std::map<std::string, std::string> notifiedFromJson(std::string_view json);

}  // namespace redvault::model::channel_posts
