#include "chat_images.hpp"

#include <QBuffer>
#include <QImageReader>
#include <QMutexLocker>

namespace redvault::app {

namespace {

// Bytes kept for full views across all chats; thumbnails stay regardless.
constexpr qint64 kMaxBytes = 256LL * 1024 * 1024;
// Formats sniffed from the content that may be decoded (no SVG: it can reference other resources).
const QList<QByteArray> kFormats = {"jpeg", "jpg", "png", "gif", "webp", "bmp"};

}  // namespace

ChatImageCache& ChatImageCache::instance() {
    static ChatImageCache cache;
    return cache;
}

std::optional<QImage> ChatImageCache::decode(const QByteArray& bytes) {
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    // Applies the photo's rotation (EXIF), like every other viewer.
    reader.setAutoTransform(true);
    if (!kFormats.contains(reader.format().toLower())) return std::nullopt;
    // A small file can claim a huge picture: refuse anything that would take more than this to decode.
    reader.setAllocationLimit(160);
    const QImage image = reader.read();
    if (image.isNull()) return std::nullopt;
    return image;
}

QImage ChatImageCache::thumbnail(const QImage& image, int edge) {
    if (image.width() <= edge && image.height() <= edge) return image;
    return image.scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

void ChatImageCache::put(const QString& messageId, const QImage& thumb, const QByteArray& bytes) {
    QMutexLocker lock(&mutex_);
    if (const auto it = entries_.find(messageId); it != entries_.end()) totalBytes_ -= it->bytes.size();
    entries_.insert(messageId, Entry{thumb, bytes, ++clock_});
    totalBytes_ += bytes.size();
    // Over the cap: let go of the least recently used files' bytes (their thumbnails stay).
    while (totalBytes_ > kMaxBytes) {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (!it->bytes.isEmpty() && it.key() != messageId && (oldest == entries_.end() || it->used < oldest->used)) {
                oldest = it;
            }
        }
        if (oldest == entries_.end()) break;
        totalBytes_ -= oldest->bytes.size();
        oldest->bytes.clear();
    }
}

bool ChatImageCache::has(const QString& messageId) const {
    QMutexLocker lock(&mutex_);
    return entries_.contains(messageId);
}

QSize ChatImageCache::thumbSize(const QString& messageId) const {
    QMutexLocker lock(&mutex_);
    const auto it = entries_.find(messageId);
    return it == entries_.end() ? QSize() : it->thumb.size();
}

QImage ChatImageCache::thumb(const QString& messageId) const {
    QMutexLocker lock(&mutex_);
    const auto it = entries_.find(messageId);
    return it == entries_.end() ? QImage() : it->thumb;
}

QImage ChatImageCache::full(const QString& messageId) const {
    const QByteArray data = bytes(messageId);
    if (data.isEmpty()) return thumb(messageId);
    return decode(data).value_or(QImage());
}

QByteArray ChatImageCache::bytes(const QString& messageId) const {
    QMutexLocker lock(&mutex_);
    const auto it = entries_.find(messageId);
    if (it == entries_.end()) return {};
    it->used = ++clock_;
    return it->bytes;
}

void ChatImageCache::clear() {
    QMutexLocker lock(&mutex_);
    entries_.clear();
    totalBytes_ = 0;
}

ChatImageProvider::ChatImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage ChatImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
    // "thumb/<id>" or "full/<id>", optionally followed by "?<revision>" to make QML reload.
    const QString path = id.section(u'?', 0, 0);
    const QString kind = path.section(u'/', 0, 0);
    const QString messageId = path.section(u'/', 1, 1);
    auto& cache = ChatImageCache::instance();
    QImage image = kind == QStringLiteral("full") ? cache.full(messageId) : cache.thumb(messageId);
    if (!image.isNull() && requestedSize.isValid() && requestedSize.width() > 0 && requestedSize.height() > 0 &&
        (image.width() > requestedSize.width() || image.height() > requestedSize.height())) {
        image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (size != nullptr) *size = image.size();
    return image;
}

}  // namespace redvault::app
