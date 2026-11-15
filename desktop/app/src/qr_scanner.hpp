// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <QtQml/qqmlregistration.h>

namespace redvault::app {

/**
 * Reads a QR code (zxing-cpp) from the webcam, a picture file, or a picture
 * on the clipboard, for verifying a contact's safety number like the phone
 * does. Decoding happens on this computer only; nothing is sent anywhere.
 * Camera frames are checked a few times a second, not every frame.
 */
class QrScanner : public QObject {
    Q_OBJECT
    QML_ELEMENT
    /// The camera preview's sink (VideoOutput.videoSink). Null stops scanning.
    Q_PROPERTY(QVideoSink* videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)

public:
    explicit QrScanner(QObject* parent = nullptr);

    QVideoSink* videoSink() const { return sink_; }
    void setVideoSink(QVideoSink* sink);

    /// The text of the first QR code in the picture at `file`, or empty.
    Q_INVOKABLE QString decodeFile(const QUrl& file) const;
    /// The text of the first QR code in a picture on the clipboard (a screenshot), or empty.
    Q_INVOKABLE QString decodeClipboard() const;

    static QString decode(const QImage& image);

signals:
    void videoSinkChanged();
    /// A QR code was read from the camera.
    void found(const QString& text);

private:
    void onFrame(const QVideoFrame& frame);

    QPointer<QVideoSink> sink_;
    QElapsedTimer last_;
};

}  // namespace redvault::app
