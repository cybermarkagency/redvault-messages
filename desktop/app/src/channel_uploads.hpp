// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QList>
#include <QObject>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/api/channel_api.hpp"

namespace redvault::app {

/**
 * The attachments of a post (or a suggested post) being written, as the web
 * app's useUploads: each picked or recorded file starts uploading as soon as
 * it's added, streamed from disk with progress, and the post goes out with
 * the ones that finished.
 */
class ChannelUploads : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ChannelsSession")
    /// [{key, name, kind, kindLabel, sizeLabel, progress (0..1), ready, error}].
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int readyCount READ readyCount NOTIFY changed)
    /// Something is still uploading.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool full READ full NOTIFY changed)

public:
    /// Asks the server for an upload (the owner's, or a follower's for a suggestion).
    using Starter = std::function<api::channels::StartedUpload(const api::channels::UploadMeta&)>;

    /// `photosOnly`: a suggested post, which takes photos only.
    ChannelUploads(std::shared_ptr<api::ApiClient> api, int max, bool photosOnly, QObject* parent = nullptr);
    ~ChannelUploads() override;

    /// Where new uploads go (the channel shown); clears what was there.
    void setStarter(Starter starter);

    QVariantList items() const;
    int count() const { return static_cast<int>(uploads_.size()); }
    int readyCount() const { return static_cast<int>(ready().size()); }
    bool busy() const;
    bool full() const { return count() >= max_; }
    /// The finished uploads' ids, in the order they were added.
    std::vector<std::string> ready() const;

    /// Picked files; `kind` "file" sends them as files, otherwise pictures go as photos, videos as videos and audio as audio.
    Q_INVOKABLE void addFiles(const QList<QUrl>& files, const QString& kind = QString());
    /// A voice clip or round video just recorded.
    Q_INVOKABLE void addRecording(const QUrl& file, const QString& kind, int durationMs, const QString& mime);
    Q_INVOKABLE void remove(int key);
    Q_INVOKABLE void clear();

signals:
    void changed();
    void sessionExpired();

private:
    struct Upload {
        int key = 0;
        QString path;
        QString name;
        std::string kind;
        std::string mime;
        qint64 size = 0;
        double progress = 0;
        std::optional<std::string> mediaId;
        QString error;
        std::optional<std::int64_t> durationMs;
        std::optional<int> width;
        std::optional<int> height;
    };

    void add(Upload upload);
    void begin(int key);
    Upload* find(int key);

    std::shared_ptr<api::ApiClient> api_;
    int max_;
    bool photosOnly_;
    Starter starter_;
    int generation_ = 0;
    int nextKey_ = 1;
    std::vector<Upload> uploads_;
    QThreadPool pool_;
};

}  // namespace redvault::app
