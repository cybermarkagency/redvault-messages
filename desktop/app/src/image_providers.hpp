#pragma once

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QQuickImageProvider>
#include <QThreadPool>

namespace redvault::app {

class AppController;

/**
 * "image://avatar/<avatarUrl>": profile photos, fetched with the session
 * cookie (the server only serves them to signed-in users, like Android's
 * avatar loader) off the UI thread, and returned already cut to a circle.
 *
 * "image://channelavatar/<avatarUrl>|<access token>": a channel's photo,
 * fetched WITHOUT the cookie (and with a private channel's access token), so
 * loading it never tells the server which account looked, as the web app's
 * ChannelAvatar does.
 *
 * The in-memory cache is emptied when the user signs out.
 */
class AvatarImageProvider : public QQuickAsyncImageProvider {
public:
    enum class Kind { Profile, Channel };
    explicit AvatarImageProvider(AppController* app, Kind kind = Kind::Profile);
    ~AvatarImageProvider() override;

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

    void clear();

private:
    AppController* app_;
    Kind kind_;
    QThreadPool pool_;
    QMutex mutex_;
    QCache<QString, QImage> cache_;

    friend class AvatarFetch;
};

/**
 * "image://community/<shape>/<revision>/<path>": a community's picture or
 * banner, or a creator's picture, fetched with the session cookie off the UI
 * thread (ApiClient::fetchCommunityImage, which takes only those paths). The
 * path is percent-encoded and comes from api/community_api.hpp's imagePath()
 * or creatorAvatarPath(). `shape` is "rounded" (a square with the corners of a
 * group's avatar), "circle", or "banner" (cut to the requested size, with a
 * card's corners). `revision` changes after this app uploads a picture, so
 * the new one is fetched; otherwise a picture is kept for five minutes (the
 * server's own Cache-Control) and the cache is emptied on signing out.
 */
class CommunityImageProvider : public QQuickAsyncImageProvider {
public:
    explicit CommunityImageProvider(AppController* app);
    ~CommunityImageProvider() override;

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

    void clear();

private:
    struct Cached {
        QImage image;
        qint64 fetchedAtMs = 0;
    };

    AppController* app_;
    QThreadPool pool_;
    QMutex mutex_;
    QCache<QString, Cached> cache_;

    friend class CommunityImageFetch;
};

/**
 * "image://qr/<text>": a QR code of the (percent-encoded) text, dark on
 * white with the standard quiet zone, for authenticator setup and
 * safety-number verification. Nothing is cached.
 */
class QrImageProvider : public QQuickImageProvider {
public:
    QrImageProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

/**
 * "image://icon/<name>/<rrggbb>": a bundled Lucide icon (ISC licensed, see
 * icons/lucide/LICENSE) drawn in that color at the requested size, so icons
 * follow the theme and stay sharp on any display.
 */
class IconImageProvider : public QQuickImageProvider {
public:
    IconImageProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

}  // namespace redvault::app
