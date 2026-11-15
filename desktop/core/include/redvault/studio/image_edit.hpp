// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Photo editing, all on this computer: crop, rotate, flip, colour correction, filters and drawing. The maths that
// decides what the result looks like is here, in plain functions pinned to the web app's ui/src/lib/studio/imageEdit.ts
// (by way of Android's studio/ImageEdit.kt) by tests/vectors/studio_vectors.json; the app draws it with Qt
// (app/src/studio_tools.cpp). An edit is a plain value (EditState), so undo is keeping the old ones. Nothing is
// uploaded to edit a photo, and the result is a fresh JPEG that carries none of the original file's metadata.

namespace redvault::studio {

/// A rectangle as fractions (0 to 1) of the picture after it is turned.
struct Rect {
    double x = 0;
    double y = 0;
    double w = 1;
    double h = 1;
    bool operator==(const Rect&) const = default;
};

struct Adjust {
    /// -100 to 100; 0 changes nothing.
    double brightness = 0;
    double contrast = 0;
    double saturation = 0;
    /// 0 to 100 (up to about 1% of the picture's height of blur).
    double blur = 0;
    bool operator==(const Adjust&) const = default;
};

struct Point {
    double x = 0;
    double y = 0;
    bool operator==(const Point&) const = default;
};

/// A line drawn on the result; points and size are fractions of the finished picture's width and height.
struct Stroke {
    std::string color = "white";  // one of the story colours
    double size = 0.02;
    bool eraser = false;
    std::vector<Point> points;
    bool operator==(const Stroke&) const = default;
};

struct EditState {
    /// 0, 90, 180 or 270.
    int rotation = 0;
    bool flipX = false;
    /// None: the whole picture.
    std::optional<Rect> crop;
    Adjust adjust;
    /// One of kPhotoFilters.
    std::string filter = "none";
    std::vector<Stroke> strokes;
    bool operator==(const EditState&) const = default;
};

/// What a filter and the sliders do, as the terms the CSS filter functions use (1 = unchanged).
struct ColorParams {
    double brightness = 1;
    double contrast = 1;
    double saturate = 1;
    double grayscale = 0;
    double sepia = 0;
    double hueRotate = 0;
    double blurPx = 0;
    /// Whether anything about the colour changes at all.
    bool neutral() const;
};

/// Which part of the turned picture ends up in the result, in its pixels.
struct Pixels {
    int sx = 0;
    int sy = 0;
    int sw = 1;
    int sh = 1;
};

namespace image {

inline constexpr double kMinCrop = 0.08;
inline constexpr double kMinPen = 0.004;
inline constexpr double kMaxPen = 0.08;
inline constexpr std::size_t kMaxStrokes = 400;
inline constexpr std::size_t kMaxPointsPerStroke = 2000;

/// "none", "vivid", "warm", "cool", "fade", "mono", "noir", "sepia" (the web's order).
const std::vector<std::string>& filters();
/// "Original", "Vivid", "Warm", ...
std::string filterLabel(std::string_view filter);
/// The pen colours as real colours (#rrggbb), matching the story colour tokens.
std::string penColor(std::string_view color);

bool hasEdits(const EditState& s);
std::pair<int, int> rotatedSize(int w, int h, int rotation);
int rotateBy(int rotation, int quarterTurns);
/// A crop kept inside the picture and no smaller than kMinCrop either way.
Rect clampRect(const Rect& r);
/// The largest centred crop with this width-to-height ratio (none: the whole picture).
std::optional<Rect> aspectCrop(std::optional<double> aspect, int imageW, int imageH);
/// Drags a crop by a handle ("move", "nw", "ne", "sw", "se", "n", "s", "e", "w"); dx and dy are fractions of the picture.
Rect dragRect(const Rect& r, std::string_view handle, double dx, double dy);
Pixels cropPixels(int imageW, int imageH, const EditState& s);
/// The finished picture's size, scaled down so its long side is at most `maxSide`.
std::pair<int, int> outputSize(int imageW, int imageH, const EditState& s, int maxSide);
/// The filter and the sliders together. Sliders run -100..100 around 1, blur 0..100 up to 1% of the height.
ColorParams colorParams(const EditState& s, double outputHeight = 1000.0);
/// Colour changes on RGBA bytes (four per pixel), in place: the web's `adjustPixels` (with `hue`, also the hue turn of its CSS filter).
void adjustPixels(std::vector<std::uint8_t>& rgba, const ColorParams& c, bool hue = false);
/// The same on 0xAARRGGBB pixels (Qt's ARGB32 / RGB32).
void adjustArgb(std::uint32_t* pixels, std::size_t count, const ColorParams& c, bool hue = true);
/// A blur of about `radius` pixels: three box passes each way, close to a Gaussian (not the browser's own pixels).
void blurArgb(std::uint32_t* pixels, int width, int height, double radius);

Stroke newStroke(std::string_view color, double size, bool eraser, Point at);
/// Adds a point to a stroke, keeping it in bounds; points too close to the last one are skipped.
Stroke extendStroke(const Stroke& stroke, Point p);
EditState addStroke(const EditState& s, const Stroke& stroke);
/// Drawing is on the finished picture, so a change of crop, turn or flip clears it.
EditState withGeometry(const EditState& s, int rotation, bool flipX, const std::optional<Rect>& crop);

}  // namespace image

/// Undo and redo for the editors: a plain value at each step (port of the web's lib/studio/history.ts).
template <typename T>
class EditHistory {
public:
    static constexpr std::size_t kMaxUndo = 60;

    explicit EditHistory(T start = T()) : present_(std::move(start)) {}

    const T& present() const { return present_; }
    bool canUndo() const { return !past_.empty(); }
    bool canRedo() const { return !future_.empty(); }
    std::size_t pastSize() const { return past_.size(); }

    /// A new step. Anything that could have been redone is gone; the oldest steps drop off past `limit`.
    void push(T value, std::size_t limit = kMaxUndo) {
        if (value == present_) return;
        past_.push_back(std::move(present_));
        while (past_.size() > limit) past_.erase(past_.begin());
        present_ = std::move(value);
        future_.clear();
    }
    /// The present replaced without a step of its own (a slider being dragged is one step, not fifty).
    void replace(T value) { present_ = std::move(value); }
    /// After replace()s: one step back to `before` (what the present was when the drag began), unless nothing changed.
    void commitFrom(T before, std::size_t limit = kMaxUndo) {
        if (before == present_) return;
        past_.push_back(std::move(before));
        while (past_.size() > limit) past_.erase(past_.begin());
        future_.clear();
    }
    void undo() {
        if (!canUndo()) return;
        future_.insert(future_.begin(), std::move(present_));
        present_ = std::move(past_.back());
        past_.pop_back();
    }
    void redo() {
        if (!canRedo()) return;
        past_.push_back(std::move(present_));
        present_ = std::move(future_.front());
        future_.erase(future_.begin());
    }

private:
    std::vector<T> past_;
    T present_;
    std::vector<T> future_;
};

}  // namespace redvault::studio
