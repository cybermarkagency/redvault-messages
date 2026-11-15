// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Collages: two to four photos in one picture. The layouts and the maths (cell sizes, how a photo fills its cell) are
// the web app's (ui/src/lib/studio/collage.ts, by way of Android's studio/StoryCollage.kt), pinned to its vectors; the
// app draws the result with Qt as a normal story photo, a fresh JPEG.

namespace redvault::studio::collage {

inline constexpr int kMaxPhotos = 4;
inline constexpr int kWidth = 1080;
inline constexpr int kHeight = 1920;
inline constexpr double kDefaultGap = 12.0;
inline constexpr int kMaxGap = 40;

/// A cell as fractions (0 to 1) of the whole picture; also a cell in pixels.
struct Box {
    double x = 0;
    double y = 0;
    double w = 1;
    double h = 1;
};

struct Layout {
    std::string id;
    std::string label;
    std::vector<Box> cells;
};

/// The part of a photo drawn so it fills a cell without stretching.
struct CoverPart {
    double sx = 0;
    double sy = 0;
    double sw = 0;
    double sh = 0;
};

struct Background {
    std::string label;
    std::uint32_t argb = 0;
};

/// The six layouts, in the web's order.
const std::vector<Layout>& layouts();
/// The layouts that take exactly `count` photos.
std::vector<Layout> layoutsFor(int count);
const Layout* layoutById(std::string_view id);
/// Dark, Light, Warm, Cool.
const std::vector<Background>& backgrounds();
/// Cells in pixels, each pulled in by half the gap on every side so neighbours are `gap` apart (and the edges are too).
std::vector<Box> cellRects(const Layout& layout, double width, double height, double gap);
/// The middle of the picture, cropped to the cell's shape.
CoverPart fitCover(double imageW, double imageH, double cellW, double cellH);

}  // namespace redvault::studio::collage
