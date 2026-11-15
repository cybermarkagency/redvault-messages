// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The three motions an overlay can have (`anim`: pulse, bounce, fade), as the web draws them with CSS
// (Stories.module.css: story-pulse 1.4 s, story-bounce 1 s, story-fade 2.2 s, each ease-in-out and repeating) and
// Android's studio/StoryMotion.kt plays them: the same key moments and the same easing curve, so they move alike on
// every app. A plain function of time, so it is tested without a screen.

namespace redvault::studio::motion {

/// How an animated word or emoji looks at one moment: its size, its lift (a fraction of its height; up is negative) and its opacity.
struct Frame {
    double scale = 1;
    double lift = 0;
    double alpha = 1;
    bool operator==(const Frame&) const = default;
};

/// CSS `ease-in-out` (cubic-bezier(0.42, 0, 0.58, 1)) at progress x (0 to 1).
double ease(double x);
/// 1400, 1000 or 2200 ms; 0 for no motion.
std::int64_t durationMs(std::string_view anim);
/// The frame `elapsedMs` after it started; "" (or anything unknown) is the resting frame.
Frame frame(std::string_view anim, std::int64_t elapsedMs);
/// The motion after `anim` when the button is pressed: still, pulse, bounce, fade, and round to still (the web's order).
std::string next(std::string_view anim);
/// "Still", "Pulse", "Bounce", "Fade".
std::string label(std::string_view anim);

}  // namespace redvault::studio::motion
