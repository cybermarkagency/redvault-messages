#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/model/story_stickers.hpp"

// What a story shows and the story settings, as the web app's
// ui/src/lib/storyCrypto.ts parses them: the same JSON, the same limits and
// the same fallbacks, so a story looks the same in every app. Everything
// here comes out of a decrypted blob someone else wrote: untrusted input.

namespace redvault::model::stories {

/// Named colours only (the apps draw each one the same way).
inline const std::vector<std::string> kColors = {"white", "black", "red", "orange", "yellow", "green", "teal", "blue", "purple", "pink"};
inline const std::vector<std::string> kBackgrounds = {"sunset", "ocean", "forest", "berry", "night", "ember", "mint", "slate"};
inline const std::vector<std::string> kFonts = {"classic", "modern", "typewriter", "strong"};

inline constexpr std::size_t kMaxOverlays = 30;
inline constexpr std::size_t kMaxOverlayText = 300;  // UTF-16 units
inline constexpr std::size_t kMaxStoryText = 700;    // UTF-16 units
/// How long a photo or text story stays up in the viewer.
inline constexpr int kStorySeconds = 5;
inline constexpr std::int64_t kMaxVideoMs = 60'000;
inline constexpr std::uint64_t kMaxVideoBytes = 60ull * 1024 * 1024;
inline constexpr int kMaxImageSide = 1920;

/// Text or an emoji sticker; x and y are the centre, as fractions of the frame.
struct Overlay {
    std::string type;  // "text" | "emoji"
    std::string text;
    double x = 0.5;
    double y = 0.5;
    double scale = 1;     // 0.5 - 4
    double rotation = 0;  // degrees, 0 - 360
    std::string color = "white";
    bool boxed = false;
    std::string font = "classic";
};

struct MediaRef {
    /// base64: the file key and secretstream header (crypto/file_crypto.hpp).
    std::string key;
    std::string header;
    std::string sha256;
    std::uint64_t encryptedSize = 0;
    std::uint64_t chunkSize = 0;
    std::string mime;
    int width = 0;
    int height = 0;
    /// Video only.
    std::optional<std::int64_t> durationMs;
};

struct Meta {
    std::string kind;  // "image" | "video" | "text"
    std::string text;
    std::string background = "sunset";
    std::string font = "classic";
    std::vector<Overlay> overlays;
    std::optional<MediaRef> media;
    /// Interactive and informational stickers (docs/stories-advanced/). Optional: older apps ignore them.
    std::vector<Sticker> stickers;
    bool closeFriends = false;
};

/// Throws StoryFormatError unless it's a well-formed story of `kind`.
Meta parseMeta(std::string_view json, std::string_view kind);
/// The JSON the web app writes (same fields).
std::string toJson(const Meta& meta);

/// The overlays of a saved draft: lenient, an overlay that doesn't fit is dropped (a story being read is stricter: parseMeta refuses it).
std::vector<Overlay> parseOverlays(std::string_view jsonArray);
std::string toJson(const std::vector<Overlay>& overlays);

/// Story settings, synced between your devices, unreadable by the server.
struct Prefs {
    std::vector<std::string> closeFriends;
    std::vector<std::string> hideFrom;
    std::vector<std::string> muted;
    bool shareViews = true;
    bool allowReplies = true;
};

/// Lenient, like the web: bad entries are dropped, ids lowercased, missing switches on.
Prefs parsePrefs(std::string_view json);
std::string toJson(const Prefs& prefs);

/// Who gets a story: your friends minus anyone hidden (and only close friends, for a close-friends story).
std::vector<std::string> recipients(const std::vector<std::string>& friendIds, const Prefs& prefs, bool closeFriendsOnly);

/// The same for the three audiences a story can have: "friends", "close_friends", or "custom" (only the friends in `customIds`).
/// Anyone hidden is left out of all three, and nobody who isn't in `friendIds` is ever added, whatever `customIds` says.
std::vector<std::string> recipients(const std::vector<std::string>& friendIds, const Prefs& prefs, std::string_view audience,
                                    const std::vector<std::string>& customIds);

/// How long a story may stay up: 1, 6, 12, 24, 48 hours or 7 days.
inline constexpr int kDefaultStorySeconds = 24 * 3600;
/// "1 hour", "6 hours", "24 hours", "48 hours", "7 days".
std::string lifetimeLabel(int seconds);

/// "now", "12m", "3h", "2d", "1w": how long ago a story went up.
std::string age(std::int64_t createdAtMs, std::int64_t nowMs);

/// What a reply quotes: "Photo", "Video", or the start of a text story.
std::string preview(const Meta& meta);

/// The ids this device remembers having seen (with when), as stored in local settings; entries over 2 days old are dropped.
struct SeenEntry {
    std::string storyId;
    std::int64_t atMs = 0;
};
std::vector<SeenEntry> parseSeen(std::string_view json, std::int64_t nowMs);
std::string toJson(const std::vector<SeenEntry>& seen);

}  // namespace redvault::model::stories
