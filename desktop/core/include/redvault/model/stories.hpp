// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

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
/// How a text or emoji overlay moves (`anim`); "none" is never written. Apps from before animated text show it still.
inline const std::vector<std::string> kAnimations = {"pulse", "bounce", "fade"};

inline constexpr std::size_t kMaxOverlays = 30;
inline constexpr std::size_t kMaxOverlayText = 300;  // UTF-16 units
inline constexpr std::size_t kMaxStoryText = 700;    // UTF-16 units
/// How long a photo or text story stays up in the viewer.
inline constexpr int kStorySeconds = 5;
inline constexpr std::int64_t kMaxVideoMs = 60'000;
inline constexpr std::uint64_t kMaxVideoBytes = 60ull * 1024 * 1024;
inline constexpr int kMaxImageSide = 1920;
/// A voice story (kind "audio"): at most 5 minutes and 24 MiB, as on the web and Android.
inline constexpr std::int64_t kMaxAudioMs = 5 * 60'000;
inline constexpr std::uint64_t kMaxAudioBytes = 24ull * 1024 * 1024;
/// Subtitles of a video or voice story.
inline constexpr std::size_t kMaxCaptions = 60;
inline constexpr std::size_t kMaxCaptionText = 120;  // UTF-16 units
inline constexpr std::int64_t kMaxCaptionEndMs = 10 * 60'000;
/// The loudness bars of a voice story: 16 to 96 of them, 0 to 100 each.
inline constexpr std::size_t kWaveformMin = 16;
inline constexpr std::size_t kWaveformMax = 96;

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
    /// "" (still), "pulse", "bounce" or "fade".
    std::string anim;
};

/// One line of subtitles shown over a video or voice story between two times.
struct Caption {
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::string text;
    bool operator==(const Caption&) const = default;
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
    /// Video and audio only.
    std::optional<std::int64_t> durationMs;
};

struct Meta {
    std::string kind;  // "image" | "video" | "text" | "audio"
    /// A text story's words; a voice story's optional title.
    std::string text;
    std::string background = "sunset";
    std::string font = "classic";
    std::vector<Overlay> overlays;
    std::optional<MediaRef> media;
    /// Interactive and informational stickers (docs/stories-advanced/). Optional: older apps ignore them.
    std::vector<Sticker> stickers;
    /// Video and audio: subtitles, in time order (lines that don't fit are dropped).
    std::vector<Caption> captions;
    /// Audio: the loudness bars, drawn before and while it plays (absent when the story has none that fit).
    std::optional<std::vector<int>> waveform;
    bool closeFriends = false;
};

/// The kinds this app shows. Any other kind is skipped and the author's other stories still show.
bool isKnownKind(std::string_view kind);

/// Throws StoryFormatError unless it's a well-formed story of `kind` ("image", "video", "text" or "audio").
Meta parseMeta(std::string_view json, std::string_view kind);
/// The JSON the web app writes (same fields).
std::string toJson(const Meta& meta);

/// The overlays of a saved draft: lenient, an overlay that doesn't fit is dropped (a story being read is stricter: parseMeta refuses it).
std::vector<Overlay> parseOverlays(std::string_view jsonArray);
std::string toJson(const std::vector<Overlay>& overlays);

/// Subtitles as every app reads them (`jsonArray` is their JSON): lines without a real span or readable text are dropped, at most 60, in time order.
std::vector<Caption> parseCaptions(std::string_view jsonArray);
std::string toJson(const std::vector<Caption>& captions);
/// The line to show at `timeMs`, if any.
std::optional<std::string> captionAt(const std::vector<Caption>& captions, std::int64_t timeMs);
/// 16 to 96 whole numbers from 0 to 100, or nothing at all (`json` is the waveform's JSON).
std::optional<std::vector<int>> parseWaveform(std::string_view json);

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

/// What a reply quotes: "Photo", "Video", or the start of a text story (a voice story: its title, or "Voice").
std::string preview(const Meta& meta);
/// The kind a reply names: a voice story is quoted as text, as the web and Android do.
std::string replyKind(const Meta& meta);

/// The ids this device remembers having seen (with when), as stored in local settings; entries over 2 days old are dropped.
struct SeenEntry {
    std::string storyId;
    std::int64_t atMs = 0;
};
std::vector<SeenEntry> parseSeen(std::string_view json, std::int64_t nowMs);
std::string toJson(const std::vector<SeenEntry>& seen);

}  // namespace redvault::model::stories
