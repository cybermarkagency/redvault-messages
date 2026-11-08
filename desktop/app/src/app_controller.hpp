// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QThreadPool>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>

#include "account_session.hpp"
#include "app_lock.hpp"
#include "call_controller.hpp"
#include "channels_session.hpp"
#include "stories_session.hpp"
#include "earnings_session.hpp"
#include "chat_session.hpp"
#include "tor_controller.hpp"
#include "community_admin_session.hpp"
#include "community_session.hpp"
#include "files_session.hpp"
#include "shares_session.hpp"
#include "group_call_controller.hpp"
#include "group_invite_session.hpp"
#include "device_keys_session.hpp"
#include "inbox_session.hpp"
#include "live_session.hpp"
#include "people_session.hpp"
#include "security_settings.hpp"
#include "vault_session.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"

class QQmlEngine;
class QJSEngine;

namespace redvault::app {

/**
 * The app's top-level state, exposed to QML as the `App` singleton: signed
 * out, waiting for a two-factor code, showing a new account's recovery key
 * once, or signed in with chat, people and account sessions.
 *
 * Like the Android client, nothing secret is written to disk: the session
 * cookie and the unwrapped private key live in memory only, and quitting
 * signs you out. Only the last handle used is remembered.
 */
class AppController : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(App)
    QML_SINGLETON
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString lastHandle READ lastHandle NOTIFY lastHandleChanged)
    Q_PROPERTY(QString handle READ handle NOTIFY handleChanged)
    Q_PROPERTY(QString recoveryKey READ recoveryKey NOTIFY stateChanged)
    Q_PROPERTY(bool recoveryNeedsCode READ recoveryNeedsCode NOTIFY recoveryNeedsCodeChanged)
    Q_PROPERTY(redvault::app::ChatSession* chats READ chats NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::PeopleSession* people READ people NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::AccountSession* account READ account NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::AppLock* lock READ lock CONSTANT)
    Q_PROPERTY(redvault::app::LiveSession* live READ live NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::CallController* calls READ calls NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::GroupCallController* groupCalls READ groupCalls NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::LinkedDevices* devices READ devices NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::TransparencyLog* transparencyLog READ transparencyLog NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::RelayCheck* relays READ relays NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::FilesSession* files READ files NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::SharesSession* shares READ shares NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::VaultSession* vault READ vault NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::InboxSession* inbox READ inbox NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::DeviceKeysSession* deviceKeys READ deviceKeys NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::GroupInviteSession* groupInvites READ groupInvites NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::ChannelsSession* channels READ channels NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::CommunitySession* communities READ communities NOTIFY stateChanged)
    /// The admin tab's Communities section: only for a super admin (null otherwise; the server refuses everyone else too).
    Q_PROPERTY(redvault::app::CommunityAdminSession* communityAdmin READ communityAdmin NOTIFY stateChanged)
    /// The signed-in account is one of RedVault's super admins (the role the server gave at sign-in).
    Q_PROPERTY(bool superAdmin READ superAdmin NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::StoriesSession* stories READ stories NOTIFY stateChanged)
    Q_PROPERTY(redvault::app::EarningsSession* earnings READ earnings NOTIFY stateChanged)
    Q_PROPERTY(QString userId READ userId NOTIFY stateChanged)
    /// Tor status for the whole app (docs/tor-routing-plan.md); null when this build has no Tor controller.
    Q_PROPERTY(redvault::app::TorController* tor READ tor CONSTANT)

public:
    enum class State { SignedOut, NeedsCode, ShowRecoveryKey, SignedIn };
    Q_ENUM(State)

    /// `tor`: the app's Tor gateway; when it's enabled every request goes through it (and nowhere else).
    explicit AppController(std::string apiBase, TorController* tor = nullptr, QObject* parent = nullptr);
    ~AppController() override;

    /// QML_SINGLETON factory: hands QML the instance main() created.
    static AppController* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    static void setInstance(AppController* instance);

    State state() const { return state_; }
    bool busy() const { return busy_; }
    QString error() const { return error_; }
    QString lastHandle() const { return lastHandle_; }
    QString handle() const;
    QString recoveryKey() const { return recoveryKey_; }
    bool recoveryNeedsCode() const { return recoveryNeedsCode_; }
    ChatSession* chats() const { return chats_.get(); }
    TorController* tor() const { return tor_; }
    PeopleSession* people() const { return people_.get(); }
    AccountSession* account() const { return account_.get(); }
    AppLock* lock() const { return lock_.get(); }
    const std::string& apiBase() const { return apiBase_; }
    LiveSession* live() const { return live_.get(); }
    CallController* calls() const { return calls_.get(); }
    GroupCallController* groupCalls() const { return groupCalls_.get(); }
    LinkedDevices* devices() const { return devices_.get(); }
    TransparencyLog* transparencyLog() const { return log_.get(); }
    RelayCheck* relays() const { return relays_.get(); }
    FilesSession* files() const { return files_.get(); }
    SharesSession* shares() const { return shares_.get(); }
    VaultSession* vault() const { return vault_.get(); }
    InboxSession* inbox() const { return inbox_.get(); }
    DeviceKeysSession* deviceKeys() const { return deviceKeys_.get(); }
    GroupInviteSession* groupInvites() const { return groupInvites_.get(); }
    ChannelsSession* channels() const { return channels_.get(); }
    CommunitySession* communities() const { return communities_.get(); }
    CommunityAdminSession* communityAdmin() const { return communityAdmin_.get(); }
    bool superAdmin() const { return identity_ && identity_->role == "super_admin"; }
    StoriesSession* stories() const { return stories_.get(); }
    EarningsSession* earnings() const { return earnings_.get(); }
    QString userId() const { return identity_ ? QString::fromStdString(identity_->userId) : QString(); }
    /// For the avatar image provider (fetches with the session cookie).
    std::shared_ptr<api::ApiClient> api() const { return api_; }

    Q_INVOKABLE void signIn(const QString& handle, const QString& password);
    Q_INVOKABLE void submitCode(const QString& code);
    Q_INVOKABLE void cancelCode();
    /// Creates an account; the recovery key is shown once before signing in. The three
    /// flags are the sign-up form's boxes, as ticked; nothing is created unless all are.
    Q_INVOKABLE void registerAccount(const QString& handle, const QString& password, const QString& confirm,
                                     bool termsAccepted, bool privacyAcknowledged, bool age18Confirmed);
    /// The Terms of Service ("terms") or Privacy Policy ("privacy") as Markdown, from
    /// legal/ in the repository, built into the app (no network request).
    Q_INVOKABLE QString legalDocument(const QString& name) const;
    /// Settings > About, "Open-Source Licenses and Source Code": the version, this version's source link
    /// ("sourceUrl", empty until REDVAULT_SOURCE_CODE_URL is set at build time) and where to report a source
    /// problem ("contact", empty: the Terms of Service contact). See source_info.hpp.
    Q_INVOKABLE QVariantMap openSourceInfo() const;
    /// The licence ("license") or third-party notices ("notices"), built into the app, as plain-text pages.
    Q_INVOKABLE QStringList openSourceDocument(const QString& name) const;
    /// The user has saved their recovery key: forget it and go to the chats.
    Q_INVOKABLE void acknowledgeRecoveryKey();
    /// Recovers with the recovery key and sets a new password (asks for a two-factor code if needed).
    Q_INVOKABLE void recover(const QString& handle, const QString& recoveryKey, const QString& newPassword,
                             const QString& confirm, const QString& code);
    Q_INVOKABLE void signOut();
    /// Quits the app. Signed in, that means signing out first (keys and session live only in memory).
    Q_INVOKABLE void quit();
    Q_INVOKABLE void clearError();
    /// Copies a secret, kept out of Windows clipboard history and cloud clipboard sync.
    Q_INVOKABLE void copySecret(const QString& text) { AccountSession::copySecret(text); }

signals:
    void stateChanged();
    void busyChanged();
    void errorChanged();
    void lastHandleChanged();
    void recoveryNeedsCodeChanged();
    void handleChanged();
    /// A followed channel's new post, for the tray notifications.
    /// Device-key messages arrived in chats that are not open (preview).
    void deviceKeyMessages(int count);
    void channelPostNotification(const QString& channelId, const QString& channelName, const QString& preview);
    /// Forwarded from the chat session, for the tray notifications.
    void incomingMessages(const QString& chatId, const QString& title, int count, bool isCurrentChat);
    /// A call is ringing, for the tray notification.
    void incomingCall(const QString& handle, const QString& kind);
    /// A file didn't fit the plan's storage, for the tray notification.
    void storageFull(const QString& message);
    /// Files arrived through one of your public share links, for the tray notification.
    void shareIncoming(const QString& text);

private:
    void startSession(data::SelfIdentity identity);
    void enterSignedIn();
    void endSession(const QString& reason);
    void dropSession();
    void setBusy(bool busy);
    void setError(const QString& error);
    void forgetPendingPassword();
    void rememberHandle(const QString& handle);
    void signOutOnQuit();

    std::string apiBase_;
    std::string allowedHost_;
    TorController* tor_ = nullptr;
    /// The transport settings every request uses (Tor-only when tor_ is enabled).
    net::CurlTransport::Options transportOptions(std::chrono::seconds connectTimeout, std::chrono::seconds stallTimeout) const;
    std::shared_ptr<net::SessionCookieJar> cookies_;
    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<data::AuthRepository> auth_;
    QThreadPool pool_;
    State state_ = State::SignedOut;
    bool busy_ = false;
    bool recoveryNeedsCode_ = false;
    QString error_;
    QString lastHandle_;
    // Only between a login that asked for a code and the code: verifyMfa unwraps the key with it.
    QString pendingHandle_;
    QString pendingPassword_;
    // Only while a new account's recovery key is on screen.
    QString recoveryKey_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    // Set once the handle is changed this session (identity_ keeps the one signed in with).
    QString changedHandle_;
    std::unique_ptr<ChatSession> chats_;
    std::unique_ptr<PeopleSession> people_;
    std::unique_ptr<AccountSession> account_;
    std::unique_ptr<AppLock> lock_;
    std::unique_ptr<LiveSession> live_;
    std::unique_ptr<CallController> calls_;
    std::unique_ptr<GroupCallController> groupCalls_;
    std::unique_ptr<LinkedDevices> devices_;
    std::unique_ptr<TransparencyLog> log_;
    std::unique_ptr<RelayCheck> relays_;
    std::unique_ptr<FilesSession> files_;
    std::unique_ptr<SharesSession> shares_;
    std::unique_ptr<VaultSession> vault_;
    std::unique_ptr<InboxSession> inbox_;
    std::unique_ptr<DeviceKeysSession> deviceKeys_;
    std::unique_ptr<GroupInviteSession> groupInvites_;
    std::unique_ptr<ChannelsSession> channels_;
    std::unique_ptr<CommunitySession> communities_;
    std::unique_ptr<CommunityAdminSession> communityAdmin_;
    std::unique_ptr<StoriesSession> stories_;
    std::unique_ptr<EarningsSession> earnings_;
    bool quitting_ = false;
};

}  // namespace redvault::app
