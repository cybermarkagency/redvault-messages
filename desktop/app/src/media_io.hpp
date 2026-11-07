#pragma once

#include <QAudioDevice>
#include <QByteArray>
#include <QCamera>
#include <QIODevice>
#include <QMediaCaptureSession>
#include <QObject>
#include <QPointer>
#include <QScreenCapture>
#include <QThread>
#include <QThreadPool>
#include <QVideoSink>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "redvault/rtc/call_media.hpp"

class QAudioSource;
class QAudioSink;

namespace redvault::app {

class EchoCanceller;

/**
 * Where a call's microphone audio and pictures go, and where its speaker
 * audio comes from: the one-to-one engine, or a group call. AudioIo and VideoIo
 * only know this, so both kinds of call share the same device handling.
 */
class MediaPort {
public:
    virtual ~MediaPort() = default;
    /// One 20 ms frame (rtc::kFrameSamples samples) of microphone audio. Any thread.
    virtual void sendAudio(const std::int16_t* pcm) = 0;
    /// Fills `out` with playback audio (silence when there's none). Any thread.
    virtual void readAudio(std::int16_t* out, std::size_t samples) = 0;
    /// One camera or screen picture. A one-to-one call has a single video track, so it ignores `screen`.
    virtual void sendVideo(const rtc::VideoFrame& frame, bool screen) = 0;
};

/// The port of a one-to-one call.
std::shared_ptr<MediaPort> makeCallMediaPort(std::shared_ptr<rtc::CallMedia> media);

/**
 * Microphone in, speaker out, for one call, on a thread of its own so audio
 * never waits on the UI. The microphone is cut into 20 ms frames for the
 * engine; the speaker pulls from the engine's jitter buffer. When the build
 * has webrtc-audio-processing, what the speaker plays is used to cancel its
 * echo from the microphone (plus noise suppression and gain control).
 */
class AudioIo : public QObject {
    Q_OBJECT
public:
    explicit AudioIo(std::shared_ptr<MediaPort> port);
    ~AudioIo() override;

    /// Starts both directions with the given devices (empty = system default). Call on the audio thread.
    Q_INVOKABLE void start(const QByteArray& microphoneId, const QByteArray& speakerId);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void setMicrophone(const QByteArray& microphoneId);
    Q_INVOKABLE void setSpeaker(const QByteArray& speakerId);
    void setMuted(bool muted) { muted_ = muted; }

    static bool echoCancellationAvailable();

private:
    void startCapture(const QByteArray& id);
    void startPlayback(const QByteArray& id);
    void onCaptured();

    std::shared_ptr<MediaPort> port_;
    std::shared_ptr<EchoCanceller> echo_;
    QAudioSource* source_ = nullptr;
    QIODevice* input_ = nullptr;
    QAudioSink* sink_ = nullptr;
    QIODevice* output_ = nullptr;
    std::vector<std::int16_t> pending_;
    std::atomic<bool> muted_{false};
};

/**
 * Camera and screen capture, and the local and remote pictures shown in the
 * call window. Frames are converted and encoded on a worker thread, at most
 * one at a time, so a slow encoder drops frames instead of piling them up.
 */
class VideoIo : public QObject {
    Q_OBJECT
public:
    /// `separateScreen`: the camera and the screen are separate tracks and may both be sent at once (a group call);
    /// otherwise sharing the screen replaces the camera (one video track).
    explicit VideoIo(std::shared_ptr<MediaPort> port, QObject* parent = nullptr, bool separateScreen = false);
    ~VideoIo() override;

    void startCamera(const QByteArray& cameraId);
    void stopCamera();
    void setCameraEnabled(bool enabled);
    bool cameraActive() const;
    /// False if the platform can't capture the screen.
    bool startScreen();
    void stopScreen();
    bool sharingScreen() const { return sharing_; }

    void setLocalSink(QVideoSink* sink) { localSink_ = sink; }
    /// Your own shared screen, when it has a picture of its own (separateScreen).
    void setLocalScreenSink(QVideoSink* sink) { localScreenSink_ = sink; }
    void setRemoteSink(QVideoSink* sink) { remoteSink_ = sink; }
    /// From the engine's thread: shows a decoded remote picture.
    void showRemote(const rtc::VideoFrame& frame);
    /// On the UI thread: shows an already converted remote picture.
    void showRemoteFrame(const QVideoFrame& frame) {
        if (remoteSink_) remoteSink_->setVideoFrame(frame);
    }

signals:
    void screenStopped();

private:
    void onFrame(const QVideoFrame& frame, bool fromScreen);

    std::shared_ptr<MediaPort> port_;
    bool separateScreen_ = false;
    QMediaCaptureSession cameraSession_;
    QCamera* camera_ = nullptr;
    QVideoSink cameraSink_;
    QMediaCaptureSession screenSession_;
    QScreenCapture* screen_ = nullptr;
    QVideoSink screenSink_;
    QPointer<QVideoSink> localSink_;
    QPointer<QVideoSink> localScreenSink_;
    QPointer<QVideoSink> remoteSink_;
    QThreadPool encoder_;
    std::atomic<bool> encoding_[2] = {false, false};  // [0] camera, [1] screen
    bool cameraEnabled_ = true;
    bool sharing_ = false;
    qint64 lastSentMs_[2] = {0, 0};
};

}  // namespace redvault::app
