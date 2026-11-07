#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "async.hpp"
#include "chat_models.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/data/outbox.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/community_keys.hpp"
#include "redvault/data/conversation_repository.hpp"
#include "redvault/data/file_repository.hpp"
#include "redvault/data/friend_repository.hpp"

namespace redvault::app {

class TorController;

/**
 * The signed-in chat state: the desktop counterpart of Android's
 * ui/chats/ChatSession.kt. Polls the chat list every 5 s and the open chat
 * every 3 s while it exists, decrypting off the UI thread, and marks what's
 * new as seen. Also files (open, save, attach), group management and
 * safety-number verification for the open chat.
 *
 * Conversation keys live in the (shared, read-only) metas and are wiped
 * when the last reference goes, at the latest when this is destroyed on
 * sign-out. Decrypted files live under a per-session cache folder that is
 * deleted then too.
 */
/// What a chat pinned to device keys can't carry, in words. Protocol 3's coverage: SECURITY_DESIGN_AND_MIGRATION.md, H-3.
namespace device_key_refusal {
inline constexpr const char* kMessage = "This chat uses device keys, so nothing was sent. Send it from the chat itself.";
inline constexpr const char* kPoll = "This chat uses device keys, which can't carry polls yet. Nothing was sent.";
inline constexpr const char* kVote = "This chat uses device keys, which can't carry poll votes yet. Nothing was sent.";
inline constexpr const char* kForward = "This chat uses device keys, which can't carry forwarded messages yet. Nothing was sent.";
inline constexpr const char* kStoryReply = "This chat uses device keys, which can't carry story replies yet. Nothing was sent.";
inline constexpr const char* kPin = "This chat uses device keys, which can't carry pins yet. Nothing was sent.";
inline constexpr const char* kChange = "This message was sent before the chat moved to device keys, so it can't be changed or reacted to now. Nothing was sent.";
}  // namespace device_key_refusal

class ChatSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    Q_PROPERTY(redvault::app::ChatListModel* chatList READ chatList CONSTANT)
    Q_PROPERTY(redvault::app::MessageModel* messages READ messages CONSTANT)
    Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString currentChatId READ currentChatId NOTIFY currentChatChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY currentChatChanged)
    Q_PROPERTY(QString currentSubtitle READ currentSubtitle NOTIFY currentChatChanged)
    Q_PROPERTY(bool currentIsGroup READ currentIsGroup NOTIFY currentChatChanged)
    Q_PROPERTY(QString currentMyRole READ currentMyRole NOTIFY currentChatChanged)
    Q_PROPERTY(QVariantList currentMembers READ currentMembers NOTIFY currentChatChanged)
    Q_PROPERTY(QString currentSafetyNumber READ currentSafetyNumber NOTIFY currentChatChanged)
    Q_PROPERTY(QString currentSafetyQr READ currentSafetyQr NOTIFY currentChatChanged)
    Q_PROPERTY(QString currentAvatarUrl READ currentAvatarUrl NOTIFY currentChatChanged)
    Q_PROPERTY(bool currentVerified READ currentVerified NOTIFY currentChatChanged)
    /// The other person in the open DM (empty for a group).
    Q_PROPERTY(QString currentUserId READ currentUserId NOTIFY currentChatChanged)
    /// The open DM's contact has a different key from the one first seen here, and it isn't verified yet.
    Q_PROPERTY(bool currentKeyChanged READ currentKeyChanged NOTIFY currentChatChanged)
    /// Why the open chat's key was refused (data/key_trust.hpp), empty when it wasn't. Nothing can be sent there.
    Q_PROPERTY(QString currentKeyProblem READ currentKeyProblem NOTIFY keyProblemChanged)
    /// Files in the open chat, newest first.
    Q_PROPERTY(QVariantList currentFiles READ currentFiles NOTIFY currentChatChanged)
    /// Your disappearing-messages timer in the open chat, in seconds (0 = off). It governs only what you send.
    Q_PROPERTY(int currentDisappearing READ currentDisappearing NOTIFY currentChatChanged)
    Q_PROPERTY(QString currentDisappearingLabel READ currentDisappearingLabel NOTIFY currentChatChanged)
    /// [{seconds, label}] for the timer picker, the same choices as the web and Android apps.
    Q_PROPERTY(QVariantList disappearingOptions READ disappearingOptions CONSTANT)
    /// Everyone you have a 1:1 chat with: {chatId, userId, handle, initial, avatarUrl, verified}.
    Q_PROPERTY(QVariantList contacts READ contacts NOTIFY contactsChanged)
    /// userIds, kept on this computer only.
    Q_PROPERTY(QStringList favorites READ favorites NOTIFY localPrefsChanged)
    Q_PROPERTY(QStringList muted READ muted NOTIFY localPrefsChanged)
    Q_PROPERTY(bool sending READ sending NOTIFY sendingChanged)
    Q_PROPERTY(QString fileActivity READ fileActivity NOTIFY fileActivityChanged)
    Q_PROPERTY(bool groupBusy READ groupBusy NOTIFY groupBusyChanged)
    /// The open chat has older messages than it shows; loadOlder() fetches them.
    Q_PROPERTY(bool hasOlder READ hasOlder NOTIFY historyChanged)
    Q_PROPERTY(bool loadingOlder READ loadingOlder NOTIFY historyChanged)
    /// Unread messages across every chat that isn't muted, for the nav rail badge.
    Q_PROPERTY(int unreadTotal READ unreadTotal NOTIFY unreadTotalChanged)
    /// Chats someone who isn't your friend started: [{chatId, userId, handle, initial, avatarUrl, messages, preview, time}].
    /// Shown apart and read without sending receipts until accepted.
    Q_PROPERTY(QVariantList messageRequests READ messageRequests NOTIFY messageRequestsChanged)
    /// The open chat's pinned messages, newest pin first: [{messageId, preview, by}].
    Q_PROPERTY(QVariantList currentPinned READ currentPinned NOTIFY currentChatChanged)
    /// Goes up whenever a picture finishes loading (views re-read imageInfo).
    Q_PROPERTY(int imagesRevision READ imagesRevision NOTIFY imagesChanged)
    /// Messages and files in the Tor outbox, waiting for Tor (docs/tor-routing-plan.md).
    Q_PROPERTY(int queuedCount READ queuedCount NOTIFY queuedChanged)
    /// Of those, how many the server refused for good (they wait for Retry or Discard).
    Q_PROPERTY(int heldCount READ heldCount NOTIFY queuedChanged)
    /// The open chat is a paid or free community: its id (empty otherwise). Its page has its members and settings.
    Q_PROPERTY(QString currentCommunityId READ currentCommunityId NOTIFY currentChatChanged)
    /// "channel" or "group" for a community, empty otherwise.
    Q_PROPERTY(QString currentCommunityKind READ currentCommunityKind NOTIFY currentChatChanged)
    /// The open community's picture path (for the "community" image provider), or empty.
    Q_PROPERTY(QString currentCommunityImage READ currentCommunityImage NOTIFY currentChatChanged)
    /// False for a channel's subscribers: they read, only the creator and staff post.
    Q_PROPERTY(bool currentCanPost READ currentCanPost NOTIFY currentChatChanged)

public:
    /// `tor`: the app's Tor gateway when routing goes through it; sends then always pass through the sealed outbox.
    ChatSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                TorController* tor = nullptr, QObject* parent = nullptr);
    ~ChatSession() override;

    ChatListModel* chatList() { return &chatList_; }
    MessageModel* messages() { return &messages_; }
    bool loaded() const { return loaded_; }
    QString error() const { return error_; }
    QString currentChatId() const { return QString::fromStdString(currentChatId_); }
    QString currentTitle() const;
    QString currentSubtitle() const;
    bool currentIsGroup() const;
    QString currentMyRole() const;
    QVariantList currentMembers() const;
    QString currentSafetyNumber() const;
    /// What the verification QR carries (the same digits, in the format Android scans).
    QString currentSafetyQr() const;
    QString currentAvatarUrl() const;
    bool currentVerified() const;
    bool sending() const { return sending_; }
    QString fileActivity() const { return fileActivity_; }
    bool groupBusy() const { return groupBusy_; }
    bool hasOlder() const { return hasOlder_; }
    bool loadingOlder() const { return loadingOlder_; }
    int unreadTotal() const { return unreadTotal_; }
    QVariantList messageRequests() const;
    QVariantList currentPinned() const;
    Q_INVOKABLE bool isPinned(const QString& messageId) const;
    /// Pins a message for everyone in the chat (at most three stay pinned; the oldest goes).
    Q_INVOKABLE void pin(const QString& messageId);
    Q_INVOKABLE void unpin(const QString& messageId);
    /// A poll in the open chat: 2-10 options. `pollDone` reports a problem (empty on success).
    Q_INVOKABLE void createPoll(const QString& question, const QStringList& options, bool multiple);
    /// Your vote (option indexes); it replaces an earlier one, and an empty list takes it back.
    Q_INVOKABLE void vote(const QString& pollId, const QVariantList& options);
    /// Only the poll's creator can end it; votes after that don't count.
    Q_INVOKABLE void endPoll(const QString& pollId);
    int imagesRevision() const { return imagesRevision_; }
    QString currentCommunityId() const;
    QString currentCommunityKind() const;
    QString currentCommunityImage() const;
    bool currentCanPost() const;
    /// Who sent a message in the open chat (for a report), or empty.
    Q_INVOKABLE QString senderOf(const QString& messageId) const;
    /// A picture in the open chat: {state: ""|"loading"|"ready"|"failed"|"large", width, height} (thumbnail size).
    Q_INVOKABLE QVariantMap imageInfo(const QString& messageId) const;
    /**
     * Downloads, decrypts and decodes a picture for showing inline, in
     * memory only. Pictures over 20 MB wait for `force` (the user asked).
     */
    Q_INVOKABLE void loadImage(const QString& messageId, bool force = false);
    /// Accept: the chat joins your list and opens. Ignore: it's hidden (the sender isn't told).
    Q_INVOKABLE void respondToRequest(const QString& chatId, bool accept);

    Q_INVOKABLE void openChat(const QString& chatId);
    Q_INVOKABLE void closeChat();
    /// Shows another page of the open chat's history, above what's there.
    Q_INVOKABLE void loadOlder();
    /// Sends to the open chat; `replyToId` (optional) quotes one of its messages.
    /// `entitlementId` / `safetyContact`: to someone who charges for messages (EarningsSession supplies them).
    Q_INVOKABLE void sendMessage(const QString& text, const QString& replyToId = {}, const QString& entitlementId = {}, bool safetyContact = false);
    Q_INVOKABLE void react(const QString& messageId, const QString& emoji);
    /// Your own text message, sent within the last 24 hours (Signal's window), and not a file.
    Q_INVOKABLE bool canEdit(const QString& messageId) const;
    /// The newest message you could edit (for the Up arrow in an empty composer), or empty.
    Q_INVOKABLE QString lastEditableId() const;
    /// Sends a new version of one of your messages; `edited` reports how it went.
    Q_INVOKABLE void editMessage(const QString& messageId, const QString& text);
    /// Every version of an edited message, oldest first: [{text, time}], the current one last.
    Q_INVOKABLE QVariantList editHistory(const QString& messageId) const;
    /// Chats a message can be forwarded to: [{chatId, title, initial, isGroup, avatarUrl, userId}], by recent activity.
    Q_INVOKABLE QVariantList forwardTargets() const;
    /**
     * Forwards messages of the open chat (oldest first) to other chats: text
     * as a new message marked "Forwarded", files re-encrypted and uploaded
     * again (sending files needs Pro). `forwardDone` reports the outcome.
     */
    Q_INVOKABLE void forwardMessages(const QStringList& messageIds, const QStringList& chatIds);
    /**
     * A channel post into chats, end-to-end encrypted like any message, as a
     * post from that channel (each copy that's seen counts a view of it).
     * `forwardDone` reports the outcome.
     */
    Q_INVOKABLE void forwardChannelPost(const QString& text, const QString& channelId, const QString& postId, const QString& channelName,
                                        const QStringList& chatIds);
    /// Hides a message on this computer only; the others keep it.
    Q_INVOKABLE void deleteForMe(const QString& messageId);
    /// Deletes your own message for everyone: the server erases its ciphertext (and a file's upload).
    Q_INVOKABLE void deleteForEveryone(const QString& messageId);
    // --- Several selected messages at once ---
    Q_INVOKABLE void deleteManyForMe(const QStringList& messageIds);
    /// Only your own messages are deleted; anything else in the list is skipped.
    Q_INVOKABLE void deleteManyForEveryone(const QStringList& messageIds);
    /// Every message in the list is yours (so "delete for everyone" applies to all of them).
    Q_INVOKABLE bool allFromMe(const QStringList& messageIds) const;
    Q_INVOKABLE bool anyFile(const QStringList& messageIds) const;
    /// The readable ones (undecryptable messages can't be forwarded).
    Q_INVOKABLE QStringList forwardable(const QStringList& messageIds) const;
    Q_INVOKABLE void refreshNow();
    /// Unsent composer text for a chat, kept in memory only.
    Q_INVOKABLE void setDraft(const QString& chatId, const QString& text) { chatList_.setDraft(chatId, text); }
    Q_INVOKABLE QString draftFor(const QString& chatId) const { return chatList_.draft(chatId); }
    /// A message's full text as shown (for editing, copying, forwarding).
    Q_INVOKABLE QString textOf(const QString& messageId) const;
    /// A short quote of a message in the open chat, for the reply bar.
    Q_INVOKABLE QString previewOf(const QString& messageId) const;
    Q_INVOKABLE void clearError();

    // --- Files ---
    /// Encrypts, uploads and sends a local file to the open chat.
    Q_INVOKABLE void attachFile(const QUrl& file);
    /**
     * What a paste would attach: copied files (file URLs), or a copied
     * picture (a screenshot, say) written to a temporary PNG that's deleted
     * once sent or discarded. Empty when the clipboard holds plain text.
     */
    Q_INVOKABLE QStringList pastedAttachments();
    /// Deletes a temporary pasted picture that won't be sent after all.
    Q_INVOKABLE void discardPasted(const QString& fileUrl);
    /// For the send confirmation: {name, size, image} of a local file.
    Q_INVOKABLE QVariantMap localFileInfo(const QString& fileUrl) const;
    /// Decrypts a received file into the session cache and opens it with its default app (not for programs).
    Q_INVOKABLE void openFile(const QString& messageId);
    /// Decrypts a received file and saves it where the user chose.
    Q_INVOKABLE void saveFile(const QString& messageId, const QUrl& destination);
    /// Whether a file may be opened directly (only inert types; anything else can only be saved).
    Q_INVOKABLE bool canOpenFile(const QString& messageId) const;

    // --- Groups (the open chat, except createGroup) ---
    /// `memberIds`: friends' user ids. Opens the new group when it's ready.
    Q_INVOKABLE void createGroup(const QString& name, const QStringList& memberIds);
    /**
     * Opens a 1:1 chat with someone who isn't a friend: a message request on
     * their side. For creators who charge for messages (the server's paywall
     * decides what sending costs). An existing chat with them just opens.
     * Answers with groupActionDone.
     */
    Q_INVOKABLE void startMessageRequest(const QString& userId);
    Q_INVOKABLE void addMembers(const QStringList& userIds);
    Q_INVOKABLE void removeMember(const QString& userId);
    Q_INVOKABLE void setMemberRole(const QString& userId, const QString& role);
    Q_INVOKABLE void renameGroup(const QString& name);
    Q_INVOKABLE void leaveGroup();

    // --- Safety number (the open DM) ---
    /// Remembered on this device only, for the contact's current key.
    Q_INVOKABLE void setCurrentVerified(bool verified);

    QString currentUserId() const;
    bool currentKeyChanged() const;
    QString currentKeyProblem() const;
    /**
     * Why nothing can go out in this chat right now, or empty: its key was refused, a contact's key changed, or the chat
     * is pinned to device keys and `onDeviceKeys` says why this can't go on them. Pinned chats never fall back unasked.
     */
    QString sendRefusal(const std::string& chatId, const char* onDeviceKeys) const;
    /// Set once by the app: whether a chat is pinned to device keys. Such a chat never sends through this class.
    void setDeviceKeyCheck(std::function<bool(const std::string& chatId)> pinned) { chatPinnedToDeviceKeys_ = std::move(pinned); }
    std::shared_ptr<data::GroupRepository> groupRepository() const { return groups_; }
    QVariantList currentFiles() const { return messages_.files(); }
    int currentDisappearing() const;
    QString currentDisappearingLabel() const;
    QVariantList disappearingOptions() const;
    /// Sets your timer for the open chat (0 turns it off). Messages already sent keep theirs.
    Q_INVOKABLE void setDisappearing(int seconds);
    QVariantList contacts() const;
    QStringList favorites() const;
    QStringList muted() const;
    Q_INVOKABLE void toggleFavorite(const QString& userId);
    /// Muted contacts don't notify or ring.
    Q_INVOKABLE void toggleMuted(const QString& userId);
    /// The 1:1 chat with this person, or empty.
    Q_INVOKABLE QString chatIdForUser(const QString& userId) const;
    Q_INVOKABLE bool isVerifiedUser(const QString& userId) const;

    /// Any loaded chat's details (null until the chat list has it).
    std::shared_ptr<const data::ConversationMeta> metaFor(const std::string& chatId) const;
    /// For work that sends on its own thread (a group call's invite message).
    std::shared_ptr<data::ConversationRepository> repository() const { return repository_; }
    /// The community epoch keys this session holds (shared with the Communities screens' key upkeep).
    std::shared_ptr<data::CommunityKeyStore> communityKeys() const { return communityKeys_; }
    /// Every loaded chat's details.
    std::vector<std::shared_ptr<const data::ConversationMeta>> metas() const;
    /// Opens a chat as soon as the chat list has it (a group just joined by link), or straight away if it does.
    void openWhenListed(const std::string& chatId);
    /// A group's members changed elsewhere (a request to join approved): re-resolve it now.
    void groupChanged(const std::string& chatId);

    int queuedCount() const { return static_cast<int>(queued_.size()); }
    int heldCount() const;
    /// A queued message the person gave up on: deleted from the outbox (it never reached the server).
    Q_INVOKABLE void discardQueued(const QString& id);
    /// A held message: try it again.
    Q_INVOKABLE void retryQueued(const QString& id);
    /// Sends what's queued, oldest first, if Tor is connected. Called when Tor connects and on a timer.
    void drainOutbox();

signals:
    void queuedChanged();
    /// A send to someone who charges for messages needs a paid message first.
    void paymentRequired(const QString& chatId);
    void loadedChanged();
    void errorChanged();
    void currentChatChanged();
    void sendingChanged();
    void fileActivityChanged();
    void groupBusyChanged();
    void contactsChanged();
    void localPrefsChanged();
    void historyChanged();
    void unreadTotalChanged();
    void messageRequestsChanged();
    void imagesChanged();
    /// A send finished; `ok` false means the text should stay in the composer.
    void sent(bool ok);
    void keyProblemChanged();
    /// An edit finished; `ok` false means the text should stay in the composer.
    void edited(bool ok);
    /// A forward finished: how many messages went out, and the first problem if any.
    void forwardDone(int sent, const QString& error);
    /// A poll was created (`error` empty) or couldn't be.
    void pollDone(const QString& error);
    /// A file was saved (`path` is where), or opening/saving failed (`error`).
    void fileSaved(const QString& path);
    /// A group action finished; `error` is empty on success.
    void groupActionDone(const QString& error);
    /// New messages from someone else arrived in a chat (after the first load).
    void incomingMessages(const QString& chatId, const QString& title, int count, bool isCurrentChat);
    /// The server says the session is over (401): the app signs out.
    void sessionExpired();

private:
    struct RefreshResult {
        std::vector<ChatSummary> chats;
        std::map<std::string, std::shared_ptr<const data::ConversationMeta>> metas;
        std::vector<ChatSummary> requests;
    };

    void refreshAll();
    void refreshThread();
    void applyThread(const std::string& chatId, int window, data::MessagesResult result);
    /// Sends a small control message to the open chat (pin, vote...), then refreshes it.
    template <typename Work>
    void sendControl(const char* onDeviceKeys, Work work);
    void setHistory(bool hasOlder, bool loadingOlder);
    /// Takes a message out of the open chat's view and its chat-list row right away.
    void removeLocally(const std::string& messageId);
    void updateUnreadTotal();
    void fail(const Failure& failure);
    void setError(const QString& error);
    /// sendRefusal, shown, and answered with `sent(false)`. True: nothing may go out.
    bool refuseSend(const std::string& chatId, const char* onDeviceKeys);
    bool contactKeyChanged(const data::ConversationMeta& meta) const;
    std::function<bool(const std::string&)> chatPinnedToDeviceKeys_;
    QString shownKeyProblem_;
    void setFileActivity(const QString& activity);
    void setGroupBusy(bool busy);
    /// Runs a group change, then re-resolves the chat's members and name.
    template <typename Work>
    void groupAction(Work work, const std::string& openAfter = {});
    std::vector<data::Message> withKnownSecurity(std::vector<data::Message> messages) const;
    /**
     * Emits incomingMessages for messages from others not seen before (the
     * first load only records them). Messages older than `historyBefore` are
     * history just scrolled back to, recorded without notifying.
     */
    void noticeIncoming(const ChatSummary& chat, std::int64_t historyBefore = 0);
    const data::ConversationMeta* currentMeta() const;
    std::shared_ptr<const data::ConversationMeta> currentMetaShared() const;

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    /// Group key epochs and message signatures for this session (docs/group-keys-and-signatures.md).
    std::shared_ptr<data::MessageKeys> keys_;
    // Before repository_, which reads communities with it.
    std::shared_ptr<data::CommunityKeyStore> communityKeys_;
    std::shared_ptr<data::ConversationRepository> repository_;
    std::shared_ptr<data::GroupRepository> groups_;
    std::filesystem::path cacheDir_;
    std::shared_ptr<data::FileRepository> files_;
    QThreadPool pool_;
    QTimer listTimer_;
    QTimer threadTimer_;

    ChatListModel chatList_;
    MessageModel messages_;
    std::map<std::string, std::shared_ptr<const data::ConversationMeta>> metas_;
    std::vector<ChatSummary> requests_;
    // Picture loading by message id: "loading", "ready", "failed" (the images live in ChatImageCache).
    std::map<std::string, QString> imageStates_;
    int imagesRevision_ = 0;
    // Survives refreshes, which rebuild messages from the server and would
    // otherwise drop the verification computed for messages sent this session.
    std::map<std::string, data::MessageSecurity> securityById_;
    std::string currentChatId_;
    std::string pendingOpenChatId_;
    // Chats whose members or name just changed: re-resolved on the next refresh, shown as-is until then.
    std::set<std::string> staleMetas_;
    std::set<std::string> knownIncoming_;
    bool incomingBaseline_ = false;
    bool loaded_ = false;
    bool refreshingList_ = false;
    bool refreshingThread_ = false;
    bool sending_ = false;
    // The Tor outbox (only when routing goes through Tor).
    TorController* tor_ = nullptr;
    std::shared_ptr<data::Outbox> outbox_;
    std::vector<data::OutboxItem> queued_;
    bool draining_ = false;
    QTimer drainTimer_;
    /// Puts `messages` (plus this chat's queued ones) in the open thread's model.
    void showThread(std::shared_ptr<const data::ConversationMeta> meta, std::vector<data::Message> messages);
    /// Re-reads the outbox and refreshes everything that shows it.
    void publishQueue();
    bool groupBusy_ = false;
    // How many of the open chat's latest messages its poll fetches; grows with loadOlder().
    int threadWindow_ = data::ConversationRepository::kPageSize;
    bool hasOlder_ = false;
    bool loadingOlder_ = false;
    int unreadTotal_ = 0;
    QString fileActivity_;
    QString error_;
};

}  // namespace redvault::app
