// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "voice_recorder.hpp"

#include <QFile>
#include <QMediaFormat>

namespace redvault::app {

VoiceRecorder::VoiceRecorder(QObject* parent) : QObject(parent) {
    session_.setAudioInput(&input_);
    session_.setRecorder(&recorder_);
    QMediaFormat format(QMediaFormat::Mpeg4Audio);
    format.setAudioCodec(QMediaFormat::AudioCodec::AAC);
    recorder_.setMediaFormat(format);
    recorder_.setAudioChannelCount(1);
    recorder_.setAudioSampleRate(44'100);
    recorder_.setAudioBitRate(64'000);
    recorder_.setEncodingMode(QMediaRecorder::ConstantBitRateEncoding);

    connect(&recorder_, &QMediaRecorder::recorderStateChanged, this, &VoiceRecorder::onRecorderState);
    connect(&recorder_, &QMediaRecorder::durationChanged, this, [this](qint64 ms) {
        emit elapsedChanged();
        if (recording_ && ms >= kMaxDurationMs) {
            emit limitReached();
            stop();
        }
    });
    connect(&recorder_, &QMediaRecorder::errorOccurred, this, [this](QMediaRecorder::Error, const QString& message) {
        error_ = message.isEmpty() ? QStringLiteral("Couldn't record from the microphone.") : message;
        keep_ = false;
        emit stateChanged();
    });
}

void VoiceRecorder::start(const QUrl& file) {
    if (recording_ || !file.isLocalFile()) return;
    error_.clear();
    file_ = file;
    keep_ = false;
    recorder_.setOutputLocation(file);
    recording_ = true;
    emit stateChanged();
    recorder_.record();
}

void VoiceRecorder::stop() {
    if (!recording_) return;
    keep_ = recorder_.duration() >= kMinDurationMs && error_.isEmpty();
    recorder_.stop();
}

void VoiceRecorder::cancel() {
    if (!recording_) return;
    keep_ = false;
    recorder_.stop();
}

void VoiceRecorder::onRecorderState(QMediaRecorder::RecorderState state) {
    if (state != QMediaRecorder::StoppedState || !recording_) return;
    // Stopped: the file is complete and closed only now.
    recording_ = false;
    emit stateChanged();
    emit elapsedChanged();
    const QUrl actual = recorder_.actualLocation().isEmpty() ? file_ : recorder_.actualLocation();
    if (keep_ && actual.isLocalFile() && QFile::exists(actual.toLocalFile())) {
        emit finished(actual);
    } else {
        removeFile();
        emit discarded();
    }
}

void VoiceRecorder::removeFile() {
    const QUrl actual = recorder_.actualLocation().isEmpty() ? file_ : recorder_.actualLocation();
    if (actual.isLocalFile()) QFile::remove(actual.toLocalFile());
}

}  // namespace redvault::app
