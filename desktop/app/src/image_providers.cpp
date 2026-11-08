// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "image_providers.hpp"

#include <QDateTime>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QFile>
#include <QRegularExpression>
#include <QRunnable>
#include <QSvgRenderer>
#include <QUrl>

#include <qrencode.h>

#include "app_controller.hpp"
#include "redvault/api/channel_api.hpp"

namespace redvault::app {

namespace {

constexpr int kAvatarSourceSize = 256;

QImage circle(const QImage& source, int size) {
    const int side = std::min(source.width(), source.height());
    const QImage square = source.copy((source.width() - side) / 2, (source.height() - side) / 2, side, side)
                              .scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage out(size, size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath path;
    path.addEllipse(0, 0, size, size);
    painter.setClipPath(path);
    painter.drawImage(0, 0, square);
    return out;
}

constexpr int kCommunityAvatarSourceSize = 256;
constexpr int kBannerSourceWidth = 1600;
constexpr qint64 kCommunityImageMaxAgeMs = 5 * 60 * 1000;

/// `source` scaled to cover `size`, cut to it from the middle, inside a rounded rectangle.
QImage roundedCover(const QImage& source, const QSize& size, qreal radius) {
    const QImage scaled = source.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QImage cut = scaled.copy((scaled.width() - size.width()) / 2, (scaled.height() - size.height()) / 2, size.width(), size.height());
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath path;
    path.addRoundedRect(0, 0, size.width(), size.height(), radius, radius);
    painter.setClipPath(path);
    painter.drawImage(0, 0, cut);
    return out;
}

/// A community image as asked for: see CommunityImageProvider.
QImage shaped(const QImage& source, const QString& shape, const QSize& requestedSize) {
    if (shape == QStringLiteral("banner")) {
        QSize size = requestedSize.isValid() && requestedSize.width() > 0 && requestedSize.height() > 0
                         ? requestedSize
                         : QSize(source.width(), std::min(source.height(), source.width() / 4));
        if (size.isEmpty()) return source;
        // Card corners (Theme.radiusCard) at the 2x the screens ask for.
        return roundedCover(source, size, 32);
    }
    const int side = requestedSize.isValid() && requestedSize.width() > 0 ? requestedSize.width() : 96;
    if (shape == QStringLiteral("circle")) return circle(source, side);
    // A group avatar's corners (Avatar.qml: 30% of its size).
    return roundedCover(source, QSize(side, side), side * 0.3);
}

}  // namespace

/// What QML holds: filled in (on its own thread) when the fetch reports back.
class AvatarResponse : public QQuickImageResponse {
public:
    QQuickTextureFactory* textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(image_);
    }
    QString errorString() const override { return error_; }

    void deliver(const QImage& image) {
        if (image.isNull()) error_ = QStringLiteral("No picture");
        image_ = image;
        emit finished();
    }

private:
    QImage image_;
    QString error_;
};

/**
 * The fetch itself, separate from the response (Qt's documented pattern):
 * it reports through a queued signal, so if QML has already dropped the
 * response, the result is simply discarded.
 */
class AvatarFetch : public QObject, public QRunnable {
    Q_OBJECT
public:
    AvatarFetch(AvatarImageProvider* provider, QString avatarUrl, int size)
        : provider_(provider), avatarUrl_(std::move(avatarUrl)), size_(size) {}

    void run() override {
        QImage source;
        {
            QMutexLocker lock(&provider_->mutex_);
            if (const QImage* cached = provider_->cache_.object(avatarUrl_)) source = *cached;
        }
        if (source.isNull()) {
            try {
                const auto api = provider_->app_->api();
                std::string bytes;
                if (provider_->kind_ == AvatarImageProvider::Kind::Channel) {
                    // "<avatarUrl>|<access token>": the URL and token never contain '|'.
                    const auto bar = avatarUrl_.indexOf(u'|');
                    const QString token = bar < 0 ? QString() : avatarUrl_.mid(bar + 1);
                    bytes = api::channels::avatar(*api, (bar < 0 ? avatarUrl_ : avatarUrl_.left(bar)).toStdString(),
                                                  token.isEmpty() ? std::nullopt : std::optional<std::string>(token.toStdString()));
                } else {
                    bytes = api->fetchAvatar(avatarUrl_.toStdString());
                }
                source = QImage::fromData(reinterpret_cast<const uchar*>(bytes.data()), static_cast<int>(bytes.size()));
                if (!source.isNull()) {
                    // Keep a modest copy: avatars are shown small.
                    if (std::max(source.width(), source.height()) > kAvatarSourceSize) {
                        source = source.scaled(kAvatarSourceSize, kAvatarSourceSize, Qt::KeepAspectRatioByExpanding,
                                               Qt::SmoothTransformation);
                    }
                    QMutexLocker lock(&provider_->mutex_);
                    provider_->cache_.insert(avatarUrl_, new QImage(source));
                }
            } catch (const std::exception&) {
                // No photo (removed, offline, signed out): the initial stays.
            }
        }
        emit done(source.isNull() ? QImage() : circle(source, size_));
    }

signals:
    void done(const QImage& image);

private:
    AvatarImageProvider* provider_;
    QString avatarUrl_;
    int size_;
};

AvatarImageProvider::AvatarImageProvider(AppController* app, Kind kind) : app_(app), kind_(kind), cache_(200) {
    pool_.setMaxThreadCount(3);
    // Signing out forgets everyone's photos.
    QObject::connect(app, &AppController::stateChanged, app, [this, app] {
        if (app->state() != AppController::State::SignedIn) clear();
    });
}

AvatarImageProvider::~AvatarImageProvider() {
    pool_.clear();
    pool_.waitForDone();
}

QQuickImageResponse* AvatarImageProvider::requestImageResponse(const QString& id, const QSize& requestedSize) {
    const int size = requestedSize.isValid() && requestedSize.width() > 0 ? requestedSize.width() : 96;
    auto* response = new AvatarResponse;
    auto* fetch = new AvatarFetch(this, QUrl::fromPercentEncoding(id.toUtf8()), size);
    QObject::connect(fetch, &AvatarFetch::done, response, &AvatarResponse::deliver, Qt::QueuedConnection);
    pool_.start(fetch);  // auto-deleted after run()
    return response;
}

void AvatarImageProvider::clear() {
    QMutexLocker lock(&mutex_);
    cache_.clear();
}

/// Like AvatarFetch, for community pictures and banners.
class CommunityImageFetch : public QObject, public QRunnable {
    Q_OBJECT
public:
    CommunityImageFetch(CommunityImageProvider* provider, QString shape, QString key, QString path, QSize requested)
        : provider_(provider), shape_(std::move(shape)), key_(std::move(key)), path_(std::move(path)), requested_(requested) {}

    void run() override {
        QImage source;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        {
            QMutexLocker lock(&provider_->mutex_);
            if (const auto* cached = provider_->cache_.object(key_); cached && now - cached->fetchedAtMs < kCommunityImageMaxAgeMs) {
                source = cached->image;
            }
        }
        if (source.isNull()) {
            try {
                const auto api = provider_->app_->api();
                const std::string bytes = api->fetchCommunityImage(path_.toStdString());
                source = QImage::fromData(reinterpret_cast<const uchar*>(bytes.data()), static_cast<int>(bytes.size()));
                if (!source.isNull()) {
                    // Keep a modest copy: pictures are shown small, banners at most a page wide.
                    const int limit = shape_ == QStringLiteral("banner") ? kBannerSourceWidth : kCommunityAvatarSourceSize;
                    if (std::max(source.width(), source.height()) > limit) {
                        source = source.scaled(limit, limit, shape_ == QStringLiteral("banner") ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding,
                                               Qt::SmoothTransformation);
                    }
                    QMutexLocker lock(&provider_->mutex_);
                    provider_->cache_.insert(key_, new CommunityImageProvider::Cached{source, now});
                }
            } catch (const std::exception&) {
                // No picture (none set, not allowed to see it, offline, signed out): the letter stays.
            }
        }
        emit done(source.isNull() ? QImage() : shaped(source, shape_, requested_));
    }

signals:
    void done(const QImage& image);

private:
    CommunityImageProvider* provider_;
    QString shape_;
    QString key_;
    QString path_;
    QSize requested_;
};

CommunityImageProvider::CommunityImageProvider(AppController* app) : app_(app), cache_(100) {
    pool_.setMaxThreadCount(3);
    QObject::connect(app, &AppController::stateChanged, app, [this, app] {
        if (app->state() != AppController::State::SignedIn) clear();
    });
}

CommunityImageProvider::~CommunityImageProvider() {
    pool_.clear();
    pool_.waitForDone();
}

QQuickImageResponse* CommunityImageProvider::requestImageResponse(const QString& id, const QSize& requestedSize) {
    auto* response = new AvatarResponse;
    // "<shape>/<revision>/<percent-encoded path>"; the fetch itself checks the path.
    static const QRegularExpression kId(QStringLiteral("^(rounded|circle|banner)/([0-9]{1,9})/(.+)$"));
    const auto match = kId.match(id);
    if (!match.hasMatch()) {
        QMetaObject::invokeMethod(response, [response] { response->deliver({}); }, Qt::QueuedConnection);
        return response;
    }
    const QString path = QUrl::fromPercentEncoding(match.captured(3).toUtf8());
    // One cached copy per picture and revision, whatever shape it's shown in.
    const QString key = match.captured(2) + u' ' + path;
    auto* fetch = new CommunityImageFetch(this, match.captured(1), key, path, requestedSize);
    QObject::connect(fetch, &CommunityImageFetch::done, response, &AvatarResponse::deliver, Qt::QueuedConnection);
    pool_.start(fetch);  // auto-deleted after run()
    return response;
}

void CommunityImageProvider::clear() {
    QMutexLocker lock(&mutex_);
    cache_.clear();
}

IconImageProvider::IconImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage IconImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
    // "<name>/<rrggbb>": only names and hex colors, so an id can't reach any other resource.
    static const QRegularExpression kId(QStringLiteral("^([a-z0-9-]{1,40})/([0-9a-fA-F]{6})$"));
    const auto match = kId.match(id);
    if (!match.hasMatch()) return {};
    // The design's own icons (icons/ui, from the web app) first, then Lucide's.
    QFile file(QStringLiteral(":/qt/qml/Redvault/icons/ui/") + match.captured(1) + QStringLiteral(".svg"));
    if (!file.exists()) file.setFileName(QStringLiteral(":/qt/qml/Redvault/icons/lucide/") + match.captured(1) + QStringLiteral(".svg"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray svg = file.readAll();
    svg.replace("currentColor", "#" + match.captured(2).toLatin1());

    const int width = requestedSize.width() > 0 ? requestedSize.width() : 24;
    const int height = requestedSize.height() > 0 ? requestedSize.height() : width;
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QSvgRenderer renderer(svg);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter);
    painter.end();
    if (size != nullptr) *size = image.size();
    return image;
}

QrImageProvider::QrImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage QrImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
    const QByteArray text = QUrl::fromPercentEncoding(id.toUtf8()).toUtf8();
    QRcode* code = QRcode_encodeString8bit(text.constData(), 0, QR_ECLEVEL_M);
    if (code == nullptr) return {};

    constexpr int kQuietZone = 4;  // modules of white border scanners expect
    const int modules = code->width + 2 * kQuietZone;
    const int target = requestedSize.isValid() && requestedSize.width() > 0 ? requestedSize.width() : modules * 8;
    const int scale = std::max(1, target / modules);
    QImage image(modules * scale, modules * scale, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    for (int y = 0; y < code->width; ++y) {
        for (int x = 0; x < code->width; ++x) {
            if (code->data[y * code->width + x] & 1) {
                painter.fillRect((x + kQuietZone) * scale, (y + kQuietZone) * scale, scale, scale, Qt::black);
            }
        }
    }
    painter.end();
    QRcode_free(code);
    if (size != nullptr) *size = image.size();
    return image;
}

}  // namespace redvault::app

#include "image_providers.moc"
