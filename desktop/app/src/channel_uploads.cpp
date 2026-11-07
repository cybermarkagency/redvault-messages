#include "channel_uploads.hpp"

#include <QFileInfo>
#include <QImageReader>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMimeDatabase>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <filesystem>

#include "async.hpp"
#include "redvault/model/channel_posts.hpp"
#include "redvault/model/file_message.hpp"

namespace redvault::app {

namespace {

namespace channels = api::channels;
namespace posts = model::channel_posts;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

/**
 * A recording's length and a video's size, read from the file before it's
 * uploaded (best effort, as the web's probeMedia: never more than 4 s).
 */
void probe(const QString& path, QObject* context, std::function<void(std::optional<std::int64_t>, std::optional<int>, std::optional<int>)> done) {
    auto* player = new QMediaPlayer(context);
    auto finished = std::make_shared<bool>(false);
    auto finish = [player, finished, done](bool ok) {
        if (*finished) return;
        *finished = true;
        std::optional<std::int64_t> duration;
        std::optional<int> width;
        std::optional<int> height;
        if (ok) {
            if (player->duration() > 0) duration = player->duration();
            const QSize resolution = player->metaData().value(QMediaMetaData::Resolution).toSize();
            if (resolution.isValid() && !resolution.isEmpty()) {
                width = resolution.width();
                height = resolution.height();
            }
        }
        player->deleteLater();
        done(duration, width, height);
    };
    QObject::connect(player, &QMediaPlayer::mediaStatusChanged, player, [finish](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) finish(true);
        if (status == QMediaPlayer::InvalidMedia) finish(false);
    });
    QObject::connect(player, &QMediaPlayer::errorOccurred, player, [finish] { finish(false); });
    QTimer::singleShot(4000, player, [finish] { finish(false); });
    player->setSource(QUrl::fromLocalFile(path));
}

}  // namespace

ChannelUploads::ChannelUploads(std::shared_ptr<api::ApiClient> api, int max, bool photosOnly, QObject* parent)
    : QObject(parent), api_(std::move(api)), max_(max), photosOnly_(photosOnly) {
    pool_.setMaxThreadCount(2);
}

ChannelUploads::~ChannelUploads() {
    pool_.clear();
    pool_.waitForDone();
}

void ChannelUploads::setStarter(Starter starter) {
    starter_ = std::move(starter);
    ++generation_;
    uploads_.clear();
    emit changed();
}

QVariantList ChannelUploads::items() const {
    QVariantList out;
    for (const auto& u : uploads_) {
        out.append(QVariantMap{{"key", u.key},
                               {"name", u.name},
                               {"kind", qs(u.kind)},
                               {"kindLabel", qs(posts::kindLabel(u.kind))},
                               {"sizeLabel", qs(model::file_message::formatBytes(u.size))},
                               {"progress", u.progress},
                               {"ready", u.mediaId.has_value()},
                               {"error", u.error}});
    }
    return out;
}

bool ChannelUploads::busy() const {
    return std::any_of(uploads_.begin(), uploads_.end(), [](const Upload& u) { return !u.mediaId && u.error.isEmpty(); });
}

std::vector<std::string> ChannelUploads::ready() const {
    std::vector<std::string> out;
    for (const auto& u : uploads_) {
        if (u.mediaId) out.push_back(*u.mediaId);
    }
    return out;
}

ChannelUploads::Upload* ChannelUploads::find(int key) {
    const auto it = std::find_if(uploads_.begin(), uploads_.end(), [key](const Upload& u) { return u.key == key; });
    return it == uploads_.end() ? nullptr : &*it;
}

void ChannelUploads::addFiles(const QList<QUrl>& files, const QString& kind) {
    const QMimeDatabase mimes;
    for (const auto& file : files) {
        const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
        Upload upload;
        upload.path = path;
        upload.name = QFileInfo(path).fileName();
        upload.size = QFileInfo(path).size();
        upload.mime = mimes.mimeTypeForFile(path).name().toStdString();
        if (upload.mime.empty() || upload.mime == "application/octet-stream") upload.mime = "application/octet-stream";
        upload.kind = photosOnly_ ? std::string("photo") : kind == QStringLiteral("file") ? std::string("file") : posts::mediaKindFor(upload.mime);
        add(std::move(upload));
    }
}

void ChannelUploads::addRecording(const QUrl& file, const QString& kind, int durationMs, const QString& mime) {
    Upload upload;
    upload.path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    upload.name = QFileInfo(upload.path).fileName();
    upload.size = QFileInfo(upload.path).size();
    upload.kind = kind == QStringLiteral("round") ? "round" : "voice";
    upload.mime = mime.toStdString();
    if (durationMs > 0) upload.durationMs = durationMs;
    add(std::move(upload));
}

void ChannelUploads::add(Upload upload) {
    if (full()) return;
    upload.key = nextKey_++;
    if (upload.size <= 0) upload.error = QStringLiteral("That file is empty or can't be read.");
    if (upload.error.isEmpty() && photosOnly_ && !posts::isPhotoMime(upload.mime)) {
        upload.error = QStringLiteral("Suggested posts can include photos only.");
    }
    if (upload.error.isEmpty() && !starter_) upload.error = QStringLiteral("The upload failed.");
    const int key = upload.key;
    const bool start = upload.error.isEmpty();
    const std::string kind = upload.kind;
    const QString path = upload.path;
    uploads_.push_back(std::move(upload));
    emit changed();
    if (!start) return;

    if (kind == "photo") {
        const QSize size = QImageReader(path).size();
        if (auto* u = find(key); u && size.isValid()) {
            u->width = size.width();
            u->height = size.height();
        }
        begin(key);
    } else if (kind == "file") {
        begin(key);
    } else {
        const int generation = generation_;
        probe(path, this, [this, key, generation](std::optional<std::int64_t> duration, std::optional<int> width, std::optional<int> height) {
            if (generation != generation_) return;
            if (auto* u = find(key)) {
                if (!u->durationMs) u->durationMs = duration;
                u->width = width;
                u->height = height;
            }
            begin(key);
        });
    }
}

void ChannelUploads::begin(int key) {
    const Upload* u = find(key);
    if (!u) return;
    const channels::UploadMeta meta{u->kind, u->mime, u->size, u->name.toStdString(), u->durationMs, u->width, u->height};
    const std::filesystem::path path(u->path.toStdU16String());
    const qint64 size = u->size;
    auto api = api_;
    auto starter = starter_;
    const int generation = generation_;
    QPointer<ChannelUploads> self(this);
    runInBackground(
        pool_, this,
        [api, starter, meta, path, size, self, key, generation] {
            const auto started = starter(meta);
            qint64 lastReported = 0;
            channels::uploadMedia(*api, started.uploadUrl, path, size, [&](std::int64_t sent) {
                // About every 1%, so the bar moves without flooding the UI thread.
                if (sent - lastReported < size / 100 && sent < size) return;
                lastReported = sent;
                const double progress = static_cast<double>(sent) / static_cast<double>(size);
                QMetaObject::invokeMethod(
                    qApp,
                    [self, key, generation, progress] {
                        if (!self || generation != self->generation_) return;
                        if (auto* upload = self->find(key)) {
                            upload->progress = std::min(progress, 0.99);
                            emit self->changed();
                        }
                    },
                    Qt::QueuedConnection);
            });
            return started.id;
        },
        [this, key, generation](std::string id) {
            if (generation != generation_) return;
            if (auto* upload = find(key)) {
                upload->mediaId = std::move(id);
                upload->progress = 1;
                emit changed();
            }
        },
        [this, key, generation](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            if (generation != generation_) return;
            if (auto* upload = find(key)) {
                upload->error = failure.status == 413 && failure.code.isEmpty() ? QStringLiteral("That file is too big for this server.")
                                : failure.code.isEmpty()                         ? QStringLiteral("The upload failed.")
                                                                                 : failure.message;
                emit changed();
            }
        });
}

void ChannelUploads::remove(int key) {
    std::erase_if(uploads_, [key](const Upload& u) { return u.key == key; });
    emit changed();
}

void ChannelUploads::clear() {
    ++generation_;
    uploads_.clear();
    emit changed();
}

}  // namespace redvault::app
