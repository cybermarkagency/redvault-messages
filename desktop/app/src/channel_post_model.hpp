#pragma once

#include <QAbstractListModel>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/channel_api.hpp"
#include "redvault/model/channel_posts.hpp"

namespace redvault::app {

/**
 * A channel's posts, newest first, for the channel page. A model rather than
 * a list property so that a vote, a tick or a closed poll changes only that
 * post's row: the others keep their delegates (a podcast keeps playing).
 *
 * Each row's "post" is what the page shows:
 *   {id, when, time, views, suggested, body, bodyRich, bodyRevealed,
 *    photos: [media], others: [media], poll: {...} or {}, checklist: {...} or {}}
 * with media {id, kind, url, size, name, sizeLabel, durationLabel, width, height}.
 *
 * The page lists row 0 (the newest post) at the bottom, as a chat does, so a
 * row's "dayHeader" is the day label when the post above it (the next row,
 * which is older) is from another day, and empty otherwise.
 */
class ChannelPostModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ChannelsSession")
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles { PostRole = Qt::UserRole + 1, IdRole, DayHeaderRole };

    /// What this computer remembers of your vote in a poll (for showing results).
    using VoteLookup = std::function<std::optional<model::channel_posts::RememberedVote>(const std::string& postId)>;

    explicit ChannelPostModel(VoteLookup votes, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return static_cast<int>(posts_.size()); }

    const std::vector<api::channels::Post>& posts() const { return posts_; }
    const api::channels::Post* find(const std::string& id) const;

    void reset(std::vector<api::channels::Post> posts);
    void append(std::vector<api::channels::Post> posts);
    void prepend(api::channels::Post post);
    /// The post with this id changed (a vote, a tick, a closed poll): only its row updates.
    void replace(const api::channels::Post& post);
    void remove(const std::string& id);
    /// A remembered vote changed: that post's row shows it.
    void refresh(const std::string& id);

    /// One post as the page shows it (also used for scheduled posts and suggestions' media).
    static QVariantMap rowOf(const api::channels::Post& post, const std::optional<model::channel_posts::RememberedVote>& mine);
    static QVariantMap mediaOf(const api::channels::Media& media);
    /// "18 Oct 2026, 9:35 am".
    static QString when(const std::string& iso);

signals:
    void countChanged();

private:
    QString dayHeader(int row) const;

    std::vector<api::channels::Post> posts_;
    VoteLookup votes_;
};

}  // namespace redvault::app
