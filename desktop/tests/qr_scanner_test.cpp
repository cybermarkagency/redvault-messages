// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QImage>
#include <QPainter>

#include <qrencode.h>

#include <string>
#include <vector>

#include "qr_scanner.hpp"
#include "redvault/crypto/safety_number_qr.hpp"

using redvault::app::QrScanner;
namespace safety_qr = redvault::crypto::safety_qr;

namespace {

/// The code the way the app draws it (QrImageProvider): libqrencode, level M, then scaled up with a quiet zone.
QImage drawQr(const std::string& text, int scale = 8, bool photoLike = false) {
    QRcode* code = QRcode_encodeString8bit(text.c_str(), 0, QR_ECLEVEL_M);
    REQUIRE(code != nullptr);
    const int modules = code->width;
    const int margin = 4;
    QImage image((modules + 2 * margin) * scale, (modules + 2 * margin) * scale, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (code->data[y * modules + x] & 1) painter.fillRect((x + margin) * scale, (y + margin) * scale, scale, scale, Qt::black);
        }
    }
    painter.end();
    QRcode_free(code);
    if (photoLike) {
        // A phone screen through a webcam: smaller, grey and not quite square on.
        QImage photo(image.width() + 200, image.height() + 160, QImage::Format_RGB32);
        photo.fill(QColor(70, 70, 70));
        QPainter p(&photo);
        p.translate(100, 80);
        p.rotate(4);
        p.drawImage(0, 0, image.scaled(image.size() * 0.8));
        p.end();
        return photo;
    }
    return image;
}

std::vector<std::string> sampleNumber() {
    return {"12345", "67890", "11111", "22222", "33333", "44444", "55555", "66666", "77777", "88888", "99999", "00000"};
}

}  // namespace

TEST_CASE("a safety-number code the app draws is read back and matches", "[qr]") {
    const auto groups = sampleNumber();
    const std::string text = safety_qr::encode(groups);
    const QString read = QrScanner::decode(drawQr(text));
    REQUIRE(read.toStdString() == text);
    CHECK(safety_qr::compare(read.toStdString(), groups) == safety_qr::Result::Match);

    auto other = groups;
    other[5] = "54321";
    CHECK(safety_qr::compare(read.toStdString(), other) == safety_qr::Result::Mismatch);
}

TEST_CASE("a tilted, smaller code on a grey background is still read", "[qr]") {
    const std::string text = safety_qr::encode(sampleNumber());
    CHECK(QrScanner::decode(drawQr(text, 6, true)).toStdString() == text);
}

TEST_CASE("pictures without a code read as nothing", "[qr]") {
    QImage blank(400, 300, QImage::Format_RGB32);
    blank.fill(Qt::white);
    CHECK(QrScanner::decode(blank).isEmpty());
    CHECK(QrScanner::decode(QImage()).isEmpty());
}
