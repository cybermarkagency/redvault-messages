// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/model/stories.hpp"

// Subtitles for a video or voice story, typed by the author or read from an .srt file (port of the web app's
// ui/src/lib/studio/captions.ts, pinned to what it makes by tests/vectors/studio_vectors.json). Nothing is transcribed
// for you: speech recognition would mean sending the recording to a service.

namespace redvault::studio::captions {

/// "1:23.4" for 83,400 ms, rounded as JavaScript's toFixed rounds (from the exact value), so 59.95 s is "0:60.0" as on the web.
std::string formatClock(double ms);
/// Reads "83.4", "1:23.4", "0:01:23,400" or "00:01:23.4" as milliseconds; nullopt if it isn't a time (or is past 10 minutes).
std::optional<std::int64_t> parseClock(std::string_view text);
/// Reads SubRip (.srt) text. Cues that don't fit the limits are left out, as everywhere captions are read.
std::vector<model::stories::Caption> parseSrt(std::string_view text);
std::string toSrt(const std::vector<model::stories::Caption>& captions);
/// Spreads typed lines evenly over a recording: a quick way to caption speech you wrote down.
std::vector<model::stories::Caption> evenCaptions(const std::vector<std::string>& lines, std::int64_t durationMs);

}  // namespace redvault::studio::captions
