// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "async.hpp"
#include "channel_post_model.hpp"
#include "channel_uploads.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/api/channel_api.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/model/channels.hpp"

namespace redvault::app {

/**
 * Channels, the desktop counterpart of the web app's screens/Channels.tsx and
 * ChannelView.tsx: one-way and plain text by design (and labelled so).
 * Premium members create (up to 5) and post: formatted text, photos, videos,
 * files, podcasts, voice clips and round videos, polls, quizzes and
 * checklists, now or scheduled. A public channel is in the directory for
 * anyone to find and follow, a private one only opens from its private link
 * (…?channel=<id>#<secret>). Posts appear as the channel's (its name and
 * photo), never who posted them, each with an anonymous view count;
 * followers may suggest posts when the owner allows it.
 *
 * Reading goes out without the session cookie (api/channel_api.hpp), with a
 * private channel's access token when there is one, and following is kept
 * only on this computer, in the account's local settings, so the server
 * never learns who reads or follows what. For the same reason this
 * computer checks the followed channels itself (every couple of minutes and
 * when the window comes back) for the Channels badge and notifications.
 */
class ChannelsSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    // --- The list ---
    /// Followed on this computer: [{id, name, description, when, unread, deleted, isPrivate, photo}].
    Q_PROPERTY(QVariantList following READ following NOTIFY followingChanged)
    Q_PROPERTY(int followingCount READ followingCount NOTIFY followingChanged)
    /// Followed channels with posts you haven't seen, as of the last check (the Channels badge).
    Q_PROPERTY(int unreadChannels READ unreadChannels NOTIFY unreadChannelsChanged)
    /// The public directory: rows as above.
    Q_PROPERTY(QVariantList discover READ discover NOTIFY discoverChanged)
    Q_PROPERTY(bool discoverLoaded READ discoverLoaded NOTIFY discoverChanged)
    Q_PROPERTY(bool discoverMore READ discoverMore NOTIFY discoverChanged)
    Q_PROPERTY(QString discoverQuery READ discoverQuery NOTIFY discoverChanged)
    Q_PROPERTY(QString discoverError READ discoverError NOTIFY discoverChanged)
    /// Your own channels: rows as above.
    Q_PROPERTY(QVariantList mine READ mine NOTIFY mineChanged)
    Q_PROPERTY(bool mineLoaded READ mineLoaded NOTIFY mineChanged)
    /// Premium: creates channels and posts in them.
    Q_PROPERTY(bool canCreate READ canCreate NOTIFY mineChanged)
    Q_PROPERTY(bool creating READ creating NOTIFY busyChanged)
    /// The Channels page is on screen (a channel open there isn't notified).
    Q_PROPERTY(bool pageShown READ pageShown WRITE setPageShown NOTIFY pageShownChanged)
    // --- One channel ---
    /// The channel shown, or empty for the list.
    Q_PROPERTY(QString channelId READ channelId NOTIFY channelChanged)
    /**
     * {id, name, description, isPrivate, hasPhoto, photo, link,
     * acceptSuggestions}; empty until it has loaded. `photo` is what
     * ChannelAvatar fetches (the photo's URL and, for a private channel, the
     * access token); `link` is empty for a private channel whose secret this
     * computer doesn't have.
     */
    Q_PROPERTY(QVariantMap channel READ channel NOTIFY channelChanged)
    /// Newest first (ChannelPostModel).
    Q_PROPERTY(redvault::app::ChannelPostModel* posts READ postModel CONSTANT)
    Q_PROPERTY(bool hasOlder READ hasOlder NOTIFY postsChanged)
    Q_PROPERTY(bool isMine READ isMine NOTIFY channelChanged)
    Q_PROPERTY(bool isFollowing READ isFollowing NOTIFY followingChanged)
    /// Following, without notifications for new posts.
    Q_PROPERTY(bool isMuted READ isMuted NOTIFY followingChanged)
    /// A super admin can take any channel or post down.
    Q_PROPERTY(bool isAdmin READ isAdmin CONSTANT)
    /// The owner's tools on this channel: its owner, or a super admin (who still can't change who reads it).
    Q_PROPERTY(bool canManage READ canManage NOTIFY channelChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool posting READ posting NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY messagesChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY messagesChanged)
    /// The composer's own problem (shown under it).
    Q_PROPERTY(QString composerError READ composerError NOTIFY composerChanged)
    /// The owner's posts waiting for their time, soonest first: post rows plus {scheduledFor}.
    Q_PROPERTY(QVariantList scheduled READ scheduled NOTIFY scheduledChanged)
    /// The owner: suggestions waiting [{id, body, bodyRich, sent, photos}]. Never who sent them.
    Q_PROPERTY(QVariantList suggestions READ suggestions NOTIFY suggestionsChanged)
    /// A follower: what you suggested to this channel [{id, body, status}].
    Q_PROPERTY(QVariantList mySuggestions READ mySuggestions NOTIFY suggestionsChanged)
    Q_PROPERTY(bool suggestionSent READ suggestionSent NOTIFY suggestionsChanged)
    Q_PROPERTY(QString suggestionError READ suggestionError NOTIFY suggestionsChanged)
    /// The composer's attachments, and a suggestion's photos.
    Q_PROPERTY(redvault::app::ChannelUploads* postUploads READ postUploads CONSTANT)
    Q_PROPERTY(redvault::app::ChannelUploads* suggestionUploads READ suggestionUploads CONSTANT)
    Q_PROPERTY(int maxPostLength READ maxPostLength CONSTANT)
    Q_PROPERTY(int maxNameLength READ maxNameLength CONSTANT)
    Q_PROPERTY(int maxDescriptionLength READ maxDescriptionLength CONSTANT)
    Q_PROPERTY(int maxPollOptions READ maxPollOptions CONSTANT)
    Q_PROPERTY(int maxChecklistItems READ maxChecklistItems CONSTANT)

public:
    ChannelsSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, QString webAppBase,
                    std::function<std::string()> tier, QObject* parent = nullptr);
    ~ChannelsSession() override;

    std::shared_ptr<api::ApiClient> api() const { return api_; }

    QVariantList following() const;
    int followingCount() const { return static_cast<int>(followed_.size()); }
    int unreadChannels() const { return unreadChannels_; }
    QVariantList discover() const { return discover_; }
    bool discoverLoaded() const { return discoverLoaded_; }
    bool discoverMore() const { return discoverMore_; }
    QString discoverQuery() const { return discoverQuery_; }
    QString discoverError() const { return discoverError_; }
    QVariantList mine() const { return mine_; }
    bool mineLoaded() const { return mineLoaded_; }
    bool canCreate() const;
    bool creating() const { return creating_; }
    bool pageShown() const { return pageShown_; }
    void setPageShown(bool shown);
    QString channelId() const { return QString::fromStdString(channelId_); }
    QVariantMap channel() const;
    ChannelPostModel* postModel() const { return posts_.get(); }
    bool hasOlder() const { return hasOlder_; }
    bool isMine() const { return isMine_; }
    bool isFollowing() const;
    bool isMuted() const;
    bool isAdmin() const;
    bool canManage() const { return isMine_ || isAdmin(); }
    bool busy() const { return busy_; }
    bool posting() const { return posting_; }
    QString error() const { return error_; }
    QString notice() const { return notice_; }
    QString composerError() const { return composerError_; }
    QVariantList scheduled() const;
    QVariantList suggestions() const;
    QVariantList mySuggestions() const;
    bool suggestionSent() const { return suggestionSent_; }
    QString suggestionError() const { return suggestionError_; }
    ChannelUploads* postUploads() const { return postUploads_.get(); }
    ChannelUploads* suggestionUploads() const { return suggestionUploads_.get(); }
    int maxPostLength() const { return api::channels::kMaxPostLength; }
    int maxNameLength() const { return api::channels::kMaxNameLength; }
    int maxDescriptionLength() const { return api::channels::kMaxDescriptionLength; }
    int maxPollOptions() const { return api::channels::kMaxPollOptions; }
    int maxChecklistItems() const { return api::channels::kMaxChecklistItems; }

    // --- The list ---
    /// Fetches every followed channel (for names, descriptions, photos and the unread dot).
    Q_INVOKABLE void refreshFollowing();
    /// A followed channel that was deleted (or made private without its new link here): off the list.
    Q_INVOKABLE void removeFollowed(const QString& id);
    Q_INVOKABLE void search(const QString& query);
    Q_INVOKABLE void loadMoreDiscover();
    Q_INVOKABLE void refreshMine();
    /// Opens the new channel once it's made; `created` says how it went.
    Q_INVOKABLE void create(const QString& name, const QString& description);
    /**
     * Checks every followed channel for new posts: the Channels badge, and a
     * notification for each newly posted one unless it's muted or open.
     */
    Q_INVOKABLE void checkFollowed();

    // --- One channel ---
    /// `secret`: a private channel's link secret, from a link just opened (otherwise a followed or own channel's is used).
    Q_INVOKABLE void openChannel(const QString& id, const QString& secret = QString());
    Q_INVOKABLE void closeChannel();
    Q_INVOKABLE void loadOlder();
    /// Follow or unfollow, on this computer only (a private channel's secret is kept with the follow).
    Q_INVOKABLE void toggleFollow();
    /// Notifications for this followed channel's new posts on or off.
    Q_INVOKABLE void toggleMuted();
    /// A text-only post now.
    Q_INVOKABLE void post(const QString& body);
    /**
     * The owner's composer: formatted text, plus one of the finished
     * uploads, a poll or quiz, or a checklist; now, or at `publishAt` (an ISO
     * time, empty for now). `extra`: {type: "" | "poll" | "quiz" | "checklist",
     * question, options, multiple, correct, explanation, title, items, collaborative}.
     */
    Q_INVOKABLE void submitPost(const QString& body, const QVariantMap& extra, const QString& publishAt);
    Q_INVOKABLE void deletePost(const QString& postId);
    Q_INVOKABLE void saveDetails(const QString& name, const QString& description);
    Q_INVOKABLE void deleteChannel();
    Q_INVOKABLE void report(const QString& reason);
    Q_INVOKABLE void copyLink();
    Q_INVOKABLE void clearMessages();
    /// Shows a line under the channel's header ("Forwarded.").
    Q_INVOKABLE void setNotice(const QString& notice);
    /**
     * The owner: private (a fresh private link; the old one stops working)
     * or public. Making it private again is how the link is reset.
     */
    Q_INVOKABLE void setPrivate(bool makePrivate);
    /// The owner: whether followers may suggest posts.
    Q_INVOKABLE void setAcceptSuggestions(bool accept);
    /// The owner: a picture file as the channel's photo (cut to a small square JPEG here).
    Q_INVOKABLE void setPhoto(const QUrl& imageFile);
    Q_INVOKABLE void removePhoto();

    // --- Polls and checklists ---
    /// Your vote (option indexes); an empty list takes a poll vote back. A quiz's answer is final.
    Q_INVOKABLE void vote(const QString& postId, const QVariantList& options);
    /// The owner: no more votes (a quiz's answer shows to everyone).
    Q_INVOKABLE void closePoll(const QString& postId);
    /// Ticks or unticks a checklist item (the owner, or anyone in a collaborative one).
    Q_INVOKABLE void tick(const QString& postId, int index);

    // --- Scheduled and suggested posts ---
    Q_INVOKABLE void publishScheduled(const QString& postId);
    Q_INVOKABLE void deleteScheduled(const QString& postId);
    /// The owner: publishes a suggestion with this text, now or at `publishAt` (ISO, empty for now).
    Q_INVOKABLE void approveSuggestion(const QString& suggestionId, const QString& body, const QString& publishAt);
    Q_INVOKABLE void rejectSuggestion(const QString& suggestionId);
    /// A follower: suggests a post (the text and the finished photo uploads).
    Q_INVOKABLE void sendSuggestion(const QString& body);
    Q_INVOKABLE void resetSuggestionForm();

    // --- Media ---
    /// Saves a post's file (or any of its media) to `destination`, streamed (files can be gigabytes).
    Q_INVOKABLE void saveMedia(const QString& url, const QUrl& destination);
    /// "3:05", or "1:02:03" for an hour or more.
    Q_INVOKABLE QString formatDuration(double seconds) const;

    /**
     * Posts seen on this computer, in a channel or as forwarded copies in a
     * chat: each is reported once, anonymously (post ids only, no cookie).
     * Best effort: a failed report is simply not counted.
     */
    Q_INVOKABLE void countViews(const QStringList& postIds);

    /// A channel link on this app's own site: clicked in a chat, it opens here rather than in a browser.
    Q_INVOKABLE bool isOwnChannelLink(const QString& text) const;
    /// Opens the channel a link points to and asks for the Channels page.
    Q_INVOKABLE void openChannelLink(const QString& text);
    /// Opens a channel by id and asks for the Channels page ("Forwarded from …" in a chat, a notification).
    Q_INVOKABLE void showChannel(const QString& id);

signals:
    void followingChanged();
    void unreadChannelsChanged();
    void discoverChanged();
    void mineChanged();
    void channelChanged();
    void postsChanged();
    void busyChanged();
    void messagesChanged();
    void composerChanged();
    void scheduledChanged();
    void suggestionsChanged();
    void pageShownChanged();
    /// Show the Channels page (a channel link was opened).
    void showRequested();
    /// A post went out (or was scheduled): the composer can clear.
    void posted();
    /// The channel's details were saved: the edit form can close.
    void detailsSaved();
    /// A suggestion was published or turned down: its review form can close.
    void suggestionDecided();
    /// Creating finished; `error` is empty when it worked.
    void created(const QString& error);
    /// A followed channel has a new post: notify (the channel's name, and the post's text or what it carries).
    void newPostNotification(const QString& channelId, const QString& channelName, const QString& preview);
    void sessionExpired();

private:
    struct OwnChannel {
        api::channels::Channel channel;
        /// A private channel's access token, from its secret unsealed with this account's key.
        std::optional<std::string> access;
    };

    void saveFollowed(model::channels::Followed list);
    /// Opening (or posting in) a followed channel counts its posts as seen, and keeps a newer private link's secret.
    void markSeen();
    void applyMine(std::vector<OwnChannel> list);
    void setBusy(bool busy);
    void setMessages(const QString& error, const QString& notice);
    void fail(const Failure& failure, const QString& fallback);
    void searchPage(int offset);
    /// A post from the composer or an approved suggestion: at the top, or with the scheduled ones.
    void added(api::channels::Post post, bool scheduled);
    void loadSuggestions();
    /// Followed channels changed: the badge is worked out again soon.
    void scheduleCheck();
    /// The access token for a secret (cached: rows ask for it often), or nullopt for none or a malformed one.
    std::optional<std::string> accessFor(const std::optional<std::string>& secret) const;
    /// The open channel's shareable link, if this computer can make it.
    std::optional<std::string> link() const;
    QVariantMap rowOf(const api::channels::Channel& c, const std::optional<std::string>& access) const;
    std::optional<model::channel_posts::RememberedVote> rememberedVote(const std::string& postId) const;

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    QString webAppBase_;
    std::function<std::string()> tier_;
    QThreadPool pool_;

    model::channels::Followed followed_;
    /// id -> the channel as last fetched; nullopt once it's known to be gone (or private without its link here).
    std::map<std::string, std::optional<api::channels::Channel>> fetched_;
    mutable std::map<std::string, std::string> accessCache_;
    int unreadChannels_ = 0;
    bool checking_ = false;
    bool checkAgain_ = false;
    QTimer checkTimer_;
    QTimer soonTimer_;
    bool pageShown_ = false;

    QVariantList discover_;
    bool discoverLoaded_ = false;
    bool discoverMore_ = false;
    QString discoverQuery_;
    QString discoverError_;
    int discoverGeneration_ = 0;

    QVariantList mine_;
    std::vector<std::string> mineIds_;
    bool mineLoaded_ = false;
    bool creating_ = false;

    std::string channelId_;
    int channelGeneration_ = 0;
    std::optional<api::channels::Channel> channel_;
    /// A private channel's link secret: from the link just opened, a followed channel, or the owner's own copy.
    std::optional<std::string> secret_;
    std::unique_ptr<ChannelPostModel> posts_;
    std::vector<api::channels::Post> scheduled_;
    std::vector<api::channels::Suggestion> suggestions_;
    std::vector<api::channels::Suggestion> mySuggestions_;
    bool suggestionSent_ = false;
    QString suggestionError_;
    std::unique_ptr<ChannelUploads> postUploads_;
    std::unique_ptr<ChannelUploads> suggestionUploads_;
    bool hasOlder_ = false;
    bool isMine_ = false;
    bool loadingOlder_ = false;
    bool busy_ = false;
    bool posting_ = false;
    QString error_;
    QString notice_;
    QString composerError_;
};

}  // namespace redvault::app
