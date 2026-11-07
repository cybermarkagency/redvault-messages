#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>

#include <optional>

namespace redvault::app {

/**
 * Decrypted chat images, in memory only: for each message a thumbnail, and
 * the image file's bytes for the full view (capped in total; the oldest
 * are dropped first and fetched again if opened). Nothing decrypted is kept
 * on disk. Shared with the image provider's thread, so it's locked; emptied
 * when the chat session ends.
 */
class ChatImageCache {
public:
    static ChatImageCache& instance();

    /// Decodes untrusted bytes (JPEG, PNG, GIF, WebP or BMP only, judged by content). Empty on failure.
    static std::optional<QImage> decode(const QByteArray& bytes);
    /// A thumbnail at most `edge` px on its long side.
    static QImage thumbnail(const QImage& image, int edge = 640);

    void put(const QString& messageId, const QImage& thumb, const QByteArray& bytes);
    bool has(const QString& messageId) const;
    QSize thumbSize(const QString& messageId) const;
    QImage thumb(const QString& messageId) const;
    /// The full image, decoded from the kept bytes (null if they were dropped).
    QImage full(const QString& messageId) const;
    /// The image file's bytes, for saving or opening (empty if dropped).
    QByteArray bytes(const QString& messageId) const;
    void clear();

private:
    struct Entry {
        QImage thumb;
        QByteArray bytes;
        mutable quint64 used = 0;
    };
    mutable QMutex mutex_;
    QHash<QString, Entry> entries_;
    qint64 totalBytes_ = 0;
    mutable quint64 clock_ = 0;
};

/**
 * "image://chatimage/thumb/<messageId>" and ".../full/<messageId>": images
 * from ChatImageCache. Asynchronous QML Images call this off the UI thread,
 * so decoding a full image doesn't stall the app.
 */
class ChatImageProvider : public QQuickImageProvider {
public:
    ChatImageProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

}  // namespace redvault::app
