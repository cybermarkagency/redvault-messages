// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "qr_scanner.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QImageReader>
#include <QMimeData>

#include <ReadBarcode.h>  // zxing-cpp (built from source: its headers are in core/src)

namespace redvault::app {

namespace {
constexpr qint64 kFrameIntervalMs = 250;
// A safety-number code is short; anything bigger isn't one of ours.
constexpr qsizetype kMaxTextLength = 2048;
// Pictures larger than this are scaled down first (decoding stays quick).
constexpr int kMaxSide = 1600;
}  // namespace

QrScanner::QrScanner(QObject* parent) : QObject(parent) {}

void QrScanner::setVideoSink(QVideoSink* sink) {
    if (sink_ == sink) return;
    if (sink_) disconnect(sink_, nullptr, this, nullptr);
    sink_ = sink;
    if (sink_) connect(sink_, &QVideoSink::videoFrameChanged, this, &QrScanner::onFrame);
    emit videoSinkChanged();
}

void QrScanner::onFrame(const QVideoFrame& frame) {
    if (last_.isValid() && last_.elapsed() < kFrameIntervalMs) return;
    last_.start();
    const QString text = decode(frame.toImage());
    if (!text.isEmpty()) emit found(text);
}

QString QrScanner::decodeFile(const QUrl& file) const {
    if (!file.isLocalFile()) return {};
    QImageReader reader(file.toLocalFile());
    reader.setAutoTransform(true);
    return decode(reader.read());
}

QString QrScanner::decodeClipboard() const {
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    if (!data || !data->hasImage()) return {};
    return decode(qvariant_cast<QImage>(data->imageData()));
}

QString QrScanner::decode(const QImage& source) {
    if (source.isNull()) return {};
    QImage image = source;
    if (image.width() > kMaxSide || image.height() > kMaxSide) image = image.scaled(kMaxSide, kMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image = image.convertToFormat(QImage::Format_Grayscale8);
    const ZXing::ImageView view(image.constBits(), image.width(), image.height(), ZXing::ImageFormat::Lum, static_cast<int>(image.bytesPerLine()));
    ZXing::ReaderOptions options;
    options.setFormats(ZXing::BarcodeFormat::QRCode);
    options.setTryHarder(true);
    options.setTryRotate(true);
    const auto result = ZXing::ReadBarcode(view, options);
    if (!result.isValid()) return {};
    const QString text = QString::fromStdString(result.text());
    return text.size() <= kMaxTextLength ? text : QString();
}

}  // namespace redvault::app
