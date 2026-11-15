// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QAudioInput>
#include <QMediaCaptureSession>
#include <QMediaRecorder>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

namespace redvault::app {

/**
 * Records a voice message from the microphone, the same format as the
 * Android app's VoiceRecorder (AAC, mono, 44.1 kHz, 64 kbit/s, in an .m4a),
 * so every app plays every other's. Up to 5 minutes; anything under 0.7 s
 * is dropped. The recording goes to the file the chat session hands out
 * (ChatSession::newVoiceFile), which the send then deletes.
 */
class VoiceRecorder : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(qint64 elapsedMs READ elapsedMs NOTIFY elapsedChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
    static constexpr qint64 kMaxDurationMs = 5 * 60 * 1000;
    static constexpr qint64 kMinDurationMs = 700;

    explicit VoiceRecorder(QObject* parent = nullptr);

    bool recording() const { return recording_; }
    qint64 elapsedMs() const { return recorder_.duration(); }
    QString error() const { return error_; }

    /// Starts recording into `file` (a file:// URL). The microphone is the system default.
    Q_INVOKABLE void start(const QUrl& file);
    /// Stops; `finished` follows with the file, or `discarded` if it was too short or failed.
    Q_INVOKABLE void stop();
    /// Stops and throws the recording away.
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    void elapsedChanged();
    /// The recording is complete and closed: send it.
    void finished(const QUrl& file);
    /// Nothing to send (cancelled, too short, or the recorder failed: see error).
    void discarded();
    /// It reached 5 minutes and stopped by itself (finished follows).
    void limitReached();

private:
    void onRecorderState(QMediaRecorder::RecorderState state);
    void removeFile();

    QMediaCaptureSession session_;
    QAudioInput input_;
    QMediaRecorder recorder_;
    QUrl file_;
    bool recording_ = false;
    bool keep_ = false;
    QString error_;
};

}  // namespace redvault::app
