// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QAbstractListModel>
#include <QAudioOutput>
#include <QBuffer>
#include <QMediaPlayer>
#include <QObject>
#include <QTimer>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/model/file_message.hpp"
#include "story_device_keys.hpp"

#ifdef REDVAULT_HAS_E2E_V3
#include "redvault/data/file_repository.hpp"
#include "redvault/e2ev3/chat.hpp"
#include "redvault/e2ev3/community.hpp"
#include "redvault/e2ev3/device.hpp"
#include "redvault/e2ev3/pairing.hpp"
#include "redvault/e2ev3/stories.hpp"
#endif

namespace redvault::app {

struct Failure;
class TorController;

/**
 * The open chat's device-key messages (docs/multi-device-threat-model.md section 9.7), oldest first.
 * Each row has the roles MessageDelegate reads for a protocol-1 message (formatting, quotes, polls,
 * ticks, pictures), so both kinds of chat look and work the same, plus a few of its own.
 */
class V3ChatModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by DeviceKeysSession")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    /// Replaces the rows: a map per message, keyed by the role names.
    void setRows(QVariantList rows);
    /// The row of a message, or -1.
    Q_INVOKABLE int indexOf(const QString& messageId) const;
    /// The role names, in role order (Qt::UserRole + 1 onwards).
    static const QList<QByteArray>& names();

signals:
    void countChanged();

private:
    QVariantList rows_;
};

/**
 * Settings > Devices & encryption: per-device encryption keys ("protocol 3",
 * docs/multi-device-threat-model.md) with Signal sessions, the default path for
 * one-to-one chats and plain groups (docs/signal-integration/07-step1-default-path.md).
 * It does nothing unless this build includes the engine (REDVAULT_E2E_V3) AND
 * the server has E2E_V3_ENABLED.
 *
 * ensureDeviceKeys() runs after sign-in, on unlock and every 6 hours: the first
 * device is set up without asking when the account has no signed list, and
 * Signal is kept up to date on a trusted one. A computer that isn't in the
 * account's list is never added automatically (`needsLink`).
 *
 * A chat picks its protocol by itself (`chatMode`): device keys when every
 * member has a signed device list, or once the chat is pinned to them; the
 * older protocol only while some member has no list and the chat isn't pinned.
 *
 * The device's keys live only on this computer: the engine's state in a file,
 * its storage key in DPAPI / the system keyring / a user-only file (key_vault.hpp).
 * `status` is one of:
 *   "off"     nothing to show (no engine in this build, or the server has it off)
 *   "setup"   no device keys on the account yet: this computer can be the first trusted device
 *   "pair"    the account has trusted devices and this computer isn't one: pair it, or use the recovery key
 *   "removed" this computer was removed from the account's trusted devices
 *   "trusted" this computer is a trusted device
 */
class DeviceKeysSession : public QObject, public StoryDeviceKeys {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    /// Whether this build includes the engine at all.
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    /// {deviceId, fingerprint, isThis, canRemove, signal} for each trusted device (signal: Signal is on for it, in the signed list).
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    /// {deviceId, asking, answering, canCompare}: registered but not approved.
    Q_PROPERTY(QVariantList waiting READ waiting NOTIFY changed)
    /// Non-empty while a trusted device is asked to approve this computer: the numbers to compare.
    Q_PROPERTY(QVariantList joinCodes READ joinCodes NOTIFY changed)
    Q_PROPERTY(bool joining READ joining NOTIFY changed)
    Q_PROPERTY(QString answeringDeviceId READ answeringDeviceId NOTIFY changed)
    Q_PROPERTY(QVariantList answerCodes READ answerCodes NOTIFY changed)
    /// {effectiveAt} while someone's reset of this account's keys is waiting; empty otherwise.
    Q_PROPERTY(QVariantMap pendingReset READ pendingReset NOTIFY changed)
    /// Shown once after creating it, never stored.
    Q_PROPERTY(QString recoveryKey READ recoveryKey NOTIFY changed)
    /// {kind ("dpapi"|"keyring"|"file"), text, weak}
    Q_PROPERTY(QVariantMap protection READ protection NOTIFY changed)
    /// {text, when} newest first.
    Q_PROPERTY(QVariantList events READ events NOTIFY changed)
    Q_PROPERTY(bool trusted READ trusted NOTIFY changed)
    /// The open chat's device-key messages, and whether the chat is in device-key mode (pinned, or every member has device keys).
    Q_PROPERTY(redvault::app::V3ChatModel* chat READ chat CONSTANT)
    Q_PROPERTY(bool chatActive READ chatActive NOTIFY chatChanged)
    /**
     * How the open chat sends: "" (not this one: a community, or no engine / the server has it off),
     * "checking" (not known yet: nothing is sent until it is), "device" (device keys),
     * "older" (some member has no device keys and the chat isn't pinned: the older protocol),
     * "link" (they have device keys but this computer isn't linked: nothing can be sent until it is).
     */
    Q_PROPERTY(QString chatMode READ chatMode NOTIFY chatChanged)
    /// The account has a signed device list and this computer isn't in it: link it in Settings.
    Q_PROPERTY(bool needsLink READ needsLink NOTIFY changed)
    /// Shown once after this computer was set up as the first device: offer to create a recovery key.
    Q_PROPERTY(bool recoveryNotice READ recoveryNotice NOTIFY changed)
    Q_PROPERTY(bool chatSending READ chatSending NOTIFY chatChanged)
    Q_PROPERTY(QString chatError READ chatError NOTIFY chatChanged)
    /// What's happening to a file in a device-key chat ("Uploading..."), empty when nothing is.
    Q_PROPERTY(QString chatFileActivity READ chatFileActivity NOTIFY chatChanged)
    /// Whether the other side of the open chat has device keys: "unknown", "yes" or "no" (a group: whether every other member has).
    Q_PROPERTY(QString chatContact READ chatContact NOTIFY chatChanged)
    /// Which protocol the open one-to-one chat uses (M5): "signal" (Signal sessions with every device), "olm" (the
    /// device-key protocol), "mixed" (some of each), or "" (not known, or a group).
    Q_PROPERTY(QString chatProtocol READ chatProtocol NOTIFY chatChanged)
    /// Messages kept on the server because a contact's keys changed: they open once the person accepts the change.
    Q_PROPERTY(int chatHeld READ chatHeld NOTIFY chatChanged)
    /// A note about the last send that isn't an error: some of their devices couldn't be reached.
    Q_PROPERTY(QString chatNotice READ chatNotice NOTIFY chatChanged)
    /// Device-key messages received in chats that were not open: {chatId: count}, cleared when the chat is opened.
    Q_PROPERTY(QVariantMap unreadChats READ unreadChats NOTIFY chatChanged)
    /// This computer made keys but no trusted device approved them yet (pairing was cut short).
    Q_PROPERTY(bool interrupted READ interrupted NOTIFY changed)
    /// The open chat's pinned messages, the latest first: {messageId, preview}.
    Q_PROPERTY(QVariantList chatPins READ chatPins NOTIFY chatChanged)
    /// Your disappearing timer in the open chat, in seconds (0: off). Each message you send there carries it.
    Q_PROPERTY(int chatTimer READ chatTimer NOTIFY chatChanged)
    /// Goes up whenever a picture finishes loading (views re-read imageInfo).
    Q_PROPERTY(int imagesRevision READ imagesRevision NOTIFY imagesChanged)
    /// The voice message playing ("" for none), and where it is (ms).
    Q_PROPERTY(QString voiceId READ voiceId NOTIFY voiceChanged)
    Q_PROPERTY(qint64 voicePosition READ voicePosition NOTIFY voiceChanged)
    Q_PROPERTY(qint64 voiceDuration READ voiceDuration NOTIFY voiceChanged)
    Q_PROPERTY(bool voicePlaying READ voicePlaying NOTIFY voiceChanged)
    /// Part C2: the open chat is a community (always on device keys; no older-protocol sends, no history on the server).
    Q_PROPERTY(bool chatCommunity READ chatCommunity NOTIFY chatChanged)
    /// What the top of the open device-key chat says ("Messages from before you joined aren't available"), or "".
    Q_PROPERTY(QString chatTopNotice READ chatTopNotice NOTIFY chatChanged)

public:
    DeviceKeysSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, QObject* parent = nullptr);
    ~DeviceKeysSession() override;

    static constexpr bool available() {
#ifdef REDVAULT_HAS_E2E_V3
        return true;
#else
        return false;
#endif
    }
    QString status() const { return status_; }
    bool busy() const { return busy_; }
    QString error() const { return error_; }
    QVariantList devices() const { return devices_; }
    QVariantList waiting() const { return waiting_; }
    QVariantList joinCodes() const { return joinCodes_; }
    bool joining() const { return joining_; }
    QString answeringDeviceId() const { return answeringDeviceId_; }
    QVariantList answerCodes() const { return answerCodes_; }
    QVariantMap pendingReset() const { return pendingReset_; }
    QString recoveryKey() const { return recoveryKey_; }
    QVariantMap protection() const { return protection_; }
    QVariantList events() const { return events_; }
    bool trusted() const { return status_ == QStringLiteral("trusted"); }
    bool interrupted() const { return interrupted_; }
    V3ChatModel* chat() { return &chatModel_; }
    bool chatActive() const { return chatActive_; }
    QString chatMode() const { return chatMode_; }
    bool needsLink() const { return status_ == QStringLiteral("pair") || status_ == QStringLiteral("removed"); }
    bool recoveryNotice() const { return recoveryNotice_ && status_ == QStringLiteral("trusted"); }
    bool chatSending() const { return chatSending_; }
    QString chatError() const { return chatError_; }
    QString chatFileActivity() const { return chatFileActivity_; }
    QString chatContact() const { return chatContact_; }
    QString chatProtocol() const { return chatProtocol_; }
    int chatHeld() const { return chatHeld_; }
    QString chatNotice() const { return chatNotice_; }
    QVariantMap unreadChats() const { return unread_; }
    QVariantList chatPins() const { return chatPins_; }
    int chatTimer() const;
    int imagesRevision() const { return imagesRevision_; }
    QString voiceId() const { return voiceId_; }
    qint64 voicePosition() const;
    qint64 voiceDuration() const;
    bool voicePlaying() const;
    bool chatCommunity() const { return chatCommunity_; }
    QString chatTopNotice() const { return chatTopNotice_; }

    /// A community chat from the chat list (Part C2, docs/signal-integration/11-part-c2-communities.md).
    struct CommunityChat {
        std::string communityId;
        std::string conversationId;
        std::string kind;  // "group" | "channel"
        std::string role;  // "owner" | "admin" | "moderator" | "member"
        bool canPost = false;
    };
    /// The communities in the chat list now: their chats go over device keys only, and owner/staff devices run the
    /// membership tasks (admit, rosters, sender-key rotation) on start and every 30 s.
    void setCommunities(const std::vector<CommunityChat>& communities);
    /// Part C2: a community call's roster (who may hand out its room keys); members of a community don't list each other.
    void setGroupCallMembers(const std::string& callId, const std::string& conversationId, const std::vector<std::string>& members);
    /// A round of membership tasks now (an owner or staff member admitted or removed someone on the manage page).
    void membershipNow();

    /// What the app knows about a chat that this class doesn't (from the chat list).
    struct ChatInfo {
        bool known = false;
        bool isGroup = false;
        std::string contactUserId;  // one-to-one: the other person
        int timerSeconds = 0;  // your disappearing timer there (0: off)
        std::map<std::string, std::string> handles;  // user id -> handle
        std::string selfHandle;
        int others = 1;  // members other than you
    };
    void setChatInfo(std::function<ChatInfo(const std::string& chatId)> info) { chatInfo_ = std::move(info); }
    /// A message of the open chat to forward: its text, or its file (whose plaintext is uploaded again).
    struct ForwardItem {
        std::string text;
        std::optional<model::FileDescriptor> file;
        std::int64_t at = 0;
    };
    std::optional<ForwardItem> forwardItem(const QString& messageId);
    /**
     * Forwards into a device-key chat (pinned to them), open or not: each text with fwd, each file decrypted,
     * encrypted and uploaded again with a new key for that chat. Waits for Tor like any send.
     */
    void forwardInto(const QString& chatId, std::vector<ForwardItem> items);

    /// Whether this chat must not send with the older protocol, open or not (the in-call chat panel has to know too):
    /// it's pinned to device keys, or it's the open chat and it uses device keys or isn't decided yet.
    Q_INVOKABLE bool chatPinned(const QString& chatId);
    /// Why the older protocol can't send in this chat when it isn't simply on device keys ("checking", "link"); empty otherwise.
    QString chatRefusal(const QString& chatId) const;
    /// The chat on screen: loads its device-key history and works out its mode (chatMode).
    Q_INVOKABLE void openChat(const QString& chatId, const QString& contactUserId, bool isGroup, bool eligible);
    Q_INVOKABLE void closeChat();
    /// Unpins the open chat from device keys (9.1). Not offered anywhere in the app (no way back, step 1); kept for tests and tools.
    void useOlderProtocol();
    /// `replyToId`: the message this answers (its preview travels with it), or empty.
    Q_INVOKABLE void sendChat(const QString& text, const QString& replyToId = QString());
    /// What a reply to this message quotes ("" when it isn't in the open chat).
    Q_INVOKABLE QString previewOf(const QString& messageId);
    /// A poll in the open chat: answered by pollDone.
    Q_INVOKABLE void createPoll(const QString& question, const QStringList& options, bool multiple);
    /// Your vote (option indexes; empty takes it back), and ending your own poll.
    Q_INVOKABLE void vote(const QString& pollId, const QVariantList& options);
    Q_INVOKABLE void endPoll(const QString& pollId);
    /// Pins or unpins a message for everyone in the chat.
    Q_INVOKABLE void pinChat(const QString& messageId, bool pinned);
    /// A message is on screen in the open chat: a read receipt, if read receipts are on.
    Q_INVOKABLE void markShown(const QString& messageId);
    /// Selecting several: which can be forwarded, whether any is a file, whether all are yours to delete.
    Q_INVOKABLE QStringList forwardable(const QStringList& messageIds);
    Q_INVOKABLE bool anyFile(const QStringList& messageIds);
    Q_INVOKABLE bool allFromMe(const QStringList& messageIds);
    /// Deletes your selected messages for everyone.
    Q_INVOKABLE void deleteMany(const QStringList& messageIds);
    /// A message that failed: dropped from this computer (it never reached anyone).
    Q_INVOKABLE void discardChat(const QString& messageId);
    /// Pictures: {state, width, height} and loading one (decrypted in memory only, like the other chats).
    Q_INVOKABLE QVariantMap imageInfo(const QString& messageId) const;
    Q_INVOKABLE void loadImage(const QString& messageId, bool force = false);
    /// Plays or pauses a voice message (decrypted in memory only).
    Q_INVOKABLE void toggleVoice(const QString& messageId);
    Q_INVOKABLE void retryChat(const QString& messageId);
    /// Adds the emoji, or takes it away if it's already your reaction.
    Q_INVOKABLE void reactChat(const QString& messageId, const QString& emoji);
    /// Your own text message, by the composer: answered by chatSent.
    Q_INVOKABLE void editChat(const QString& messageId, const QString& text);
    /// Your own message, for everyone; a file goes from the server too, best effort.
    Q_INVOKABLE void deleteChat(const QString& messageId);
    /// Encrypts, uploads and releases the file, then sends its key in the envelope. Answered by chatFileDone.
    Q_INVOKABLE void sendFile(const QUrl& file);
    Q_INVOKABLE void openFile(const QString& messageId);
    Q_INVOKABLE void saveFile(const QString& messageId, const QUrl& destination);
    /// Runs the receive loop now (a live event, a push, a timer).
    Q_INVOKABLE void receiveNow();
    Q_INVOKABLE void clearChatError();

    /// Asks the server whether protocol 3 is on and reads this computer's device and the account's list.
    Q_INVOKABLE void refresh();
    /// The background check (after sign-in, on unlock, every 6 hours): sets up the first device when the account has
    /// no signed list, keeps Signal up to date when trusted. Silent: a failure is retried at the next one.
    Q_INVOKABLE void ensureDeviceKeys();
    /// Whether the app is locked now (the check waits for the unlock).
    void setLockCheck(std::function<bool()> locked) { locked_ = std::move(locked); }
    /**
     * The app's Tor gateway (docs/tor-by-default.md). Every request already goes through the app's one
     * transport, so through Tor; this only decides when a send waits: while Tor isn't connected a send is
     * queued (encrypted on this computer) instead of failing, and the queue goes, in order, once it is.
     */
    void setTor(TorController* tor);
    Q_INVOKABLE void dismissRecoveryNotice();
    /// Opens Settings > Devices & encryption (a banner or a chat asks for it).
    Q_INVOKABLE void showSettings() { emit showSettingsRequested(); }
    /// This computer becomes the first trusted device; the account's current key vouches for it.
    Q_INVOKABLE void setUp();
    /// A new computer: makes its own keys and asks the trusted devices to approve it; then shows `joinCodes`.
    Q_INVOKABLE void startPairing();
    /// The person says the numbers match (on this new computer).
    Q_INVOKABLE void confirmJoin();
    /// The numbers don't match, or the person gave up: forgets this computer's keys.
    Q_INVOKABLE void cancelJoin();
    /// On a trusted device: answers a new computer's request and shows `answerCodes`.
    Q_INVOKABLE void compareCodes(const QString& newDeviceId);
    Q_INVOKABLE void approve();
    Q_INVOKABLE void rejectAnswer();
    /// Removes a trusted device from the list.
    Q_INVOKABLE void revoke(const QString& deviceId);
    /// Turns Signal off for one of this account's devices, in the signed list. Contacts are told. Not offered in the app.
    Q_INVOKABLE void turnOffSignal(const QString& deviceId);
    /// Removes a device that was never approved.
    Q_INVOKABLE void removeWaiting(const QString& deviceId);
    Q_INVOKABLE void createRecoveryKey();
    Q_INVOKABLE void dismissRecoveryKey();
    /// With no other trusted device left: restores from the recovery key.
    Q_INVOKABLE void restore(const QString& recoveryKey);
    Q_INVOKABLE void cancelReset();
    /// Deletes this computer's keys (and its device-key history) here and in the server's directory.
    Q_INVOKABLE void forgetDevice();
    /// Pairing was interrupted: forgets the half-made device so it can start again.
    Q_INVOKABLE void startOver();
    Q_INVOKABLE void clearError();

    /// The device-key safety number with a contact: asynchronous, answered by safetyNumberReady.
    Q_INVOKABLE void loadSafetyNumber(const QString& contactUserId, const QString& conversationId);
    /// The person compared the device-key number over `accountKey` and marks it verified, or takes that back.
    Q_INVOKABLE void setDeviceKeysVerified(const QString& contactUserId, const QString& conversationId, const QString& accountKey,
                                           bool verified);
    /// After a key change: trusts the contact's new account key (verified when the new number was compared).
    Q_INVOKABLE void acceptNewDeviceKeys(const QString& contactUserId, const QString& conversationId, const QString& accountKey,
                                         bool verified);

    // --- Calls over device keys (part B, docs/signal-integration/09-part-b-calls.md) ---

    /// A call's offer, answer or restart that came from a device in its sender's signed list and passed the call checks.
    struct IncomingCallSignal {
        std::string callId;
        std::string conversationId;
        std::string fromUserId;
        std::string fromDeviceId;
        std::string step;
        std::string kind;
        std::string sdp;
    };
    /// A group call's room key that passed the checks (a member of the group, an epoch no older than the newest held).
    struct IncomingCallKey {
        std::string callId;
        std::string conversationId;
        std::string fromUserId;
        std::int64_t epoch = 0;
        std::shared_ptr<const crypto::SecretBytes> key;
    };
    /// Where checked call envelopes go (UI thread). Envelopes for calls nobody expects are never handed out.
    void setCallHandlers(std::function<void(const IncomingCallSignal&)> signal, std::function<void(const IncomingCallKey&)> key) {
        callSignalHandler_ = std::move(signal);
        callKeyHandler_ = std::move(key);
    }
    /**
     * Whether a call in this chat is set up over device keys (the chat's own rule: pinned to them, or the open chat on
     * them). False with `refusal` set when it can't be set up at all right now (not decided yet, or this computer isn't
     * linked); false with no refusal: the older protocol.
     */
    bool callsOverDeviceKeys(const QString& chatId, QString* refusal);
    /// A one-to-one call the live events named, set up over device keys: its envelopes may now be used.
    void expectCall(const std::string& callId, const std::string& conversationId, const std::string& peerUserId, bool caller);
    /// A group call on device keys this app is in: room keys from the group's members may now be used.
    void expectGroupCall(const std::string& callId, const std::string& conversationId);
    void forgetCall(const std::string& callId);
    /// The other party's device a call is bound to (the one whose offer or answer was taken), or "".
    std::string callPeerDevice(const std::string& callId) const;
    /**
     * Sends a call envelope to the peer's devices ("*") or to one of them, never queued: `done` gets "" once it went,
     * or why not. While Tor is down it fails at once, and after 30 s in any case (the call fails).
     */
    void sendCallSignal(const std::string& conversationId, const std::string& peerUserId, const std::string& callId, const std::string& step,
                        const std::string& kind, const std::string& sdp, const std::string& to, std::function<void(const QString& error)> done);
    /// Sends a group call's room key to every device of `userIds`, pairwise, never queued; `done` as above.
    void sendCallKey(const std::string& conversationId, const std::string& callId, std::int64_t epoch,
                     std::shared_ptr<const crypto::SecretBytes> key, std::vector<std::string> userIds,
                     std::function<void(const QString& error)> done);

    // --- Stories over device keys (Part D, docs/signal-integration/12-part-d-stories.md) ---

    bool storiesOnDeviceKeys() const override;
    std::shared_ptr<const crypto::SecretBytes> storyKey(const std::string& storyId, const std::string& authorId) override;
    void forgetStory(const std::string& storyId) override;
    std::vector<HeldKey> heldStoryKeys() override;
    std::vector<NamedAnswer> storyAnswers(const std::string& storyId) override;
    void sendStoryKey(const std::string& storyId, std::shared_ptr<const crypto::SecretBytes> key, std::int64_t expiresAt,
                      std::vector<std::string> viewers, std::function<void(const QString& error)> done) override;
    void sendStoryAnswer(const std::string& storyId, const std::string& authorId, const std::string& stickerId, bool final, std::string payloadJson,
                         std::int64_t expiresAt, std::function<void(const QString& error)> done) override;
    void fillStoryGaps(std::vector<std::pair<std::string, std::vector<std::string>>> gaps,
                       std::function<void(const std::string& storyId, const std::vector<std::string>& userIds)> addAccess) override;
    crypto::Bytes sealStoryAnswer(const std::string& storyId, const std::string& authorId, const model::stories::Sticker& sticker,
                                  const model::stories::Answer& answer) override;
    std::optional<model::stories::Answer> openStoryAnswer(const std::string& storyId, const model::stories::Sticker& sticker,
                                                          const crypto::Bytes& rvsd) override;
    void sendStoryReply(const std::string& chatId, const std::string& authorId, const std::string& text, const model::StoryReplyRef& ref,
                        std::function<void(const QString& error)> done) override;

signals:
    void changed();
    /**
     * The device-key safety number with a contact: `info` is empty when the contact (or this computer)
     * has no device keys, else groups (QStringList), qrText, accountKey, verified, keyChanged; `problem`
     * is set when it couldn't be checked.
     */
    void safetyNumberReady(const QString& contactUserId, const QVariantMap& info);
    void chatChanged();
    /// Device-key messages arrived in chats other than the open one (no text: the notification says only that).
    void incoming(int count);
    /// A device-key message or edit from the composer was sent (ok) or failed.
    void chatSent(bool ok);
    /// A poll from createPoll: sent (empty) or why not.
    void pollDone(const QString& error);
    void imagesChanged();
    void voiceChanged();
    /// A file from sendFile was dealt with, sent or not: `fileUrl` can go if it was only a pasted copy.
    void chatFileDone(const QString& fileUrl);
    void chatFileSaved(const QString& path);
    void sessionExpired();
    void showSettingsRequested();
    /// Part D: keys of these stories arrived on this device (no key in the signal): the feed can open them now.
    void storyKeysArrived(const QStringList& storyIds);

private:
    void setBusy(bool busy);
    void setError(const QString& message);
    bool ready() const;
    void refreshWith(bool quiet);
    /// Works out the open chat's mode from what's known now (status, pin, the members' lists).
    void decideChat();
    void setChatMode(const QString& mode);
    bool refreshing_ = false;
    bool interrupted_ = false;
    /// A refresh answered (the server's flag and this computer's status are known), so chats can be decided.
    bool known_ = false;
    /// An ensureDeviceKeys() check is waiting for its refresh: set up the first device if that's what it finds.
    bool ensurePending_ = false;
    bool autoSettingUp_ = false;
    bool recoveryNotice_ = false;
    std::function<bool()> locked_;
    QTimer ensureTimer_;

#ifdef REDVAULT_HAS_E2E_V3
    struct Snapshot;
    void apply(const Snapshot& snapshot);
    void run(const char* what, std::function<void()> work, std::function<void()> done = {});
    void updateSafetyNumber(const QString& contactUserId, const QString& conversationId, std::function<void(e2ev3::V3Device&)> change);
    e2ev3::Backend backend();
    void abandonJoin(const QString& message);
    void finishAnswer();
    /// The first device, set up by ensureDeviceKeys(): like setUp() but silent.
    void autoSetUp();

    std::shared_ptr<e2ev3::DeviceStore> store_;
    std::shared_ptr<e2ev3::V3Server> server_;
    /// server_ itself: the HTTP server, with chat sends queued while Tor is down (send_queue.hpp).
    std::shared_ptr<e2ev3::QueueingV3Server> queueing_;
    /// Part C2: the community chats and their routes (over the same ApiClient, so the Tor transport).
    std::shared_ptr<e2ev3::Communities> communities_;
    std::string communitiesDevice_;
    /// One round of membership tasks at a time, on the chat thread (owner/staff devices only do anything).
    void runMembershipTasks();
    bool tasksRunning_ = false;
    bool hasStaffCommunity_ = false;
    /// Part B: the checks on call envelopes, and the ones the chat thread received, waiting to be checked here.
    e2ev3::CallGate callGate_;
    struct CallInbox;
    std::shared_ptr<CallInbox> callInbox_;
    void dispatchCalls();
    /// One receive pass that keeps the call envelopes it finds for dispatchCalls (they're acknowledged, never stored).
    static std::vector<e2ev3::ChatRecord> receiveWithCalls(e2ev3::V3Device& device, e2ev3::ChatHistory& history, CallInbox* inbox,
                                                           e2ev3::ReceiveStats* stats = nullptr);
    /// Tor routing is on and Tor isn't connected: sends wait.
    bool torDown() const;
    /// Sends what waited for Tor, in order (nothing if Tor is down or nothing waits).
    void drainQueued();
    bool draining_ = false;
    // One instance of the device for the session: its state lives in memory between engine calls.
    std::shared_ptr<e2ev3::V3Device> device_;
    /// Part D: this device's story keys and answers, and the device, for calls from the stories thread.
    struct StoryPart;
    std::shared_ptr<StoryPart> storyPart_;
    std::shared_ptr<e2ev3::StoryStore> storyStore();
    e2ev3::StoryFanOut storyFanOut() const;
    void announceStoryKeys();
    std::shared_ptr<e2ev3::ChatHistory> history();
    void reloadChat();
    void checkContact();
    using ChatWork = std::function<void(e2ev3::V3Device&, e2ev3::ChatHistory&, const std::string& chat, const std::string& contact)>;
    /// Runs one send on the chat thread, after reading what arrived first (9.6). `fromComposer`: it holds the composer until it's done.
    /// `queue`: what to do instead while Tor is down or earlier sends still wait (queue it, so nothing overtakes).
    void runChat(ChatWork work, const QString& fallback, bool fromComposer, ChatWork queue = {},
                 std::function<void(const QString& error)> finished = {});
    void chatFailed(const Failure& failure, const QString& fallback);
    void setChatFileActivity(const QString& activity);
    std::optional<e2ev3::ChatRecord> shownMessage(const QString& messageId);
    std::optional<model::FileDescriptor> shownFile(const QString& messageId);
    /// What the chat's sends go to: the contact, or empty for a group.
    std::string chatTarget() const { return chatIsGroup_ ? std::string() : chatContactUser_.toStdString(); }
    std::filesystem::path cacheDir_;
    std::shared_ptr<data::FileRepository> files_;
    std::shared_ptr<e2ev3::ChatHistory> history_;
    /// Receipts this device owes (part A), sent in batches from the chat thread.
    std::shared_ptr<e2ev3::ReceiptBatcher> receipts_ = std::make_shared<e2ev3::ReceiptBatcher>();
    /// Batches that failed: added back on the next mailbox pass.
    std::vector<e2ev3::ReceiptBatcher::Batch> retryReceipts_;
    /// A new message for the open chat with your timer: the chat's own send path, so it waits for Tor like the rest.
    void sendMessage(e2ev3::OutgoingMessage message, const QString& fallback, bool fromComposer);
    std::string historyDevice_;
    std::shared_ptr<e2ev3::NewDevicePairing> joiningPairing_;
    std::shared_ptr<e2ev3::TrustedPairing> answeringPairing_;
    std::shared_ptr<std::atomic<bool>> stopWaiting_ = std::make_shared<std::atomic<bool>>(false);
    bool enabled_ = false;
    QVariantList enrollmentRequests_;
#endif

    std::function<ChatInfo(const std::string&)> chatInfo_;
    ChatInfo openInfo() const;
    QVariantList chatPins_;
    /// The open chat's messages from others not yet read here: id -> author (a read receipt once shown).
    std::map<QString, std::string> readable_;
    std::map<QString, QString> imageStates_;
    int imagesRevision_ = 0;
    QString voiceId_;
    std::unique_ptr<QMediaPlayer> voicePlayer_;
    std::unique_ptr<QAudioOutput> voiceOutput_;
    std::unique_ptr<QBuffer> voiceBuffer_;
    QTimer receiptTimer_;
    QTimer expiryTimer_;
    QTimer membershipTimer_;
    bool chatCommunity_ = false;
    QString chatTopNotice_;
    /// Schedules the next receipt batch and the next disappearing message (both on the chat thread).
    void scheduleReceipts();
    void scheduleExpiry();
    void flushReceipts();
    void eraseExpiredNow();

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    TorController* tor_ = nullptr;
    QThreadPool pool_;
    QThreadPool waitPool_;
    QThreadPool chatPool_;
    /// Call envelopes go out here, beside the chat thread, so a call never waits behind a file.
    QThreadPool callPool_;
    QTimer receiveTimer_;
    /// A receive was asked for while one ran (an offer may be waiting): one more pass when it's done.
    bool receiveAgain_ = false;
    std::function<void(const IncomingCallSignal&)> callSignalHandler_;
    std::function<void(const IncomingCallKey&)> callKeyHandler_;
    V3ChatModel chatModel_;
    QString chatId_;
    QString chatContactUser_;
    QString chatContact_ = QStringLiteral("unknown");
    QString chatMode_;
    /// The chat whose members' lists are being asked for (one check at a time).
    QString contactCheckFor_;
    QString chatProtocol_;
    int chatHeld_ = 0;
    QString chatNotice_;
    /// Set on the chat thread by a send, read when it finishes: devices that got no copy.
    std::shared_ptr<std::atomic<int>> lastUnreachable_ = std::make_shared<std::atomic<int>>(0);
    QVariantMap unread_;
    bool chatIsGroup_ = false;
    bool chatEligible_ = false;
    bool chatActive_ = false;
    bool chatSending_ = false;
    bool chatReceiving_ = false;
    QString chatError_;
    QString chatFileActivity_;

    QString status_ = QStringLiteral("off");
    bool busy_ = false;
    QString error_;
    QVariantList devices_;
    QVariantList waiting_;
    QVariantList joinCodes_;
    bool joining_ = false;
    QString answeringDeviceId_;
    QVariantList answerCodes_;
    QVariantMap pendingReset_;
    QString recoveryKey_;
    QVariantMap protection_;
    QVariantList events_;
};

}  // namespace redvault::app
