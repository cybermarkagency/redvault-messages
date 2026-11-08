// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "video_convert.hpp"

#include <QVideoFrameFormat>

#include <algorithm>
#include <cstring>

namespace redvault::app::video {

namespace {

inline std::uint8_t clamp8(int v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
}

}  // namespace

rtc::VideoFrame toI420(const QImage& source, int maxWidth, int maxHeight) {
    if (source.isNull()) return {};
    QSize size = source.size();
    if (size.width() > maxWidth || size.height() > maxHeight) size = size.scaled(maxWidth, maxHeight, Qt::KeepAspectRatio);
    const int w = std::max(2, size.width() & ~1);
    const int h = std::max(2, size.height() & ~1);
    const QImage img = source.scaled(w, h, Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_RGB32);

    rtc::VideoFrame out = rtc::VideoFrame::blank(w, h);
    std::uint8_t* yPlane = out.y();
    std::uint8_t* uPlane = out.u();
    std::uint8_t* vPlane = out.v();
    for (int row = 0; row < h; ++row) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(row));
        for (int col = 0; col < w; ++col) {
            const QRgb p = line[col];
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            yPlane[row * w + col] = clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    // Chroma from each 2x2 block's average.
    for (int row = 0; row < h / 2; ++row) {
        const auto* line0 = reinterpret_cast<const QRgb*>(img.constScanLine(row * 2));
        const auto* line1 = reinterpret_cast<const QRgb*>(img.constScanLine(row * 2 + 1));
        for (int col = 0; col < w / 2; ++col) {
            const QRgb a = line0[col * 2], b = line0[col * 2 + 1], c = line1[col * 2], d = line1[col * 2 + 1];
            const int r = (qRed(a) + qRed(b) + qRed(c) + qRed(d)) / 4;
            const int g = (qGreen(a) + qGreen(b) + qGreen(c) + qGreen(d)) / 4;
            const int bl = (qBlue(a) + qBlue(b) + qBlue(c) + qBlue(d)) / 4;
            uPlane[row * (w / 2) + col] = clamp8(((-38 * r - 74 * g + 112 * bl + 128) >> 8) + 128);
            vPlane[row * (w / 2) + col] = clamp8(((112 * r - 94 * g - 18 * bl + 128) >> 8) + 128);
        }
    }
    return out;
}

rtc::VideoFrame toI420(const QVideoFrame& frame, int maxWidth, int maxHeight) {
    return toI420(frame.toImage(), maxWidth, maxHeight);
}

QVideoFrame toVideoFrame(const rtc::VideoFrame& in) {
    if (in.width <= 0 || in.height <= 0) return {};
    return toVideoFrame(in.width, in.height, in.y(), in.u(), in.v());
}

QVideoFrame toVideoFrame(int w, int h, const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v) {
    if (w <= 0 || h <= 0) return {};
    QVideoFrame frame(QVideoFrameFormat(QSize(w, h), QVideoFrameFormat::Format_YUV420P));
    if (!frame.map(QVideoFrame::WriteOnly)) return {};
    const std::uint8_t* planes[3] = {y, u, v};
    const int widths[3] = {w, w / 2, w / 2};
    const int heights[3] = {h, h / 2, h / 2};
    for (int p = 0; p < 3; ++p) {
        uchar* dst = frame.bits(p);
        const int stride = frame.bytesPerLine(p);
        for (int row = 0; row < heights[p]; ++row) {
            std::memcpy(dst + static_cast<std::ptrdiff_t>(row) * stride, planes[p] + static_cast<std::ptrdiff_t>(row) * widths[p],
                        static_cast<std::size_t>(widths[p]));
        }
    }
    frame.unmap();
    return frame;
}

}  // namespace redvault::app::video
