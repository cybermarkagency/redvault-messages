#include "media_io.hpp"

#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QCameraDevice>
#include <QDateTime>
#include <QGuiApplication>
#include <QMediaDevices>
#include <QMetaObject>

#include <algorithm>
#include <cstring>

#include "video_convert.hpp"

#ifdef REDVAULT_HAS_APM
#include <modules/audio_processing/include/audio_processing.h>
#endif

namespace redvault::app {

namespace {

constexpr int k10ms = rtc::kSampleRate / 100;  // 480 samples: the echo canceller's frame

QAudioFormat callFormat() {
    QAudioFormat f;
    f.setSampleRate(rtc::kSampleRate);
    f.setChannelCount(1);
    f.setSampleFormat(QAudioFormat::Int16);
    return f;
}

template <typename List>
QAudioDevice pick(const List& devices, const QByteArray& id, const QAudioDevice& fallback) {
    for (const auto& d : devices) {
        if (d.id() == id) return d;
    }
    return fallback;
}

}  // namespace

// --- Echo cancellation (optional) ---

class EchoCanceller {
public:
#ifdef REDVAULT_HAS_APM
    EchoCanceller() {
        apm_ = webrtc::AudioProcessingBuilder().Create();
        if (!apm_) return;
        webrtc::AudioProcessing::Config config;
        config.echo_canceller.enabled = true;
        config.noise_suppression.enabled = true;
        config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
        config.gain_controller1.enabled = true;
        config.gain_controller1.mode = webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital;
        config.high_pass_filter.enabled = true;
        apm_->ApplyConfig(config);
    }
    ~EchoCanceller() { delete apm_; }
    /// 10 ms of what the speaker is about to play.
    void render(const std::int16_t* pcm) {
        if (!apm_) return;
        std::lock_guard lock(mutex_);
        const webrtc::StreamConfig c(rtc::kSampleRate, 1);
        apm_->ProcessReverseStream(pcm, c, c, reverse_);
    }
    /// 10 ms of microphone, cleaned in place.
    void capture(std::int16_t* pcm) {
        if (!apm_) return;
        std::lock_guard lock(mutex_);
        const webrtc::StreamConfig c(rtc::kSampleRate, 1);
        apm_->set_stream_delay_ms(60);
        apm_->ProcessStream(pcm, c, c, pcm);
    }

private:
    webrtc::AudioProcessing* apm_ = nullptr;
    std::mutex mutex_;
    std::int16_t reverse_[k10ms]{};
#else
    void render(const std::int16_t*) {}
    void capture(std::int16_t*) {}
#endif
};

/// The speaker pulls here; the engine's jitter buffer fills it.
class PlaybackDevice : public QIODevice {
public:
    PlaybackDevice(std::shared_ptr<MediaPort> port, std::shared_ptr<EchoCanceller> echo)
        : port_(std::move(port)), echo_(std::move(echo)) {}
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 16; }

protected:
    qint64 readData(char* data, qint64 maxSize) override {
        // Whole 10 ms blocks, so each one can be handed to the echo canceller.
        const qint64 blocks = maxSize / (k10ms * 2);
        if (blocks <= 0) return 0;
        auto* pcm = reinterpret_cast<std::int16_t*>(data);
        port_->readAudio(pcm, static_cast<std::size_t>(blocks * k10ms));
        for (qint64 b = 0; b < blocks; ++b) echo_->render(pcm + b * k10ms);
        return blocks * k10ms * 2;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    std::shared_ptr<MediaPort> port_;
    std::shared_ptr<EchoCanceller> echo_;
};

namespace {

class CallMediaPort final : public MediaPort {
public:
    explicit CallMediaPort(std::shared_ptr<rtc::CallMedia> media) : media_(std::move(media)) {}
    void sendAudio(const std::int16_t* pcm) override { media_->sendAudio(pcm); }
    void readAudio(std::int16_t* out, std::size_t samples) override { media_->readAudio(out, samples); }
    void sendVideo(const rtc::VideoFrame& frame, bool) override { media_->sendVideo(frame); }

private:
    std::shared_ptr<rtc::CallMedia> media_;
};

}  // namespace

std::shared_ptr<MediaPort> makeCallMediaPort(std::shared_ptr<rtc::CallMedia> media) {
    return std::make_shared<CallMediaPort>(std::move(media));
}

AudioIo::AudioIo(std::shared_ptr<MediaPort> port) : port_(std::move(port)), echo_(std::make_shared<EchoCanceller>()) {}

AudioIo::~AudioIo() {
    stop();
}

bool AudioIo::echoCancellationAvailable() {
#ifdef REDVAULT_HAS_APM
    return true;
#else
    return false;
#endif
}

void AudioIo::start(const QByteArray& microphoneId, const QByteArray& speakerId) {
    startCapture(microphoneId);
    startPlayback(speakerId);
}

void AudioIo::startCapture(const QByteArray& id) {
    if (source_) {
        source_->stop();
        delete source_;
        source_ = nullptr;
        input_ = nullptr;
    }
    const QAudioDevice device = pick(QMediaDevices::audioInputs(), id, QMediaDevices::defaultAudioInput());
    if (device.isNull()) return;
    source_ = new QAudioSource(device, callFormat(), this);
    source_->setBufferSize(rtc::kFrameSamples * 2 * 4);
    input_ = source_->start();
    if (input_) connect(input_, &QIODevice::readyRead, this, &AudioIo::onCaptured);
    pending_.clear();
}

void AudioIo::startPlayback(const QByteArray& id) {
    if (sink_) {
        sink_->stop();
        delete sink_;
        sink_ = nullptr;
    }
    delete output_;
    output_ = nullptr;
    const QAudioDevice device = pick(QMediaDevices::audioOutputs(), id, QMediaDevices::defaultAudioOutput());
    if (device.isNull()) return;
    sink_ = new QAudioSink(device, callFormat(), this);
    sink_->setBufferSize(k10ms * 2 * 6);  // ~60 ms: low latency, room for scheduling jitter
    output_ = new PlaybackDevice(port_, echo_);
    output_->open(QIODevice::ReadOnly);
    sink_->start(output_);
}

void AudioIo::setMicrophone(const QByteArray& microphoneId) {
    startCapture(microphoneId);
}

void AudioIo::setSpeaker(const QByteArray& speakerId) {
    startPlayback(speakerId);
}

void AudioIo::stop() {
    if (source_) {
        source_->stop();
        delete source_;
        source_ = nullptr;
        input_ = nullptr;
    }
    if (sink_) {
        sink_->stop();
        delete sink_;
        sink_ = nullptr;
    }
    delete output_;
    output_ = nullptr;
}

void AudioIo::onCaptured() {
    if (!input_) return;
    const QByteArray bytes = input_->readAll();
    const auto* samples = reinterpret_cast<const std::int16_t*>(bytes.constData());
    pending_.insert(pending_.end(), samples, samples + bytes.size() / 2);
    while (pending_.size() >= static_cast<std::size_t>(rtc::kFrameSamples)) {
        std::int16_t frame[rtc::kFrameSamples];
        std::memcpy(frame, pending_.data(), sizeof frame);
        pending_.erase(pending_.begin(), pending_.begin() + rtc::kFrameSamples);
        echo_->capture(frame);
        echo_->capture(frame + k10ms);
        if (muted_) std::memset(frame, 0, sizeof frame);
        port_->sendAudio(frame);
    }
}

// --- Video ---

VideoIo::VideoIo(std::shared_ptr<MediaPort> port, QObject* parent, bool separateScreen)
    : QObject(parent), port_(std::move(port)), separateScreen_(separateScreen) {
    encoder_.setMaxThreadCount(separateScreen_ ? 2 : 1);
    cameraSession_.setVideoSink(&cameraSink_);
    screenSession_.setVideoSink(&screenSink_);
    connect(&cameraSink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& f) { onFrame(f, false); });
    connect(&screenSink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& f) { onFrame(f, true); });
}

VideoIo::~VideoIo() {
    stopScreen();
    stopCamera();
    encoder_.waitForDone(2000);
}

void VideoIo::startCamera(const QByteArray& cameraId) {
    stopCamera();
    QCameraDevice device = QMediaDevices::defaultVideoInput();
    for (const auto& d : QMediaDevices::videoInputs()) {
        if (d.id() == cameraId) device = d;
    }
    if (device.isNull()) return;
    camera_ = new QCamera(device, this);
    cameraSession_.setCamera(camera_);
    camera_->start();
}

void VideoIo::stopCamera() {
    if (!camera_) return;
    camera_->stop();
    cameraSession_.setCamera(nullptr);
    delete camera_;
    camera_ = nullptr;
}

bool VideoIo::cameraActive() const {
    return camera_ && camera_->isActive() && cameraEnabled_;
}

void VideoIo::setCameraEnabled(bool enabled) {
    cameraEnabled_ = enabled;
    if (!enabled && localSink_ && !sharing_) localSink_->setVideoFrame(QVideoFrame());
}

bool VideoIo::startScreen() {
    if (!screen_) {
        screen_ = new QScreenCapture(this);
        screenSession_.setScreenCapture(screen_);
        connect(screen_, &QScreenCapture::errorOccurred, this, [this] {
            stopScreen();
            emit screenStopped();
        });
    }
    screen_->setScreen(QGuiApplication::primaryScreen());
    screen_->setActive(true);
    sharing_ = screen_->isActive() || screen_->error() == QScreenCapture::NoError;
    return sharing_;
}

void VideoIo::stopScreen() {
    if (screen_) screen_->setActive(false);
    sharing_ = false;
}

void VideoIo::onFrame(const QVideoFrame& frame, bool fromScreen) {
    if (!frame.isValid()) return;
    if (!separateScreen_ && fromScreen != sharing_) return;  // one track: only the active source is shown and sent
    if (separateScreen_ && fromScreen && !sharing_) return;
    if (!fromScreen && !cameraEnabled_) return;
    const int index = fromScreen ? 1 : 0;
    QPointer<QVideoSink> sink = fromScreen && separateScreen_ ? localScreenSink_ : localSink_;
    if (sink) sink->setVideoFrame(frame);
    // ~15 fps for the screen (sharper, fewer frames), ~24 for the camera.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastSentMs_[index] < (fromScreen ? 66 : 40)) return;
    if (encoding_[index].exchange(true)) return;  // still busy with the last one: drop this frame
    lastSentMs_[index] = now;
    auto port = port_;
    QVideoFrame copy = frame;
    encoder_.start([this, port, copy, fromScreen, index]() mutable {
        const rtc::VideoFrame i420 = fromScreen ? video::toI420(copy, 1280, 720) : video::toI420(copy, 640, 480);
        if (i420.width > 0) port->sendVideo(i420, fromScreen);
        encoding_[index] = false;
    });
}

void VideoIo::showRemote(const rtc::VideoFrame& frame) {
    QVideoFrame converted = video::toVideoFrame(frame);
    QPointer<VideoIo> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, converted] {
            if (self && self->remoteSink_) self->remoteSink_->setVideoFrame(converted);
        },
        Qt::QueuedConnection);
}

}  // namespace redvault::app
