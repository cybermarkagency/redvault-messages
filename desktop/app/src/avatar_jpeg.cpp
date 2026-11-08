// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "avatar_jpeg.hpp"

#include <QBuffer>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>

#include <algorithm>

#include "redvault/api/api_error.hpp"

namespace redvault::app {

namespace {

// Like Android and the web: a small square JPEG, and a cap on what's read in.
constexpr int kAvatarSize = 128;
constexpr qint64 kMaxAvatarSourceBytes = 10 * 1024 * 1024;

}  // namespace

/**
 * The same processing as Android's avatarJpegBase64 (and the web's
 * readImageAsAvatar): upright per EXIF, center-cropped square, 128 px, JPEG
 * at 85%. Re-encoding drops the photo's EXIF data (location, camera, time),
 * so none of it is uploaded. Runs on a worker thread.
 */
std::string avatarJpegBase64(const QString& path) {
    if (QFileInfo(path).size() > kMaxAvatarSourceBytes) throw api::UserFacingError("That image is too large (max 10 MB).");
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead()) throw api::UserFacingError("That's not an image file.");
    // Decode big photos at a reduced size: a phone photo can be 50+ megapixels.
    const QSize full = reader.size();
    if (full.isValid() && std::min(full.width(), full.height()) > kAvatarSize * 4) {
        reader.setScaledSize(full.scaled(kAvatarSize * 4, kAvatarSize * 4, Qt::KeepAspectRatioByExpanding));
    }
    const QImage image = reader.read();
    if (image.isNull()) throw api::UserFacingError("Couldn't load that as an image.");
    const int side = std::min(image.width(), image.height());
    const QImage square = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
                              .scaled(kAvatarSize, kAvatarSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGB32);  // JPEG has no alpha
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    if (!square.save(&buffer, "JPEG", 85)) throw api::UserFacingError("Couldn't prepare that image.");
    return jpeg.toBase64().toStdString();
}

}  // namespace redvault::app
