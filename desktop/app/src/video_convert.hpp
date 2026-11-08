// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QImage>
#include <cstdint>
#include <QVideoFrame>

#include "redvault/rtc/call_media.hpp"

namespace redvault::app::video {

/// Scales (keeping the aspect ratio) to fit maxWidth x maxHeight and converts to I420 (BT.601), even dimensions.
rtc::VideoFrame toI420(const QImage& image, int maxWidth, int maxHeight);

/// A captured frame (any pixel format Qt Multimedia hands out) to I420.
rtc::VideoFrame toI420(const QVideoFrame& frame, int maxWidth, int maxHeight);

/// A decoded remote picture as a QVideoFrame a QML VideoOutput can show.
QVideoFrame toVideoFrame(const rtc::VideoFrame& frame);

/// The same from bare I420 planes (tightly packed: stride = width, width / 2 for U and V). Width and height are even.
QVideoFrame toVideoFrame(int width, int height, const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v);

}  // namespace redvault::app::video
