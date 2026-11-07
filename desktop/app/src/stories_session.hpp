#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
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
#include "redvault/api/api_client.hpp"
#include "redvault/api/story_api.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/story_drafts.hpp"
#include "redvault/model/stories.hpp"

namespace redvault::app {

class ChatSession;

namespace stories_detail {
/// One opened story (defined in stories_session.cpp).
struct Decoded;
struct FeedResult;
struct PrefsResult;
/// What opening a story's stickers found: your answers and the published totals, or (your own story) the answers sealed to you.
struct InteractionsResult;
/// A friend as stories need them: who they are and the key their stories are sealed with.
struct Friend {
    std::string userId;
    std::string handle;
    std::optional<std::string> avatarUrl;
    crypto::Bytes publicKey;
};
/// A highlight as this account can see it.
struct Highlight {
    std::string id;
    std::string ownerId;
    std::string title;
    std::string coverStoryId;
    std::vector<std::shared_ptr<Decoded>> stories;
};
}  // namespace stories_detail

/**
 * Stories, the desktop counterpart of the web app's state/StoriesContext.tsx
 * and components/stories: photos, videos and text friends see for 24 hours,
 * end-to-end encrypted with a key per story (crypto/story_crypto.hpp). Every
 * plan can post. Highlights keep chosen stories on your profile; the archive
 * keeps them for you only.
 *
 * Decrypted photos live only in memory (the chat image cache); a video is
 * decrypted to a private temporary file for the player and deleted when the
 * viewer closes.
 */
class StoriesSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    /// Friends with stories, people with something new first: [{authorId, handle, avatarUrl, ring, unseen}]; ring is "new", "close" or "seen".
    Q_PROPERTY(QVariantList tray READ tray NOTIFY feedChanged)
    Q_PROPERTY(int ownCount READ ownCount NOTIFY feedChanged)
    Q_PROPERTY(bool mediaEnabled READ mediaEnabled NOTIFY feedChanged)
    /// Polls, quizzes, questions and sliders are on (the server says so).
    Q_PROPERTY(bool interactiveEnabled READ interactiveEnabled NOTIFY feedChanged)
    /// How long a story may stay up: [{seconds, label}] (the server's list; 24 hours alone on a server that predates this).
    Q_PROPERTY(QVariantList lifetimes READ lifetimes NOTIFY feedChanged)
    /// A story with something to answer stays up this long in the viewer (others: storySeconds).
    Q_PROPERTY(int stickerStorySeconds READ stickerStorySeconds CONSTANT)
    /// Your friends for the settings pickers: [{userId, handle, avatarUrl, close, hidden, muted}].
    Q_PROPERTY(QVariantList friends READ friends NOTIFY prefsChanged)
    Q_PROPERTY(bool shareViews READ shareViews NOTIFY prefsChanged)
    Q_PROPERTY(bool allowReplies READ allowReplies NOTIFY prefsChanged)
    Q_PROPERTY(bool archiveEnabled READ archiveEnabled NOTIFY prefsChanged)
    Q_PROPERTY(int closeFriendsCount READ closeFriendsCount NOTIFY prefsChanged)
    Q_PROPERTY(int audienceCount READ audienceCount NOTIFY prefsChanged)
    // --- The viewer ---
    Q_PROPERTY(bool viewerOpen READ viewerOpen NOTIFY viewerChanged)
    /// The line-up, fixed when it opens: [{owner: {userId, handle, avatarUrl, isSelf}, stories: [story]}].
    Q_PROPERTY(QVariantList viewerGroups READ viewerGroups NOTIFY viewerChanged)
    Q_PROPERTY(int viewerStartGroup READ viewerStartGroup NOTIFY viewerChanged)
    Q_PROPERTY(int viewerStartStory READ viewerStartStory NOTIFY viewerChanged)
    /// A highlight's or the archive's name, when that's what's open.
    Q_PROPERTY(QString viewerTitle READ viewerTitle NOTIFY viewerChanged)
    Q_PROPERTY(QString viewerHighlightId READ viewerHighlightId NOTIFY viewerChanged)
    // --- The composer ---
    Q_PROPERTY(bool composerOpen READ composerOpen NOTIFY composerChanged)
    /// "", "preparing", "encrypting" or "uploading".
    Q_PROPERTY(QString postStage READ postStage NOTIFY composerChanged)
    Q_PROPERTY(QString error READ error NOTIFY messagesChanged)
    Q_PROPERTY(QString toast READ toast NOTIFY messagesChanged)
    // --- Archive and highlights ---
    /// Your stories from today and the archive, newest first: [story].
    Q_PROPERTY(QVariantList library READ library NOTIFY libraryChanged)
    /// Your highlights: [{id, title, coverStoryId, stories: [story]}].
    Q_PROPERTY(QVariantList ownHighlights READ ownHighlights NOTIFY libraryChanged)
    /// Unfinished stories saved on this computer, newest first: [{id, preview, kind, stickerCount, updatedAt (ms)}].
    Q_PROPERTY(QVariantList drafts READ drafts NOTIFY draftsChanged)
    Q_PROPERTY(QStringList colors READ colors CONSTANT)
    Q_PROPERTY(QStringList backgrounds READ backgrounds CONSTANT)
    Q_PROPERTY(QStringList fonts READ fonts CONSTANT)
    Q_PROPERTY(int storySeconds READ storySeconds CONSTANT)

public:
    StoriesSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, ChatSession* chats,
                   QObject* parent = nullptr);
    ~StoriesSession() override;

    QVariantList tray() const;
    int ownCount() const { return static_cast<int>(own_.size()); }
    bool mediaEnabled() const { return mediaEnabled_; }
    bool interactiveEnabled() const { return interactiveEnabled_; }
    QVariantList lifetimes() const;
    int stickerStorySeconds() const { return model::stories::kStickerStorySeconds; }
    QVariantList friends() const;
    bool shareViews() const { return prefs_.shareViews; }
    bool allowReplies() const { return prefs_.allowReplies; }
    bool archiveEnabled() const { return archiveEnabled_; }
    int closeFriendsCount() const;
    int audienceCount() const;
    bool viewerOpen() const { return viewerOpen_; }
    QVariantList viewerGroups() const { return viewerGroups_; }
    int viewerStartGroup() const { return viewerStartGroup_; }
    int viewerStartStory() const { return viewerStartStory_; }
    QString viewerTitle() const { return viewerTitle_; }
    QString viewerHighlightId() const { return viewerHighlightId_; }
    bool composerOpen() const { return composerOpen_; }
    QString postStage() const { return postStage_; }
    QString error() const { return error_; }
    QString toast() const { return toast_; }
    QVariantList library() const { return library_; }
    QVariantList ownHighlights() const { return ownHighlights_; }
    QVariantList drafts() const { return drafts_; }
    QStringList colors() const;
    QStringList backgrounds() const;
    QStringList fonts() const;
    int storySeconds() const { return model::stories::kStorySeconds; }

    /// The feed, friends and (every 5 minutes) keys for friends added since you posted.
    Q_INVOKABLE void refresh();

    // --- Viewing ---
    /// From the tray: their stories, then on to the next person's. "me" opens your own.
    Q_INVOKABLE void openAuthor(const QString& authorId);
    /// A highlight (from loadHighlights) or your library, starting at `index`.
    Q_INVOKABLE void openHighlight(const QString& ownerId, const QString& highlightId);
    Q_INVOKABLE void openLibrary(int index);
    Q_INVOKABLE void closeViewer();
    /// Fetches and decrypts a story's photo or video; answers with mediaReady or mediaFailed.
    Q_INVOKABLE void loadMedia(const QString& storyId);
    /// Rings go grey; the author learns you saw it only if you share views.
    Q_INVOKABLE void markSeen(const QString& storyId);
    /// A reply (or quick reaction) as a message in your chat with the author.
    Q_INVOKABLE void reply(const QString& storyId, const QString& text, bool reaction);
    Q_INVOKABLE bool canReply(const QString& storyId) const;
    Q_INVOKABLE void deleteStory(const QString& storyId);
    /// Who shared that they saw your story; answers with viewersLoaded.
    Q_INVOKABLE void loadViewers(const QString& storyId);
    Q_INVOKABLE bool isMuted(const QString& userId) const;
    Q_INVOKABLE void setMuted(const QString& userId, bool muted);
    Q_INVOKABLE void clearMessages();

    // --- Stickers (docs/stories-advanced/) ---
    /**
     * Checks a sticker the composer built, the way every reader will: {ok, error, sticker}. `candidate` has the fields
     * of a sticker map (type, text, options, correct, ...); the position fields default to the middle. A link is
     * spelled the way the web app spells it, a countdown must be in the future.
     */
    Q_INVOKABLE QVariantMap checkSticker(const QVariantMap& candidate) const;
    /// Your answers and the published totals for a story (yours: the answers sealed to you, counted, and totals published for friends); answers with interactionsLoaded.
    Q_INVOKABLE void loadInteractions(const QString& storyId);
    /// Answers a poll, quiz, question or slider: {choice} / {value} / {text}. Answers with answered or a message.
    Q_INVOKABLE void answer(const QString& storyId, const QString& stickerId, const QVariantMap& answer);
    /// The words on a story a reporter may choose to include in a report (never a photo or video).
    Q_INVOKABLE QString storyEvidence(const QString& storyId) const;
    Q_INVOKABLE void reportStory(const QString& storyId, const QString& reason, const QString& details, bool includeWords);
    /// The author reports an abusive answer, by its sealed envelope (admins see who sent it).
    Q_INVOKABLE void reportAnswer(const QString& storyId, const QString& stickerId, const QString& envelope, const QString& reason,
                                  const QString& answerText, bool includeText);

    // --- Posting ---
    Q_INVOKABLE void openComposer();
    Q_INVOKABLE void closeComposer();
    /// How many friends would see a story: `audience` is "friends", "close_friends" or "custom" (the friends in `customIds`).
    Q_INVOKABLE int recipientCount(const QString& audience, const QStringList& customIds) const;
    /**
     * `overlays`: [{type, text, x, y, scale, rotation, color, boxed, font}]. `stickers`: sticker maps from checkSticker.
     * `audience` is "friends", "close_friends" or "custom" (the friends in `customIds`, no one else); `ttlSeconds` how long it stays up.
     */
    Q_INVOKABLE void postText(const QString& text, const QString& background, const QString& font, const QVariantList& overlays,
                              const QVariantList& stickers, const QString& audience, const QStringList& customIds, bool allowReplies,
                              int ttlSeconds);
    /// A photo (re-encoded here, which leaves its metadata behind) or a video (as it is, up to 60 s / 60 MB; `durationMs` from the player).
    Q_INVOKABLE void postMedia(const QUrl& file, const QVariantList& overlays, const QVariantList& stickers, const QString& audience,
                               const QStringList& customIds, bool allowReplies, int ttlSeconds, int durationMs);
    /// Checks a picked file before the composer shows it: {kind: "image"|"video", error}.
    Q_INVOKABLE QVariantMap checkFile(const QUrl& file) const;

    // --- Drafts: unfinished stories, sealed on this computer and never sent anywhere ---
    /// Reads the list again (answers with draftsChanged).
    Q_INVOKABLE void refreshDrafts();
    /**
     * Saves what the composer holds (`state`: mode, text, background, font, overlays, stickers, audience, customIds,
     * allowReplies, ttlSeconds, mediaFile, mediaKind), over `draftId` when it is already a draft. A photo is redrawn
     * first (leaving its metadata behind), a video is kept as it is. Answers with draftSaved.
     */
    Q_INVOKABLE void saveDraft(const QString& draftId, const QVariantMap& state, int durationMs);
    /// Opens a draft for editing; answers with draftLoaded (a photo or video as a private temporary file).
    Q_INVOKABLE void openDraft(const QString& draftId);
    Q_INVOKABLE void deleteDraft(const QString& draftId);

    // --- Settings, archive and highlights ---
    Q_INVOKABLE void setCloseFriends(const QStringList& userIds);
    Q_INVOKABLE void setHideFrom(const QStringList& userIds);
    Q_INVOKABLE void setShareViews(bool on);
    Q_INVOKABLE void setAllowReplies(bool on);
    Q_INVOKABLE void setArchive(bool on);
    Q_INVOKABLE void loadLibrary();
    /// Someone's highlights as you can see them; answers with highlightsLoaded.
    Q_INVOKABLE void loadHighlights(const QString& userId);
    /// A new highlight (`highlightId` empty) or a changed one: the first of `storyIds` is the cover.
    Q_INVOKABLE void saveHighlight(const QString& highlightId, const QString& title, const QStringList& storyIds);
    Q_INVOKABLE void addToHighlight(const QString& highlightId, const QString& storyId);
    Q_INVOKABLE void removeFromHighlight(const QString& highlightId, const QString& storyId);
    Q_INVOKABLE void deleteHighlight(const QString& highlightId);

signals:
    void feedChanged();
    void prefsChanged();
    void viewerChanged();
    void composerChanged();
    void messagesChanged();
    void libraryChanged();
    void mediaReady(const QString& storyId, const QString& url);
    void mediaFailed(const QString& storyId);
    void viewersLoaded(const QString& storyId, const QVariantList& viewers);
    void draftsChanged();
    /// A draft was saved (a new one or over `id`).
    void draftSaved(const QString& id);
    /// {id, mode, text, background, font, overlays, stickers, audience, customIds, allowReplies, ttlSeconds, mediaFile, mediaKind, durationMs}.
    void draftLoaded(const QVariantMap& draft);
    /**
     * A story's stickers were opened. {own, mine: {stickerId: {answered, readable, anonymous, kind, choice, value, text}},
     * totals: {stickerId: {total, counts, hasCorrect, correct, hasAverage, average}}, entries: [{stickerId, sender, text, atMs,
     * anonymous, envelope}] (yours: written answers), unreadable}.
     */
    void interactionsLoaded(const QString& storyId, const QVariantMap& result);
    /// Your answer went through: {answered, readable, anonymous, kind, choice, value, text}.
    void answered(const QString& storyId, const QString& stickerId, const QVariantMap& mine);
    /// A report was sent or refused; `message` is worded for the person.
    void reportDone(bool ok, const QString& message);
    void highlightsLoaded(const QString& userId, const QVariantList& highlights);
    /// A story went up: the composer closes.
    void posted();
    void sessionExpired();

private:
    using Decoded = stories_detail::Decoded;
    using Friend = stories_detail::Friend;
    using FeedResult = stories_detail::FeedResult;
    using PrefsResult = stories_detail::PrefsResult;
    using InteractionsResult = stories_detail::InteractionsResult;

    std::shared_ptr<Decoded> find(const std::string& storyId) const;
    QVariantMap storyMap(const Decoded& d) const;
    QVariantMap stickerMap(const model::stories::Sticker& sticker) const;
    /// Opens a story's stickers in the background: its answers and totals (yours: counts and publishes them).
    void startInteractions(const std::shared_ptr<Decoded>& story);
    /// Your own stories with stickers whose totals friends see: counted and published now and then while the app is open.
    void syncOwnTotals();
    QVariantMap ownerMap(const std::string& userId) const;
    void applyFeed(FeedResult& result);
    QVariantMap highlightMap(const stories_detail::Highlight& h) const;
    void openList(const std::string& ownerId, const std::vector<std::shared_ptr<Decoded>>& stories, int index, const QString& title,
                  const QString& highlightId);
    void savePrefs(std::function<model::stories::Prefs(model::stories::Prefs)> change, std::optional<bool> archive);
    /// Encrypts and posts on the pool: `media` is the photo or video bytes (empty for text).
    void publish(model::stories::Meta meta, std::string media, const std::string& audience, const std::vector<std::string>& customIds,
                 bool allowReplies, int ttlSeconds);
    /// Stickers from the composer, checked again the way readers will; nullopt (with the error shown) if any doesn't fit.
    std::optional<std::vector<model::stories::Sticker>> stickersFrom(const QVariantList& list);
    void setStage(const QString& stage);
    void setMessages(const QString& error, const QString& toast);
    void fail(const Failure& failure, const QString& fallback);
    void dropTempFiles();
    void dropDraftFiles();

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    ChatSession* chats_;
    QThreadPool pool_;

    bool mediaEnabled_ = false;
    bool interactiveEnabled_ = false;
    /// The server's allowed lifetimes, in seconds.
    std::vector<int> ttlChoices_;
    /// `storyId:stickerId` -> the totals last published, so only what changed is sent.
    std::map<std::string, std::string> publishedTotals_;
    /// When each of your stories' totals were last counted in the background.
    std::map<std::string, std::int64_t> lastTotalsMs_;
    std::vector<std::shared_ptr<Decoded>> own_;
    /// Tray order: [(authorId, stories)].
    std::vector<std::pair<std::string, std::vector<std::shared_ptr<Decoded>>>> authors_;
    /// Every story opened this session (feed, highlights, archive), for loading media and replying.
    std::map<std::string, std::shared_ptr<Decoded>> known_;
    std::map<std::string, Friend> friends_;
    model::stories::Prefs prefs_;
    bool archiveEnabled_ = false;
    int prefsRevision_ = 0;
    std::int64_t lastGapFillMs_ = 0;
    int feedGeneration_ = 0;

    bool viewerOpen_ = false;
    QVariantList viewerGroups_;
    int viewerStartGroup_ = 0;
    int viewerStartStory_ = 0;
    QString viewerTitle_;
    QString viewerHighlightId_;
    bool composerOpen_ = false;
    QString postStage_;
    QString error_;
    QString toast_;
    QVariantList library_;
    std::vector<std::shared_ptr<Decoded>> libraryStories_;
    QVariantList ownHighlights_;
    std::map<std::string, stories_detail::Highlight> highlights_;
    /// Decrypted videos on disk while the viewer is open.
    QStringList tempFiles_;
    std::unique_ptr<data::StoryDraftStore> draftStore_;
    QVariantList drafts_;
    /// A draft's photo or video, decrypted for the composer, while it is open.
    QStringList draftFiles_;
};

}  // namespace redvault::app
