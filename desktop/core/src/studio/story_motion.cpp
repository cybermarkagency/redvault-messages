// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/studio/story_motion.hpp"

#include <vector>

namespace redvault::studio::motion {

namespace {

constexpr double kX1 = 0.42;
constexpr double kY1 = 0.0;
constexpr double kX2 = 0.58;
constexpr double kY2 = 1.0;

struct Stop {
    double at;
    double value;
};

const std::vector<Stop>& stops(std::string_view anim) {
    static const std::vector<Stop> pulse = {{0.0, 1.0}, {0.5, 1.12}, {1.0, 1.0}};
    static const std::vector<Stop> bounce = {{0.0, 0.0}, {0.4, -0.14}, {0.6, 0.0}, {1.0, 0.0}};
    static const std::vector<Stop> fadeStops = {{0.0, 1.0}, {0.5, 0.35}, {1.0, 1.0}};
    if (anim == "pulse") return pulse;
    if (anim == "bounce") return bounce;
    return fadeStops;
}

double bez(double t, double p1, double p2) {
    const double u = 1 - t;
    return 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t;
}

/// Eased between the neighbouring stops, as CSS does per keyframe.
double valueAt(const std::vector<Stop>& list, double progress) {
    for (std::size_t i = 0; i + 1 < list.size(); ++i) {
        const Stop& a = list[i];
        const Stop& b = list[i + 1];
        if (progress <= b.at) {
            const double span = b.at - a.at;
            if (span <= 0.0) return b.value;
            return a.value + (b.value - a.value) * ease((progress - a.at) / span);
        }
    }
    return list.back().value;
}

}  // namespace

double ease(double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    // The curve is monotonic in x: solve for t by bisection.
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = (lo + hi) / 2;
        if (bez(mid, kX1, kX2) < x) lo = mid;
        else hi = mid;
    }
    return bez((lo + hi) / 2, kY1, kY2);
}

std::int64_t durationMs(std::string_view anim) {
    if (anim == "pulse") return 1400;
    if (anim == "bounce") return 1000;
    if (anim == "fade") return 2200;
    return 0;
}

Frame frame(std::string_view anim, std::int64_t elapsedMs) {
    const std::int64_t length = durationMs(anim);
    if (length == 0) return Frame{};
    const double progress = static_cast<double>(((elapsedMs % length) + length) % length) / static_cast<double>(length);
    const double v = valueAt(stops(anim), progress);
    if (anim == "pulse") return Frame{v, 0, 1};
    if (anim == "bounce") return Frame{1, v, 1};
    return Frame{1, 0, v};
}

std::string next(std::string_view anim) {
    if (anim == "pulse") return "bounce";
    if (anim == "bounce") return "fade";
    if (anim == "fade") return "";
    return "pulse";
}

std::string label(std::string_view anim) {
    if (anim == "pulse") return "Pulse";
    if (anim == "bounce") return "Bounce";
    if (anim == "fade") return "Fade";
    return "Still";
}

}  // namespace redvault::studio::motion
