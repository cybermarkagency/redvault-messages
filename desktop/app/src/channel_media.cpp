// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "channel_media.hpp"

#include <QAudioDevice>
#include <QBuffer>
#include <QCameraDevice>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMediaDevices>
#include <QMediaFormat>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QRunnable>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

#include "app_controller.hpp"
#include "channels_session.hpp"
#include "redvault/api/channel_api.hpp"
#include "redvault/model/channel_posts.hpp"

namespace redvault::app {

namespace {

namespace channels = api::channels;
namespace posts = model::channel_posts;

/// One range request's worth: big enough to keep a video playing, small enough to seek quickly.
constexpr qint64 kChunk = 1024 * 1024;
/// Where you stopped in podcasts, for the whole computer (the web app keeps it in localStorage).
constexpr const char* kPositionsKey = "channels/mediaPositions";
constexpr int kVoiceMaxSeconds = 15 * 60;
constexpr int kRoundMaxSeconds = 60;

QString positionsJson() {
    return QSettings().value(QLatin1String(kPositionsKey)).toString();
}

}  // namespace

// --- HttpRangeDevice ---

HttpRangeDevice::HttpRangeDevice(std::shared_ptr<api::ApiClient> api, std::string url, qint64 size, QObject* parent)
    : QIODevice(parent), api_(std::move(api)), url_(std::move(url)), size_(size) {}

qint64 HttpRangeDevice::readData(char* data, qint64 maxSize) {
    QMutexLocker lock(&mutex_);
    const qint64 at = pos();
    if (maxSize <= 0 || at >= size_) return 0;
    const qint64 bufferEnd = bufferStart_ + static_cast<qint64>(buffer_.size());
    if (at < bufferStart_ || at >= bufferEnd) {
        const qint64 last = std::min(size_, at + kChunk) - 1;
        try {
            buffer_ = channels::fetchMediaRange(*api_, url_, static_cast<std::uint64_t>(at), static_cast<std::uint64_t>(last));
        } catch (const std::exception& e) {
            buffer_.clear();
            setErrorString(QString::fromUtf8(e.what()));
            return -1;
        }
        // A server that ignored the range sent the whole file.
        bufferStart_ = static_cast<qint64>(buffer_.size()) > last - at + 1 ? 0 : at;
        if (buffer_.empty() || at < bufferStart_ || at >= bufferStart_ + static_cast<qint64>(buffer_.size())) return -1;
    }
    const qint64 offset = at - bufferStart_;
    const qint64 n = std::min<qint64>(maxSize, static_cast<qint64>(buffer_.size()) - offset);
    std::memcpy(data, buffer_.data() + offset, static_cast<std::size_t>(n));
    return n;
}

// --- ChannelMediaPlayer ---

ChannelMediaPlayer::ChannelMediaPlayer(QObject* parent) : QObject(parent) {
    player_.setAudioOutput(&audio_);
    connect(&player_, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
        if (resume_ && std::abs(ms - lastSavedMs_) >= 5000) {
            lastSavedMs_ = ms;
            savePosition(static_cast<double>(ms) / 1000.0);
        }
        emit positionChanged();
    });
    connect(&player_, &QMediaPlayer::durationChanged, this, [this] { emit positionChanged(); });
    connect(&player_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        if (state == QMediaPlayer::PausedState && resume_) savePosition(static_cast<double>(player_.position()) / 1000.0);
        emit stateChanged();
    });
    connect(&player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadingMedia || status == QMediaPlayer::StalledMedia) {
            loading_ = true;
        } else if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia || status == QMediaPlayer::BufferingMedia) {
            loading_ = false;
            if (resume_ && !resumed_) {
                resumed_ = true;
                const double at = posts::savedPosition(positionsJson().toStdString(), mediaId_.toStdString());
                const double length = static_cast<double>(player_.duration()) / 1000.0;
                if (at > 5 && (length <= 0 || at < length - 5)) player_.setPosition(static_cast<qint64>(at * 1000));
            }
        } else if (status == QMediaPlayer::EndOfMedia) {
            loading_ = false;
            if (resume_) savePosition(0);
        } else if (status == QMediaPlayer::InvalidMedia) {
            loading_ = false;
            error_ = QStringLiteral("Couldn't play this.");
        }
        emit stateChanged();
        emit positionChanged();
    });
    connect(&player_, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error, const QString&) {
        if (error == QMediaPlayer::NoError) return;
        loading_ = false;
        error_ = QStringLiteral("Couldn't play this. Reload the channel and try again.");
        emit stateChanged();
    });
}

ChannelMediaPlayer::~ChannelMediaPlayer() {
    if (resume_ && device_ && player_.mediaStatus() != QMediaPlayer::EndOfMedia && player_.position() > 0) {
        savePosition(static_cast<double>(player_.position()) / 1000.0);
    }
    player_.stop();
    player_.setSourceDevice(nullptr);
}

void ChannelMediaPlayer::setSession(ChannelsSession* session) {
    if (session_ == session) return;
    session_ = session;
    emit sourceChanged();
}

void ChannelMediaPlayer::setUrl(const QString& url) {
    if (url_ == url) return;
    url_ = url;
    emit sourceChanged();
}

void ChannelMediaPlayer::setSize(qint64 size) {
    if (size_ == size) return;
    size_ = size;
    emit sourceChanged();
}

void ChannelMediaPlayer::setMediaId(const QString& id) {
    if (mediaId_ == id) return;
    mediaId_ = id;
    emit sourceChanged();
    emit positionChanged();
}

void ChannelMediaPlayer::setResume(bool on) {
    if (resume_ == on) return;
    resume_ = on;
    emit sourceChanged();
    emit positionChanged();
}

void ChannelMediaPlayer::setVideoOutput(QObject* output) {
    if (videoOutput_ == output) return;
    videoOutput_ = output;
    player_.setVideoOutput(output);
    emit sourceChanged();
}

void ChannelMediaPlayer::setKnownDuration(qint64 ms) {
    if (knownDuration_ == ms) return;
    knownDuration_ = ms;
    emit positionChanged();
}

bool ChannelMediaPlayer::playing() const {
    return player_.playbackState() == QMediaPlayer::PlayingState;
}

qint64 ChannelMediaPlayer::position() const {
    return player_.position();
}

qint64 ChannelMediaPlayer::duration() const {
    return player_.duration() > 0 ? player_.duration() : knownDuration_;
}

double ChannelMediaPlayer::resumeAt() const {
    if (!resume_ || mediaId_.isEmpty()) return 0;
    const double at = posts::savedPosition(positionsJson().toStdString(), mediaId_.toStdString());
    return at > 5 ? at : 0;
}

void ChannelMediaPlayer::toggle() {
    playing() ? pause() : play();
}

void ChannelMediaPlayer::play() {
    if (!device_) {
        if (!session_ || !channels::isMediaUrl(url_.toStdString()) || size_ <= 0) {
            error_ = QStringLiteral("Couldn't play this. Reload the channel and try again.");
            emit stateChanged();
            return;
        }
        error_.clear();
        device_ = new HttpRangeDevice(session_->api(), url_.toStdString(), size_, this);
        device_->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        loading_ = true;
        player_.setSourceDevice(device_, QUrl(QStringLiteral("channel-media:") + mediaId_));
        player_.setPlaybackRate(speed_);
        emit stateChanged();
    }
    player_.play();
}

void ChannelMediaPlayer::pause() {
    player_.pause();
}

void ChannelMediaPlayer::seek(qint64 ms) {
    if (!device_) return;
    player_.setPosition(std::max<qint64>(0, ms));
}

void ChannelMediaPlayer::skip(int seconds) {
    if (!device_) return;
    const qint64 length = duration();
    qint64 next = player_.position() + static_cast<qint64>(seconds) * 1000;
    if (length > 0) next = std::min(next, length);
    player_.setPosition(std::max<qint64>(0, next));
}

void ChannelMediaPlayer::cycleSpeed() {
    speed_ = posts::nextSpeed(speed_);
    player_.setPlaybackRate(speed_);
    emit stateChanged();
}

void ChannelMediaPlayer::savePosition(double seconds) {
    if (mediaId_.isEmpty()) return;
    QSettings settings;
    settings.setValue(QLatin1String(kPositionsKey),
                      QString::fromStdString(posts::savePosition(positionsJson().toStdString(), mediaId_.toStdString(), seconds)));
}

// --- ChannelRecorder ---

ChannelRecorder::ChannelRecorder(QObject* parent) : QObject(parent) {
    timer_.setInterval(250);
    connect(&recorder_, &QMediaRecorder::durationChanged, this, [this](qint64 ms) {
        durationMs_ = ms;
        const int now = static_cast<int>(ms / 1000);
        if (now != seconds_) {
            seconds_ = now;
            emit secondsChanged();
        }
        if (seconds_ >= maxSeconds() && recorder_.recorderState() == QMediaRecorder::RecordingState) recorder_.stop();
    });
    connect(&recorder_, &QMediaRecorder::recorderStateChanged, this, [this](QMediaRecorder::RecorderState state) {
        if (state == QMediaRecorder::StoppedState) {
            const QString file = recorder_.actualLocation().isLocalFile() ? recorder_.actualLocation().toLocalFile() : path_;
            const bool keep = !cancelled_ && error_.isEmpty() && QFileInfo(file).size() > 0;
            tearDown();
            if (keep) {
                emit finished(QUrl::fromLocalFile(file), static_cast<int>(durationMs_), mime_);
            } else if (!file.isEmpty()) {
                QFile::remove(file);
            }
        }
        emit stateChanged();
    });
    connect(&recorder_, &QMediaRecorder::errorOccurred, this, [this](QMediaRecorder::Error error, const QString& message) {
        if (error == QMediaRecorder::NoError) return;
        fail(QStringLiteral("Couldn't record: ") + (message.isEmpty() ? QStringLiteral("this computer can't make that recording.") : message));
    });
}

ChannelRecorder::~ChannelRecorder() {
    cancelled_ = true;
    if (recorder_.recorderState() != QMediaRecorder::StoppedState) recorder_.stop();
    tearDown();
}

void ChannelRecorder::setKind(const QString& kind) {
    if (kind_ == kind || recording()) return;
    kind_ = kind == QStringLiteral("round") ? kind : QStringLiteral("voice");
    emit stateChanged();
}

void ChannelRecorder::setVideoOutput(QObject* output) {
    if (videoOutput_ == output) return;
    videoOutput_ = output;
    if (camera_) session_.setVideoOutput(output);
    emit stateChanged();
}

bool ChannelRecorder::recording() const {
    return recorder_.recorderState() == QMediaRecorder::RecordingState;
}

int ChannelRecorder::maxSeconds() const {
    return kind_ == QStringLiteral("round") ? kRoundMaxSeconds : kVoiceMaxSeconds;
}

void ChannelRecorder::start() {
    if (recorder_.recorderState() != QMediaRecorder::StoppedState) return;
    error_.clear();
    cancelled_ = false;
    seconds_ = 0;
    durationMs_ = 0;
    emit secondsChanged();
    const bool round = kind_ == QStringLiteral("round");

    const QAudioDevice microphone = QMediaDevices::defaultAudioInput();
    if (microphone.isNull()) {
        fail(QStringLiteral("There's no microphone to record with."));
        return;
    }
    audioInput_ = new QAudioInput(microphone, this);
    session_.setAudioInput(audioInput_);

    // The formats this computer can make, best first (MP4 plays everywhere; WebM and Ogg are what browsers record).
    struct Choice {
        QMediaFormat::FileFormat file;
        QMediaFormat::AudioCodec audio;
        QMediaFormat::VideoCodec video;
        const char* mime;
        const char* extension;
    };
    const std::vector<Choice> choices =
        round ? std::vector<Choice>{{QMediaFormat::MPEG4, QMediaFormat::AudioCodec::AAC, QMediaFormat::VideoCodec::H264, "video/mp4", "mp4"},
                                    {QMediaFormat::WebM, QMediaFormat::AudioCodec::Opus, QMediaFormat::VideoCodec::VP8, "video/webm", "webm"}}
              : std::vector<Choice>{{QMediaFormat::Mpeg4Audio, QMediaFormat::AudioCodec::AAC, QMediaFormat::VideoCodec::Unspecified, "audio/mp4", "m4a"},
                                    {QMediaFormat::Ogg, QMediaFormat::AudioCodec::Opus, QMediaFormat::VideoCodec::Unspecified, "audio/ogg", "ogg"},
                                    {QMediaFormat::WebM, QMediaFormat::AudioCodec::Opus, QMediaFormat::VideoCodec::Unspecified, "audio/webm", "webm"}};
    std::optional<Choice> chosen;
    for (const auto& choice : choices) {
        QMediaFormat format(choice.file);
        format.setAudioCodec(choice.audio);
        if (round) format.setVideoCodec(choice.video);
        if (format.isSupported(QMediaFormat::Encode)) {
            chosen = choice;
            recorder_.setMediaFormat(format);
            break;
        }
    }
    if (!chosen) {
        fail(QStringLiteral("This computer can't record %1 here.").arg(round ? QStringLiteral("video") : QStringLiteral("audio")));
        return;
    }
    mime_ = QString::fromLatin1(chosen->mime);

    if (round) {
        QCameraDevice device;
        for (const auto& input : QMediaDevices::videoInputs()) {
            if (input.position() == QCameraDevice::FrontFace) device = input;
        }
        if (device.isNull()) device = QMediaDevices::defaultVideoInput();
        if (device.isNull()) {
            fail(QStringLiteral("There's no camera to record with."));
            return;
        }
        camera_ = new QCamera(device, this);
        // A square picture when the camera offers one (the post shows it in a circle either way), not too big.
        QCameraFormat best;
        for (const auto& format : device.videoFormats()) {
            const QSize size = format.resolution();
            if (size.height() < 240 || size.height() > 720) continue;
            const bool square = size.width() == size.height();
            const bool bestSquare = !best.isNull() && best.resolution().width() == best.resolution().height();
            if (best.isNull() || (square && !bestSquare) ||
                (square == bestSquare && std::abs(size.height() - 480) < std::abs(best.resolution().height() - 480))) {
                best = format;
            }
        }
        if (!best.isNull()) camera_->setCameraFormat(best);
        session_.setCamera(camera_);
        if (videoOutput_) session_.setVideoOutput(videoOutput_);
        camera_->start();
    }

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/session-channel-recordings");
    QDir().mkpath(dir);
    path_ = dir + QStringLiteral("/%1-%2.%3")
                      .arg(round ? QStringLiteral("round-video") : QStringLiteral("voice"))
                      .arg(QDateTime::currentMSecsSinceEpoch())
                      .arg(QString::fromLatin1(chosen->extension));
    recorder_.setQuality(QMediaRecorder::NormalQuality);
    recorder_.setOutputLocation(QUrl::fromLocalFile(path_));
    session_.setRecorder(&recorder_);
    recorder_.record();
    emit stateChanged();
}

void ChannelRecorder::stop() {
    if (recorder_.recorderState() != QMediaRecorder::StoppedState) recorder_.stop();
}

void ChannelRecorder::cancel() {
    cancelled_ = true;
    if (recorder_.recorderState() != QMediaRecorder::StoppedState) {
        recorder_.stop();
    } else {
        tearDown();
    }
    emit stateChanged();
}

void ChannelRecorder::fail(const QString& message) {
    error_ = message;
    if (recorder_.recorderState() != QMediaRecorder::StoppedState) {
        recorder_.stop();
    } else {
        tearDown();
    }
    emit stateChanged();
}

void ChannelRecorder::tearDown() {
    timer_.stop();
    if (camera_) {
        camera_->stop();
        session_.setCamera(nullptr);
        camera_->deleteLater();
        camera_ = nullptr;
    }
    if (audioInput_) {
        session_.setAudioInput(nullptr);
        audioInput_->deleteLater();
        audioInput_ = nullptr;
    }
}

// --- ChannelMediaImageProvider ---

namespace {

class ChannelPhotoResponse : public QQuickImageResponse {
public:
    QQuickTextureFactory* textureFactory() const override { return QQuickTextureFactory::textureFactoryForImage(image_); }
    QString errorString() const override { return error_; }

    void deliver(const QImage& image) {
        if (image.isNull()) error_ = QStringLiteral("Couldn't load the photo");
        image_ = image;
        emit finished();
    }

private:
    QImage image_;
    QString error_;
};

}  // namespace

/// The fetch, separate from the response (Qt's documented pattern), reporting through a queued signal.
class ChannelPhotoFetch : public QObject, public QRunnable {
    Q_OBJECT
public:
    ChannelPhotoFetch(ChannelMediaImageProvider* provider, QString id, QSize requested)
        : provider_(provider), id_(std::move(id)), requested_(requested) {}

    void run() override {
        // "<fit|full>|<media link>"
        const auto bar = id_.indexOf(u'|');
        const bool full = id_.left(bar) == QStringLiteral("full");
        const QString url = bar < 0 ? QString() : id_.mid(bar + 1);
        static const QRegularExpression kId(QStringLiteral("^/api/channel-media/([0-9a-fA-F-]{36})\\?"));
        const auto match = kId.match(url);
        // Links change every time posts load; the photo they point to doesn't.
        const QString key = match.hasMatch() ? match.captured(1) + (full ? QStringLiteral("|full") : QStringLiteral("|%1x%2").arg(requested_.width()).arg(requested_.height()))
                                             : QString();
        QImage image;
        if (!key.isEmpty()) {
            QMutexLocker lock(&provider_->mutex_);
            if (const QImage* cached = provider_->cache_.object(key)) image = *cached;
        }
        if (image.isNull() && !key.isEmpty()) {
            try {
                const std::string bytes = channels::fetchMedia(*provider_->app_->api(), url.toStdString());
                QByteArray data = QByteArray::fromStdString(bytes);
                QBuffer buffer(&data);
                buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer);
                reader.setAutoTransform(true);
                const QSize original = reader.size();
                if (!full && requested_.isValid() && original.isValid() &&
                    (original.width() > requested_.width() || original.height() > requested_.height())) {
                    reader.setScaledSize(original.scaled(requested_, Qt::KeepAspectRatio));
                }
                image = reader.read();
                if (!image.isNull()) {
                    QMutexLocker lock(&provider_->mutex_);
                    provider_->cache_.insert(key, new QImage(image), static_cast<int>(std::max<qsizetype>(1, image.sizeInBytes() / 1024)));
                }
            } catch (const std::exception&) {
                // Expired link, offline: the placeholder stays.
            }
        }
        emit done(image);
    }

signals:
    void done(const QImage& image);

private:
    ChannelMediaImageProvider* provider_;
    QString id_;
    QSize requested_;
};

ChannelMediaImageProvider::ChannelMediaImageProvider(AppController* app) : app_(app), cache_(256 * 1024) {
    pool_.setMaxThreadCount(3);
    // Signing out forgets the photos.
    QObject::connect(app, &AppController::stateChanged, app, [this, app] {
        if (app->state() != AppController::State::SignedIn) {
            QMutexLocker lock(&mutex_);
            cache_.clear();
        }
    });
}

ChannelMediaImageProvider::~ChannelMediaImageProvider() {
    pool_.clear();
    pool_.waitForDone();
}

QQuickImageResponse* ChannelMediaImageProvider::requestImageResponse(const QString& id, const QSize& requestedSize) {
    auto* response = new ChannelPhotoResponse;
    auto* fetch = new ChannelPhotoFetch(this, QUrl::fromPercentEncoding(id.toUtf8()), requestedSize);
    QObject::connect(fetch, &ChannelPhotoFetch::done, response, &ChannelPhotoResponse::deliver, Qt::QueuedConnection);
    pool_.start(fetch);
    return response;
}

}  // namespace redvault::app

#include "channel_media.moc"
