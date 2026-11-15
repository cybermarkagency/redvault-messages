// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/studio/collage.hpp"

#include <algorithm>

namespace redvault::studio::collage {

namespace {

std::vector<Box> grid(int cols, int rows) {
    std::vector<Box> out;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            out.push_back(Box{static_cast<double>(c) / cols, static_cast<double>(r) / rows, 1.0 / cols, 1.0 / rows});
        }
    }
    return out;
}

}  // namespace

const std::vector<Layout>& layouts() {
    static const std::vector<Layout> list = {
        {"stack2", "Two, stacked", grid(1, 2)},
        {"side2", "Two, side by side", grid(2, 1)},
        {"stack3", "Three, stacked", grid(1, 3)},
        {"big1small2", "One big, two small", {{0.0, 0.0, 1.0, 0.6}, {0.0, 0.6, 0.5, 0.4}, {0.5, 0.6, 0.5, 0.4}}},
        {"left1right2", "One tall, two short", {{0.0, 0.0, 0.5, 1.0}, {0.5, 0.0, 0.5, 0.5}, {0.5, 0.5, 0.5, 0.5}}},
        {"grid4", "Four", grid(2, 2)},
    };
    return list;
}

std::vector<Layout> layoutsFor(int count) {
    std::vector<Layout> out;
    for (const auto& l : layouts()) {
        if (static_cast<int>(l.cells.size()) == count) out.push_back(l);
    }
    return out;
}

const Layout* layoutById(std::string_view id) {
    for (const auto& l : layouts()) {
        if (l.id == id) return &l;
    }
    return nullptr;
}

const std::vector<Background>& backgrounds() {
    static const std::vector<Background> list = {
        {"Dark", 0xFF111111u}, {"Light", 0xFFF5F5F4u}, {"Warm", 0xFF7C2D12u}, {"Cool", 0xFF1E3A8Au}};
    return list;
}

std::vector<Box> cellRects(const Layout& layout, double width, double height, double gap) {
    const double half = gap / 2;
    std::vector<Box> out;
    for (const auto& c : layout.cells) {
        out.push_back(Box{c.x * width + half, c.y * height + half, std::max(1.0, c.w * width - gap), std::max(1.0, c.h * height - gap)});
    }
    return out;
}

CoverPart fitCover(double imageW, double imageH, double cellW, double cellH) {
    const double cellAspect = cellW / cellH;
    const double imageAspect = imageW / imageH;
    if (imageAspect > cellAspect) {
        const double sw = imageH * cellAspect;
        return CoverPart{(imageW - sw) / 2, 0.0, sw, imageH};
    }
    const double sh = imageW / cellAspect;
    return CoverPart{0.0, (imageH - sh) / 2, imageW, sh};
}

}  // namespace redvault::studio::collage
