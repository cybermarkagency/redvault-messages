// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/studio/image_edit.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace redvault::studio {

namespace {

double clamp(double v, double lo, double hi) {
    return std::min(hi, std::max(lo, v));
}

std::int64_t jsRound(double v) {
    return static_cast<std::int64_t>(std::floor(v + 0.5));
}

bool near(double v, double to) {
    return std::abs(v - to) < 1e-3;
}

struct Preset {
    double brightness = 1;
    double contrast = 1;
    double saturate = 1;
    double grayscale = 0;
    double sepia = 0;
    double hueRotate = 0;
};

Preset preset(std::string_view f) {
    Preset p;
    if (f == "vivid") {
        p.saturate = 1.45;
        p.contrast = 1.1;
    } else if (f == "warm") {
        p.sepia = 0.25;
        p.saturate = 1.2;
        p.hueRotate = -8.0;
        p.brightness = 1.03;
    } else if (f == "cool") {
        p.hueRotate = 12.0;
        p.saturate = 0.95;
        p.brightness = 1.02;
        p.contrast = 1.04;
    } else if (f == "fade") {
        p.contrast = 0.85;
        p.brightness = 1.08;
        p.saturate = 0.8;
    } else if (f == "mono") {
        p.grayscale = 1.0;
    } else if (f == "noir") {
        p.grayscale = 1.0;
        p.contrast = 1.3;
        p.brightness = 0.92;
    } else if (f == "sepia") {
        p.sepia = 0.9;
        p.contrast = 1.05;
    }
    return p;
}

/// Rounds and holds to 0..255 the way a Uint8ClampedArray does (halves go to the even number).
int toByte(double v) {
    if (std::isnan(v) || v <= 0) return 0;
    if (v >= 255) return 255;
    return static_cast<int>(std::nearbyint(v));
}

void adjustOne(double r, double g, double b, const ColorParams& c, bool hue, double out[3]) {
    if (c.saturate != 1.0) {
        const double l = 0.299 * r + 0.587 * g + 0.114 * b;
        r = l + (r - l) * c.saturate;
        g = l + (g - l) * c.saturate;
        b = l + (b - l) * c.saturate;
    }
    if (c.grayscale > 0) {
        const double l = 0.299 * r + 0.587 * g + 0.114 * b;
        r += (l - r) * c.grayscale;
        g += (l - g) * c.grayscale;
        b += (l - b) * c.grayscale;
    }
    if (c.sepia > 0) {
        const double tr = 0.393 * r + 0.769 * g + 0.189 * b;
        const double tg = 0.349 * r + 0.686 * g + 0.168 * b;
        const double tb = 0.272 * r + 0.534 * g + 0.131 * b;
        r += (tr - r) * c.sepia;
        g += (tg - g) * c.sepia;
        b += (tb - b) * c.sepia;
    }
    if (hue && c.hueRotate != 0.0) {
        const double a = c.hueRotate * std::numbers::pi / 180;
        const double cs = std::cos(a);
        const double sn = std::sin(a);
        const double nr = (0.213 + cs * 0.787 - sn * 0.213) * r + (0.715 - cs * 0.715 - sn * 0.715) * g + (0.072 - cs * 0.072 + sn * 0.928) * b;
        const double ng = (0.213 - cs * 0.213 + sn * 0.143) * r + (0.715 + cs * 0.285 + sn * 0.140) * g + (0.072 - cs * 0.072 - sn * 0.283) * b;
        const double nb = (0.213 - cs * 0.213 - sn * 0.787) * r + (0.715 - cs * 0.715 + sn * 0.715) * g + (0.072 + cs * 0.928 + sn * 0.072) * b;
        r = nr;
        g = ng;
        b = nb;
    }
    if (c.contrast != 1.0) {
        r = (r - 128) * c.contrast + 128;
        g = (g - 128) * c.contrast + 128;
        b = (b - 128) * c.contrast + 128;
    }
    if (c.brightness != 1.0) {
        r *= c.brightness;
        g *= c.brightness;
        b *= c.brightness;
    }
    out[0] = r;
    out[1] = g;
    out[2] = b;
}

void boxPass(const std::vector<std::uint32_t>& src, std::vector<std::uint32_t>& dst, int width, int height, int radius, bool horizontal) {
    const int lines = horizontal ? height : width;
    const int length = horizontal ? width : height;
    for (int line = 0; line < lines; ++line) {
        const auto at = [&](int i) {
            return horizontal ? src[static_cast<std::size_t>(line) * width + i] : src[static_cast<std::size_t>(i) * width + line];
        };
        std::int64_t sa = 0, sr = 0, sg = 0, sb = 0;
        // The window [i - radius, i + radius], with the edge pixel repeated beyond the picture.
        for (int k = -radius; k <= radius; ++k) {
            const std::uint32_t p = at(std::clamp(k, 0, length - 1));
            sa += (p >> 24) & 0xff;
            sr += (p >> 16) & 0xff;
            sg += (p >> 8) & 0xff;
            sb += p & 0xff;
        }
        const std::int64_t count = 2 * radius + 1;
        for (int i = 0; i < length; ++i) {
            const std::uint32_t value = (static_cast<std::uint32_t>(sa / count) << 24) | (static_cast<std::uint32_t>(sr / count) << 16) |
                                        (static_cast<std::uint32_t>(sg / count) << 8) | static_cast<std::uint32_t>(sb / count);
            if (horizontal) dst[static_cast<std::size_t>(line) * width + i] = value;
            else dst[static_cast<std::size_t>(i) * width + line] = value;
            const std::uint32_t out = at(std::clamp(i - radius, 0, length - 1));
            const std::uint32_t in = at(std::clamp(i + radius + 1, 0, length - 1));
            sa += static_cast<std::int64_t>((in >> 24) & 0xff) - static_cast<std::int64_t>((out >> 24) & 0xff);
            sr += static_cast<std::int64_t>((in >> 16) & 0xff) - static_cast<std::int64_t>((out >> 16) & 0xff);
            sg += static_cast<std::int64_t>((in >> 8) & 0xff) - static_cast<std::int64_t>((out >> 8) & 0xff);
            sb += static_cast<std::int64_t>(in & 0xff) - static_cast<std::int64_t>(out & 0xff);
        }
    }
}

}  // namespace

bool ColorParams::neutral() const {
    return near(brightness, 1.0) && near(contrast, 1.0) && near(saturate, 1.0) && grayscale <= 0 && sepia <= 0 && hueRotate == 0.0 && blurPx <= 0.05;
}

namespace image {

const std::vector<std::string>& filters() {
    static const std::vector<std::string> list = {"none", "vivid", "warm", "cool", "fade", "mono", "noir", "sepia"};
    return list;
}

std::string filterLabel(std::string_view f) {
    if (f == "vivid") return "Vivid";
    if (f == "warm") return "Warm";
    if (f == "cool") return "Cool";
    if (f == "fade") return "Fade";
    if (f == "mono") return "Mono";
    if (f == "noir") return "Noir";
    if (f == "sepia") return "Sepia";
    return "Original";
}

std::string penColor(std::string_view c) {
    if (c == "black") return "#111111";
    if (c == "red") return "#ef4444";
    if (c == "orange") return "#f97316";
    if (c == "yellow") return "#facc15";
    if (c == "green") return "#22c55e";
    if (c == "teal") return "#14b8a6";
    if (c == "blue") return "#3b82f6";
    if (c == "purple") return "#a855f7";
    if (c == "pink") return "#ec4899";
    return "#ffffff";
}

bool hasEdits(const EditState& s) {
    const auto& a = s.adjust;
    return s.rotation != 0 || s.flipX || s.crop.has_value() || s.filter != "none" || a.brightness != 0 || a.contrast != 0 || a.saturation != 0 ||
           a.blur != 0 || !s.strokes.empty();
}

std::pair<int, int> rotatedSize(int w, int h, int rotation) {
    return rotation == 90 || rotation == 270 ? std::pair{h, w} : std::pair{w, h};
}

int rotateBy(int rotation, int quarterTurns) {
    const int quarters = (((rotation / 90 + quarterTurns) % 4) + 4) % 4;
    return quarters * 90;
}

Rect clampRect(const Rect& r) {
    const double w = clamp(r.w, kMinCrop, 1.0);
    const double h = clamp(r.h, kMinCrop, 1.0);
    return Rect{clamp(r.x, 0.0, 1 - w), clamp(r.y, 0.0, 1 - h), w, h};
}

std::optional<Rect> aspectCrop(std::optional<double> aspect, int imageW, int imageH) {
    if (!aspect || !(*aspect > 0) || imageW <= 0 || imageH <= 0) return std::nullopt;
    const double current = static_cast<double>(imageW) / imageH;
    if (std::abs(current - *aspect) < 1e-3) return std::nullopt;
    if (current > *aspect) {
        const double w = *aspect / current;
        return Rect{(1 - w) / 2, 0.0, w, 1.0};
    }
    const double h = current / *aspect;
    return Rect{0.0, (1 - h) / 2, 1.0, h};
}

Rect dragRect(const Rect& r, std::string_view handle, double dx, double dy) {
    double x = r.x, y = r.y, w = r.w, h = r.h;
    if (handle == "move") return clampRect(Rect{clamp(x + dx, 0.0, 1 - w), clamp(y + dy, 0.0, 1 - h), w, h});
    if (handle.find('w') != std::string_view::npos) {
        const double nx = clamp(x + dx, 0.0, x + w - kMinCrop);
        w += x - nx;
        x = nx;
    }
    if (handle.find('e') != std::string_view::npos) w = clamp(w + dx, kMinCrop, 1 - x);
    if (handle.find('n') != std::string_view::npos) {
        const double ny = clamp(y + dy, 0.0, y + h - kMinCrop);
        h += y - ny;
        y = ny;
    }
    if (handle.find('s') != std::string_view::npos) h = clamp(h + dy, kMinCrop, 1 - y);
    return clampRect(Rect{x, y, w, h});
}

Pixels cropPixels(int imageW, int imageH, const EditState& s) {
    const auto [rw, rh] = rotatedSize(imageW, imageH, s.rotation);
    const Rect c = s.crop.value_or(Rect{0, 0, 1, 1});
    return Pixels{static_cast<int>(jsRound(c.x * rw)), static_cast<int>(jsRound(c.y * rh)), static_cast<int>(std::max<std::int64_t>(1, jsRound(c.w * rw))),
                  static_cast<int>(std::max<std::int64_t>(1, jsRound(c.h * rh)))};
}

std::pair<int, int> outputSize(int imageW, int imageH, const EditState& s, int maxSide) {
    const Pixels p = cropPixels(imageW, imageH, s);
    const double scale = std::min(1.0, static_cast<double>(maxSide) / std::max(p.sw, p.sh));
    return {static_cast<int>(std::max<std::int64_t>(1, jsRound(p.sw * scale))), static_cast<int>(std::max<std::int64_t>(1, jsRound(p.sh * scale)))};
}

ColorParams colorParams(const EditState& s, double outputHeight) {
    const Preset p = preset(s.filter);
    const Adjust& a = s.adjust;
    // The slider is held to its range first, then scaled: brightness and contrast move less than saturation.
    const auto slider = [](double v, double strength) { return 1 + clamp(v, -100.0, 100.0) * strength / 100; };
    ColorParams c;
    c.brightness = p.brightness * slider(a.brightness, 0.6);
    c.contrast = p.contrast * slider(a.contrast, 0.8);
    c.saturate = p.saturate * slider(a.saturation, 1.0);
    c.grayscale = p.grayscale;
    c.sepia = p.sepia;
    c.hueRotate = p.hueRotate;
    c.blurPx = clamp(a.blur, 0.0, 100.0) / 100 * (outputHeight * 0.01);
    return c;
}

void adjustPixels(std::vector<std::uint8_t>& rgba, const ColorParams& c, bool hue) {
    double out[3];
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        adjustOne(rgba[i], rgba[i + 1], rgba[i + 2], c, hue, out);
        rgba[i] = static_cast<std::uint8_t>(toByte(out[0]));
        rgba[i + 1] = static_cast<std::uint8_t>(toByte(out[1]));
        rgba[i + 2] = static_cast<std::uint8_t>(toByte(out[2]));
    }
}

void adjustArgb(std::uint32_t* pixels, std::size_t count, const ColorParams& c, bool hue) {
    double out[3];
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t p = pixels[i];
        adjustOne((p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff, c, hue, out);
        pixels[i] = (p & 0xff000000u) | (static_cast<std::uint32_t>(toByte(out[0])) << 16) | (static_cast<std::uint32_t>(toByte(out[1])) << 8) |
                    static_cast<std::uint32_t>(toByte(out[2]));
    }
}

void blurArgb(std::uint32_t* pixels, int width, int height, double radius) {
    if (radius < 0.5 || width < 1 || height < 1) return;
    // Three box passes of width w approximate a Gaussian of sigma = radius.
    const int box = std::max(1, static_cast<int>(std::nearbyint(std::sqrt(4 * radius * radius + 1) / 2)));
    const auto n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint32_t> a(pixels, pixels + n);
    std::vector<std::uint32_t> tmp(n);
    for (int pass = 0; pass < 3; ++pass) {
        boxPass(a, tmp, width, height, box, true);
        boxPass(tmp, a, width, height, box, false);
    }
    std::copy(a.begin(), a.end(), pixels);
}

Stroke newStroke(std::string_view color, double size, bool eraser, Point at) {
    Stroke s;
    s.color = std::string(color);
    s.size = clamp(size, kMinPen, kMaxPen);
    s.eraser = eraser;
    s.points.push_back(Point{clamp(at.x, 0.0, 1.0), clamp(at.y, 0.0, 1.0)});
    return s;
}

Stroke extendStroke(const Stroke& stroke, Point p) {
    if (stroke.points.size() >= kMaxPointsPerStroke) return stroke;
    const Point next{clamp(p.x, 0.0, 1.0), clamp(p.y, 0.0, 1.0)};
    // Points too close to the last one only make the file heavier.
    if (!stroke.points.empty() && std::hypot(next.x - stroke.points.back().x, next.y - stroke.points.back().y) < 0.0015) return stroke;
    Stroke out = stroke;
    out.points.push_back(next);
    return out;
}

EditState addStroke(const EditState& s, const Stroke& stroke) {
    EditState out = s;
    out.strokes.push_back(stroke);
    if (out.strokes.size() > kMaxStrokes) out.strokes.erase(out.strokes.begin(), out.strokes.end() - static_cast<std::ptrdiff_t>(kMaxStrokes));
    return out;
}

EditState withGeometry(const EditState& s, int rotation, bool flipX, const std::optional<Rect>& crop) {
    EditState out = s;
    out.rotation = rotation;
    out.flipX = flipX;
    out.crop = crop;
    out.strokes.clear();
    return out;
}

}  // namespace image
}  // namespace redvault::studio
