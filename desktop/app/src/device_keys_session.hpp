#pragma once

#include <QAbstractListModel>
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
#include <memory>

#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"

#ifdef REDVAULT_HAS_E2E_V3
#include "redvault/data/file_repository.hpp"
#include "redvault/e2ev3/chat.hpp"
#include "redvault/e2ev3/device.hpp"
#include "redvault/e2ev3/pairing.hpp"
#endif

namespace redvault::app {

struct Failure;

/// The open chat's device-key messages (docs/multi-device-threat-model.md section 9.7), oldest first.
class V3ChatModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by DeviceKeysSession")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TextRole,
        FromMeRole,
        TimeRole,
        StatusRole,
        SenderRole,
        DeletedRole,
        EditedRole,
        ReactionsRole,
        HasFileRole,
        FileNameRole,
        FileSizeRole,
        CanOpenRole,
        CanReactRole,
        CanEditRole,
        CanDeleteRole,
    };
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    /// Replaces the rows: a map per message, keyed by the role names.
    void setRows(QVariantList rows);

signals:
    void countChanged();

private:
    QVariantList rows_;
};

/**
 * Settings > Device keys (preview): per-device encryption keys ("protocol 3",
 * docs/multi-device-threat-model.md), the desktop counterpart of the web app's
 * screens/settings/DeviceKeys.tsx. A development preview: it does nothing
 * unless this build includes the engine (REDVAULT_E2E_V3) AND the server has
 * E2E_V3_ENABLED. Messages keep using the current encryption.
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
class DeviceKeysSession : public QObject {
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
    /// The open chat's device-key messages, and whether the chat is in device-key mode (pinned, or switched on by the person).
    Q_PROPERTY(redvault::app::V3ChatModel* chat READ chat CONSTANT)
    Q_PROPERTY(bool chatActive READ chatActive NOTIFY chatChanged)
    Q_PROPERTY(bool chatSending READ chatSending NOTIFY chatChanged)
    Q_PROPERTY(QString chatError READ chatError NOTIFY chatChanged)
    /// What's happening to a file in a device-key chat ("Uploading..."), empty when nothing is.
    Q_PROPERTY(QString chatFileActivity READ chatFileActivity NOTIFY chatChanged)
    /// Whether the other side of the open chat has device keys: "unknown", "yes" or "no" (a group: whether any other member has).
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
    bool chatSending() const { return chatSending_; }
    QString chatError() const { return chatError_; }
    QString chatFileActivity() const { return chatFileActivity_; }
    QString chatContact() const { return chatContact_; }
    QString chatProtocol() const { return chatProtocol_; }
    int chatHeld() const { return chatHeld_; }
    QString chatNotice() const { return chatNotice_; }
    QVariantMap unreadChats() const { return unread_; }

    /// Whether this chat sends only with device keys, open or not (the in-call chat panel has to know too).
    Q_INVOKABLE bool chatPinned(const QString& chatId);
    /// The chat on screen: loads its device-key history, and switches device-key mode on when the chat is pinned to it.
    Q_INVOKABLE void openChat(const QString& chatId, const QString& contactUserId, bool isGroup, bool eligible);
    Q_INVOKABLE void closeChat();
    /// The person chooses device keys for this chat; the first message sent pins it.
    Q_INVOKABLE void useDeviceKeys();
    /// The explicit step back to the current protocol (9.1): unpins; messages stay in the device-key history.
    Q_INVOKABLE void useOlderProtocol();
    Q_INVOKABLE void sendChat(const QString& text);
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
    /// Turns Signal off for one of this account's devices, in the signed list. Contacts are told (development builds).
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
    /// A file from sendFile was dealt with, sent or not: `fileUrl` can go if it was only a pasted copy.
    void chatFileDone(const QString& fileUrl);
    void chatFileSaved(const QString& path);
    void sessionExpired();

private:
    void setBusy(bool busy);
    void setError(const QString& message);
    bool ready() const;
    bool refreshing_ = false;
    bool interrupted_ = false;

#ifdef REDVAULT_HAS_E2E_V3
    struct Snapshot;
    void apply(const Snapshot& snapshot);
    void run(const char* what, std::function<void()> work, std::function<void()> done = {});
    void updateSafetyNumber(const QString& contactUserId, const QString& conversationId, std::function<void(e2ev3::V3Device&)> change);
    e2ev3::Backend backend();
    void abandonJoin(const QString& message);
    void finishAnswer();

    std::shared_ptr<e2ev3::DeviceStore> store_;
    std::shared_ptr<e2ev3::V3Server> server_;
    // One instance of the device for the session: its state lives in memory between engine calls.
    std::shared_ptr<e2ev3::V3Device> device_;
    std::shared_ptr<e2ev3::ChatHistory> history();
    void reloadChat();
    void checkContact();
    using ChatWork = std::function<void(e2ev3::V3Device&, e2ev3::ChatHistory&, const std::string& chat, const std::string& contact)>;
    /// Runs one send on the chat thread, after reading what arrived first (9.6). `fromComposer`: it holds the composer until it's done.
    void runChat(ChatWork work, const QString& fallback, bool fromComposer);
    void chatFailed(const Failure& failure, const QString& fallback);
    void setChatFileActivity(const QString& activity);
    std::optional<e2ev3::ChatRecord> shownMessage(const QString& messageId);
    std::optional<model::FileDescriptor> shownFile(const QString& messageId);
    /// What the chat's sends go to: the contact, or empty for a group.
    std::string chatTarget() const { return chatIsGroup_ ? std::string() : chatContactUser_.toStdString(); }
    std::filesystem::path cacheDir_;
    std::shared_ptr<data::FileRepository> files_;
    std::shared_ptr<e2ev3::ChatHistory> history_;
    std::string historyDevice_;
    std::shared_ptr<e2ev3::NewDevicePairing> joiningPairing_;
    std::shared_ptr<e2ev3::TrustedPairing> answeringPairing_;
    std::shared_ptr<std::atomic<bool>> stopWaiting_ = std::make_shared<std::atomic<bool>>(false);
    bool enabled_ = false;
    QVariantList enrollmentRequests_;
#endif

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    QThreadPool pool_;
    QThreadPool waitPool_;
    QThreadPool chatPool_;
    QTimer receiveTimer_;
    V3ChatModel chatModel_;
    QString chatId_;
    QString chatContactUser_;
    QString chatContact_ = QStringLiteral("unknown");
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
