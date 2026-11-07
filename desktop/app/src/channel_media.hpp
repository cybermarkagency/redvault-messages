#pragma once

#include <QAudioInput>
#include <QAudioOutput>
#include <QCache>
#include <QCamera>
#include <QIODevice>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaPlayer>
#include <QMediaRecorder>
#include <QMutex>
#include <QPointer>
#include <QQuickAsyncImageProvider>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <string>

#include "redvault/api/api_client.hpp"

namespace redvault::app {

class AppController;
class ChannelsSession;

/**
 * A channel's video or recording read in byte ranges through the app's own
 * HTTP client (no cookie, no User-Agent, only our server), so the media
 * player never opens connections of its own. Seeking just moves where the
 * next range starts. Read from the player's own thread.
 */
class HttpRangeDevice : public QIODevice {
    Q_OBJECT
public:
    HttpRangeDevice(std::shared_ptr<api::ApiClient> api, std::string url, qint64 size, QObject* parent = nullptr);

    bool isSequential() const override { return false; }
    qint64 size() const override { return size_; }

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    std::shared_ptr<api::ApiClient> api_;
    std::string url_;
    qint64 size_;
    QMutex mutex_;
    std::string buffer_;
    qint64 bufferStart_ = 0;
};

/**
 * Plays a channel's video, podcast, voice clip or round video for QML.
 * Nothing is fetched until play is pressed. A podcast (`resume`) starts
 * again where you stopped, remembered on this computer as the web app does.
 */
class ChannelMediaPlayer : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(redvault::app::ChannelsSession* session READ session WRITE setSession NOTIFY sourceChanged)
    /// The signed media link, and the file's size in bytes.
    Q_PROPERTY(QString url READ url WRITE setUrl NOTIFY sourceChanged)
    Q_PROPERTY(qint64 size READ size WRITE setSize NOTIFY sourceChanged)
    /// Remembers the position under this id (podcasts).
    Q_PROPERTY(QString mediaId READ mediaId WRITE setMediaId NOTIFY sourceChanged)
    Q_PROPERTY(bool resume READ resume WRITE setResume NOTIFY sourceChanged)
    /// A VideoOutput, for videos.
    Q_PROPERTY(QObject* videoOutput READ videoOutput WRITE setVideoOutput NOTIFY sourceChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool started READ started NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    /// Milliseconds; until the media says, the length the post gave (`knownDuration`).
    Q_PROPERTY(qint64 duration READ duration NOTIFY positionChanged)
    Q_PROPERTY(qint64 knownDuration READ knownDuration WRITE setKnownDuration NOTIFY positionChanged)
    Q_PROPERTY(double speed READ speed NOTIFY stateChanged)
    /// Where a podcast resumes, in seconds (0 for none).
    Q_PROPERTY(double resumeAt READ resumeAt NOTIFY positionChanged)

public:
    explicit ChannelMediaPlayer(QObject* parent = nullptr);
    ~ChannelMediaPlayer() override;

    ChannelsSession* session() const { return session_; }
    void setSession(ChannelsSession* session);
    QString url() const { return url_; }
    void setUrl(const QString& url);
    qint64 size() const { return size_; }
    void setSize(qint64 size);
    QString mediaId() const { return mediaId_; }
    void setMediaId(const QString& id);
    bool resume() const { return resume_; }
    void setResume(bool on);
    QObject* videoOutput() const { return videoOutput_; }
    void setVideoOutput(QObject* output);
    bool playing() const;
    bool loading() const { return loading_; }
    bool started() const { return device_ != nullptr; }
    QString error() const { return error_; }
    qint64 position() const;
    qint64 duration() const;
    qint64 knownDuration() const { return knownDuration_; }
    void setKnownDuration(qint64 ms);
    double speed() const { return speed_; }
    double resumeAt() const;

    Q_INVOKABLE void toggle();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void seek(qint64 ms);
    Q_INVOKABLE void skip(int seconds);
    Q_INVOKABLE void cycleSpeed();

signals:
    void sourceChanged();
    void stateChanged();
    void positionChanged();

private:
    void savePosition(double seconds);

    QPointer<ChannelsSession> session_;
    QString url_;
    qint64 size_ = 0;
    QString mediaId_;
    bool resume_ = false;
    QPointer<QObject> videoOutput_;
    QMediaPlayer player_;
    QAudioOutput audio_;
    HttpRangeDevice* device_ = nullptr;
    bool loading_ = false;
    bool resumed_ = false;
    QString error_;
    qint64 knownDuration_ = 0;
    double speed_ = 1.0;
    qint64 lastSavedMs_ = 0;
};

/**
 * Records a voice clip (the microphone, 15 minutes at most) or a round
 * video message (the front camera and microphone, a minute at most) into a
 * file, for the composer to upload.
 */
class ChannelRecorder : public QObject {
    Q_OBJECT
    QML_ELEMENT
    /// "voice" or "round".
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY stateChanged)
    /// A VideoOutput showing the camera while a round video records.
    Q_PROPERTY(QObject* videoOutput READ videoOutput WRITE setVideoOutput NOTIFY stateChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(int seconds READ seconds NOTIFY secondsChanged)
    Q_PROPERTY(int maxSeconds READ maxSeconds NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
    explicit ChannelRecorder(QObject* parent = nullptr);
    ~ChannelRecorder() override;

    QString kind() const { return kind_; }
    void setKind(const QString& kind);
    QObject* videoOutput() const { return videoOutput_; }
    void setVideoOutput(QObject* output);
    bool recording() const;
    int seconds() const { return seconds_; }
    int maxSeconds() const;
    QString error() const { return error_; }

    Q_INVOKABLE void start();
    /// Stops and hands the recording over (`finished`).
    Q_INVOKABLE void stop();
    /// Stops and throws the recording away.
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    void secondsChanged();
    /// The recording, in a file of this session's cache, and how long it is.
    void finished(const QUrl& file, int durationMs, const QString& mime);

private:
    void fail(const QString& message);
    void tearDown();

    QString kind_ = QStringLiteral("voice");
    QPointer<QObject> videoOutput_;
    QMediaCaptureSession session_;
    QAudioInput* audioInput_ = nullptr;
    QCamera* camera_ = nullptr;
    QMediaRecorder recorder_;
    QTimer timer_;
    QString path_;
    QString mime_;
    int seconds_ = 0;
    qint64 durationMs_ = 0;
    bool cancelled_ = false;
    QString error_;
};

/**
 * "image://channelmedia/<size>|<media link>": a channel's photo, fetched
 * without the cookie through the app's own HTTP client; <size> is "fit" for
 * the post (scaled down to the requested size) or "full" for the original.
 * Decoded pictures are cached per photo for the session.
 */
class ChannelMediaImageProvider : public QQuickAsyncImageProvider {
public:
    explicit ChannelMediaImageProvider(AppController* app);
    ~ChannelMediaImageProvider() override;

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

private:
    friend class ChannelPhotoFetch;
    AppController* app_;
    QThreadPool pool_;
    QMutex mutex_;
    QCache<QString, QImage> cache_;
};

}  // namespace redvault::app
