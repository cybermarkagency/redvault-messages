#include "app_controller.hpp"
#include "source_info.hpp"

#include <QPointer>

#include <QCoreApplication>
#include <QFile>
#include <QJSEngine>
#include <QTimer>
#include <QQmlEngine>
#include <QSettings>

#include "redvault/net/curl_transport.hpp"

namespace redvault::app {

namespace {

AppController* gInstance = nullptr;

const QString kLastHandleKey = QStringLiteral("lastHandle");
constexpr int kMinHandleLength = 3;
constexpr int kMaxHandleLength = 64;  // the server's registerSchema
constexpr int kMinPasswordLength = 8;

/// Overwrites a QString's buffer before it's released. Best effort: Qt may have made copies.
void wipe(QString& text) {
    text.fill(QChar(u'\0'));
    text.clear();
}

}  // namespace

AppController::AppController(std::string apiBase, TorController* tor, QObject* parent) : QObject(parent), tor_(tor) {
    // Signing in or out changes the handle too.
    connect(this, &AppController::stateChanged, this, &AppController::handleChanged);
    // Everything the app sends (API, avatars, files) goes to our own server and nowhere else.
    if (const auto url = net::parseUrl(apiBase)) allowedHost_ = url->host;
    apiBase_ = apiBase;
    // Over Tor, first contact with the onion service can take a while, and a
    // 25 s long poll needs room for Tor's latency on top.
    const bool overTor = tor_ != nullptr && tor_->enabled();
    auto transport = std::make_shared<net::CurlTransport>(
        transportOptions(std::chrono::seconds(overTor ? 90 : 15), std::chrono::seconds(overTor ? 60 : 30)));
    cookies_ = std::make_shared<net::SessionCookieJar>();
    api_ = std::make_shared<api::ApiClient>(std::move(apiBase), transport, cookies_);
    auth_ = std::make_shared<data::AuthRepository>(api_);
    pool_.setMaxThreadCount(2);
    lastHandle_ = QSettings().value(kLastHandleKey).toString();
    lock_ = std::make_unique<AppLock>();
    connect(lock_.get(), &AppLock::lockedOut, this, &AppController::endSession);
}

AppController::~AppController() {
    // Stories and earnings read the chats: they go before them.
    delete earnings_.release();
    delete stories_.release();
    delete communityAdmin_.release();
    delete communities_.release();
    delete channels_.release();
    delete groupInvites_.release();
    delete vault_.release();
    delete shares_.release();
    delete files_.release();
    delete groupCalls_.release();
    delete calls_.release();
    delete log_.release();
    delete relays_.release();
    delete devices_.release();
    delete live_.release();
    delete chats_.release();
    delete people_.release();
    delete account_.release();
    pool_.clear();
    pool_.waitForDone();
    signOutOnQuit();
    forgetPendingPassword();
    wipe(recoveryKey_);
}

void AppController::signOutOnQuit() {
    // The app keeps its sign-in in memory only, so the next start signs in
    // afresh: end this one on the server too, or it would linger in Linked
    // devices for 30 days. Short timeouts so an offline quit doesn't hang.
    if (cookies_->empty()) return;
    // Over Tor too (or not at all, if Tor is down): never a direct connection.
    const bool overTor = tor_ != nullptr && tor_->enabled();
    const auto options = transportOptions(std::chrono::seconds(overTor ? 10 : 3), std::chrono::seconds(overTor ? 10 : 3));
    try {
        data::AuthRepository(std::make_shared<api::ApiClient>(apiBase_, std::make_shared<net::CurlTransport>(options), cookies_)).logout();
    } catch (...) {
        // Quitting regardless: the server lets the sign-in expire on its own.
    }
}

net::CurlTransport::Options AppController::transportOptions(std::chrono::seconds connectTimeout, std::chrono::seconds stallTimeout) const {
    net::CurlTransport::Options options;
    options.allowedHost = allowedHost_;
    options.connectTimeout = connectTimeout;
    options.stallTimeout = stallTimeout;
    // Tor routing: the server is a .onion and the gateway is the only way to it.
    if (tor_ != nullptr && tor_->enabled()) options.torGate = tor_->gate();
    return options;
}

AppController* AppController::create(QQmlEngine*, QJSEngine*) {
    QJSEngine::setObjectOwnership(gInstance, QJSEngine::CppOwnership);
    return gInstance;
}

void AppController::setInstance(AppController* instance) {
    gInstance = instance;
}

QString AppController::handle() const {
    if (!identity_) return {};
    return changedHandle_.isEmpty() ? QString::fromStdString(identity_->handle) : changedHandle_;
}

// --- Sign in ---

void AppController::signIn(const QString& handle, const QString& password) {
    if (busy_) return;
    const QString trimmed = handle.trimmed();
    if (trimmed.isEmpty() || password.isEmpty()) {
        setError(QStringLiteral("Enter your handle and password."));
        return;
    }
    setError({});
    setBusy(true);
    auto auth = auth_;
    const std::string h = trimmed.toStdString();
    const std::string p = password.toStdString();
    runInBackground(
        pool_, this, [auth, h, p]() { return auth->login(h, p); },
        [this, trimmed, password](data::LoginOutcome outcome) mutable {
            setBusy(false);
            if (auto* identity = std::get_if<data::SelfIdentity>(&outcome)) {
                startSession(std::move(*identity));
                enterSignedIn();
                return;
            }
            pendingHandle_ = trimmed;
            pendingPassword_ = password;
            state_ = State::NeedsCode;
            emit stateChanged();
        },
        [this](const Failure& failure) {
            setBusy(false);
            setError(failure.message);
        });
}

void AppController::submitCode(const QString& code) {
    if (busy_ || state_ != State::NeedsCode) return;
    const QString trimmed = code.trimmed();
    if (trimmed.isEmpty()) {
        setError(QStringLiteral("Enter the code from your authenticator app."));
        return;
    }
    setError({});
    setBusy(true);
    auto auth = auth_;
    const std::string h = pendingHandle_.toStdString();
    const std::string p = pendingPassword_.toStdString();
    const std::string c = trimmed.toStdString();
    runInBackground(
        pool_, this, [auth, h, p, c]() { return auth->verifyMfa(h, p, c); },
        [this](data::SelfIdentity identity) {
            setBusy(false);
            forgetPendingPassword();
            startSession(std::move(identity));
            enterSignedIn();
        },
        [this](const Failure& failure) {
            setBusy(false);
            setError(failure.code == QStringLiteral("invalid_code")
                         ? QStringLiteral("Wrong code. Check your authenticator app and try again.")
                         : failure.message);
        });
}

void AppController::cancelCode() {
    forgetPendingPassword();
    api_->cookies().clear();  // drop the half-session
    state_ = State::SignedOut;
    setError({});
    emit stateChanged();
}

// --- New account ---

void AppController::registerAccount(const QString& handle, const QString& password, const QString& confirm,
                                    bool termsAccepted, bool privacyAcknowledged, bool age18Confirmed) {
    if (busy_) return;
    const data::SignupConsent consent{termsAccepted, privacyAcknowledged, age18Confirmed};
    if (!consent.complete()) {
        setError(QStringLiteral("Tick all three boxes to create an account."));
        return;
    }
    const QString trimmed = handle.trimmed();
    if (trimmed.size() < kMinHandleLength || trimmed.size() > kMaxHandleLength) {
        setError(QStringLiteral("Pick a handle of %1 to %2 characters.").arg(kMinHandleLength).arg(kMaxHandleLength));
        return;
    }
    if (password.size() < kMinPasswordLength) {
        setError(QStringLiteral("Use at least %1 characters for your password.").arg(kMinPasswordLength));
        return;
    }
    if (password != confirm) {
        setError(QStringLiteral("The passwords don't match."));
        return;
    }
    setError({});
    setBusy(true);
    auto auth = auth_;
    const std::string h = trimmed.toStdString();
    const std::string p = password.toStdString();
    runInBackground(
        pool_, this, [auth, h, p, consent]() { return auth->registerAccount(h, p, consent); },
        [this](data::Registered registered) {
            setBusy(false);
            // The session starts now (so the account is usable), but the chats
            // wait until the recovery key has been seen and saved.
            recoveryKey_ = QString::fromStdString(registered.recoveryKey);
            startSession(std::move(registered.identity));
            state_ = State::ShowRecoveryKey;
            emit stateChanged();
        },
        [this](const Failure& failure) {
            setBusy(false);
            setError(failure.message);
        });
}

QString AppController::legalDocument(const QString& name) const {
    QFile file(name == QStringLiteral("privacy") ? QStringLiteral(":/legal/privacy-policy.md")
                                                 : QStringLiteral(":/legal/terms-of-service.md"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll());
}

QVariantMap AppController::openSourceInfo() const {
    const QString version = QStringLiteral(REDVAULT_VERSION);
    return {
        {QStringLiteral("version"), version},
        {QStringLiteral("sourceUrl"), source_info::sourceCodeLink(QStringLiteral(REDVAULT_SOURCE_CODE_URL), version)},
        {QStringLiteral("contact"), source_info::sourceContact(QStringLiteral(REDVAULT_SOURCE_CONTACT))},
    };
}

QStringList AppController::openSourceDocument(const QString& name) const {
    return source_info::documentPages(name);
}

void AppController::acknowledgeRecoveryKey() {
    if (state_ != State::ShowRecoveryKey) return;
    wipe(recoveryKey_);
    enterSignedIn();
}

// --- Recovery ---

void AppController::recover(const QString& handle, const QString& recoveryKey, const QString& newPassword,
                            const QString& confirm, const QString& code) {
    if (busy_) return;
    const QString trimmed = handle.trimmed();
    if (trimmed.isEmpty() || recoveryKey.trimmed().isEmpty()) {
        setError(QStringLiteral("Enter your handle and recovery key."));
        return;
    }
    if (newPassword.size() < kMinPasswordLength) {
        setError(QStringLiteral("Use at least %1 characters for your new password.").arg(kMinPasswordLength));
        return;
    }
    if (newPassword != confirm) {
        setError(QStringLiteral("The new passwords don't match."));
        return;
    }
    setError({});
    setBusy(true);
    auto auth = auth_;
    const std::string h = trimmed.toStdString();
    const std::string key = recoveryKey.toStdString();
    const std::string p = newPassword.toStdString();
    const QString trimmedCode = code.trimmed();
    const std::optional<std::string> c =
        trimmedCode.isEmpty() ? std::nullopt : std::optional<std::string>(trimmedCode.toStdString());
    runInBackground(
        pool_, this, [auth, h, key, p, c]() { return auth->recover(h, key, p, c); },
        [this](data::RecoverOutcome outcome) {
            setBusy(false);
            if (auto* identity = std::get_if<data::SelfIdentity>(&outcome)) {
                recoveryNeedsCode_ = false;
                emit recoveryNeedsCodeChanged();
                startSession(std::move(*identity));
                enterSignedIn();
                return;
            }
            recoveryNeedsCode_ = true;
            emit recoveryNeedsCodeChanged();
            setError(QStringLiteral("This account has two-factor on. Enter the 6-digit code from your authenticator app."));
        },
        [this](const Failure& failure) {
            setBusy(false);
            setError(failure.message);
        });
}

// --- Session ---

void AppController::startSession(data::SelfIdentity identity) {
    identity_ = std::make_shared<const data::SelfIdentity>(std::move(identity));
    rememberHandle(QString::fromStdString(identity_->handle));
}

void AppController::enterSignedIn() {
    chats_ = std::make_unique<ChatSession>(api_, identity_, tor_);
    people_ = std::make_unique<PeopleSession>(api_, identity_);
    account_ = std::make_unique<AccountSession>(api_, identity_);
    const auto expired = [this] { endSession(QStringLiteral("Your session ended. Sign in again.")); };
    connect(chats_.get(), &ChatSession::sessionExpired, this, expired);
    connect(people_.get(), &PeopleSession::sessionExpired, this, expired);
    connect(account_.get(), &AccountSession::sessionExpired, this, expired);
    connect(account_.get(), &AccountSession::handleChanged, this, [this](const QString& handle) {
        changedHandle_ = handle;
        rememberHandle(handle);
        emit handleChanged();
    });
    connect(account_.get(), &AccountSession::accountDeleted, this, [this] {
        if (calls_) calls_->hangUp();
        if (groupCalls_) groupCalls_->leave();
        // Nothing of the account is left to sign back in to.
        QSettings().remove(kLastHandleKey);
        lastHandle_.clear();
        emit lastHandleChanged();
        endSession(QStringLiteral("Your account was deleted."));
    });
    // Accepting a friend request creates a chat: show it straight away.
    connect(people_.get(), &PeopleSession::chatCreated, chats_.get(), &ChatSession::refreshNow);
    connect(chats_.get(), &ChatSession::incomingMessages, this, &AppController::incomingMessages);

    // Calls, typing and online status ride on the live channel.
    live_ = std::make_unique<LiveSession>(api_, identity_);
    connect(account_.get(), &AccountSession::onlineStatusChanged, live_.get(), &LiveSession::setPresenceEnabled);
    AccountSession* account = account_.get();
    calls_ = std::make_unique<CallController>(api_, identity_, live_.get(), chats_.get(), [account] { return account->tierId(); });
    connect(calls_.get(), &CallController::ringing, this, &AppController::incomingCall);
    groupCalls_ = std::make_unique<GroupCallController>(api_, identity_, live_.get(), chats_.get(), calls_.get(), [account] { return account->tierId(); });
    calls_->setGroupCalls(groupCalls_.get());
    connect(groupCalls_.get(), &GroupCallController::ringing, this, &AppController::incomingCall);
    devices_ = std::make_unique<LinkedDevices>(api_, identity_);
    log_ = std::make_unique<TransparencyLog>(api_, identity_, chats_.get());
    relays_ = std::make_unique<RelayCheck>(api_);
    // Files (encrypted cloud storage) and Passwords (the password manager).
    files_ = std::make_unique<FilesSession>(api_, identity_, chats_.get(), calls_->webAppBase(), tor_);
    connect(files_.get(), &FilesSession::sessionExpired, this, expired);
    connect(files_.get(), &FilesSession::storageFull, this, &AppController::storageFull);
    // Public share links: files and folders anyone can open in a browser, and what people upload through them.
    shares_ = std::make_unique<SharesSession>(api_, identity_, files_.get(), calls_->webAppBase());
    connect(shares_.get(), &SharesSession::sessionExpired, this, expired);
    connect(shares_.get(), &SharesSession::incomingArrived, this, &AppController::shareIncoming);
    connect(live_.get(), &LiveSession::liveEvent, shares_.get(), [this](const api::live::LiveEvent& event) {
        if (event.type == "share_upload" && shares_) shares_->refresh();
    });
    vault_ = std::make_unique<VaultSession>(api_, identity_, chats_.get());
    connect(vault_.get(), &VaultSession::sessionExpired, this, expired);
    // Announcements from the team (not end-to-end encrypted, and labelled so).
    inbox_ = std::make_unique<InboxSession>(api_);
    connect(inbox_.get(), &InboxSession::sessionExpired, this, expired);
    // Per-device encryption keys (protocol 3 preview): inert unless this build has the engine and the server has it on.
    deviceKeys_ = std::make_unique<DeviceKeysSession>(api_, identity_);
    connect(deviceKeys_.get(), &DeviceKeysSession::sessionExpired, this, expired);
    deviceKeys_->refresh();
    connect(deviceKeys_.get(), &DeviceKeysSession::incoming, this, &AppController::deviceKeyMessages);
    // A chat pinned to device keys never falls back to the older protocol, whichever screen sends.
    chats_->setDeviceKeyCheck([keys = QPointer<DeviceKeysSession>(deviceKeys_.get())](const std::string& chatId) {
        return keys && keys->chatPinned(QString::fromStdString(chatId));
    });
    // The server tells a device its mailbox has something ({"type":"v3-mailbox"}, no sender or chat): read it now.
    connect(live_.get(), &LiveSession::liveEvent, deviceKeys_.get(), [keys = deviceKeys_.get()](const api::live::LiveEvent& event) {
        if (event.type == "v3-mailbox") keys->receiveNow();
    });
    // Group invite links (end-to-end encrypted; the secret stays in the link) and public Channels.
    groupInvites_ = std::make_unique<GroupInviteSession>(api_, identity_, chats_.get(), calls_->webAppBase());
    connect(groupInvites_.get(), &GroupInviteSession::sessionExpired, this, expired);
    channels_ = std::make_unique<ChannelsSession>(api_, identity_, calls_->webAppBase(), [account] { return account->tierId(); });
    connect(channels_.get(), &ChannelsSession::sessionExpired, this, expired);
    connect(channels_.get(), &ChannelsSession::newPostNotification, this, &AppController::channelPostNotification);
    // Paid and free communities, and their key upkeep (the epoch keys live with the chats).
    communities_ = std::make_unique<CommunitySession>(api_, identity_, chats_->communityKeys(), chats_.get(), calls_->webAppBase());
    connect(communities_.get(), &CommunitySession::sessionExpired, this, expired);
    // RedVault's own staff: the admin tab's Communities section.
    if (superAdmin()) {
        communityAdmin_ = std::make_unique<CommunityAdminSession>(api_);
        connect(communityAdmin_.get(), &CommunityAdminSession::sessionExpired, this, expired);
    }
    // Stories: end-to-end encrypted photos, videos and text for friends, on every plan.
    stories_ = std::make_unique<StoriesSession>(api_, identity_, chats_.get());
    connect(stories_.get(), &StoriesSession::sessionExpired, this, expired);
    // Paid messages, tips and exclusive posts (paying happens on Stripe's page; only the server says it's paid).
    earnings_ = std::make_unique<EarningsSession>(api_, identity_, chats_.get());
    connect(earnings_.get(), &EarningsSession::sessionExpired, this, expired);
    // The app's own PIN lock locks the vault too.
    connect(lock_.get(), &AppLock::lockedChanged, vault_.get(), [this] {
        if (lock_->locked() && vault_) vault_->lockNow();
    });
    lock_->setSignedIn(true);
    state_ = State::SignedIn;
    emit stateChanged();
}

void AppController::signOut() {
    if (busy_) return;
    if (calls_) calls_->hangUp();
    if (groupCalls_) groupCalls_->leave();
    // Keys go right away; the cookie has to outlive the logout call that uses it.
    lock_->setSignedIn(false);
    state_ = State::SignedOut;
    identity_.reset();
    changedHandle_.clear();
    wipe(recoveryKey_);
    dropSession();
    emit stateChanged();
    setBusy(true);
    auto auth = auth_;
    // Best effort, like Android: the server is told if it can be, and logout()
    // clears the cookie jar either way.
    runInBackground(
        pool_, this, [auth]() { auth->logout(); },
        [this]() {
            setBusy(false);
            endSession({});
        },
        [this](const Failure&) {
            setBusy(false);
            endSession({});
        });
}

void AppController::endSession(const QString& reason) {
    lock_->setSignedIn(false);
    state_ = State::SignedOut;
    identity_.reset();
    changedHandle_.clear();
    wipe(recoveryKey_);
    dropSession();
    emit stateChanged();
    forgetPendingPassword();
    api_->cookies().clear();
    setError(reason);
}

void AppController::dropSession() {
    // After QML has let go of the sessions (the views unload on stateChanged).
    // Destroying them waits for their background work, then drops the last
    // references to the conversation keys (SecretBytes zero themselves).
    // The call first: it hangs up and still reads the chats while doing so.
    if (earnings_) earnings_.release()->deleteLater();
    if (stories_) stories_.release()->deleteLater();
    if (communityAdmin_) communityAdmin_.release()->deleteLater();
    if (communities_) communities_.release()->deleteLater();
    if (channels_) channels_.release()->deleteLater();
    if (groupInvites_) groupInvites_.release()->deleteLater();
    if (inbox_) inbox_.release()->deleteLater();
    if (deviceKeys_) deviceKeys_.release()->deleteLater();
    if (vault_) vault_.release()->deleteLater();
    if (shares_) shares_.release()->deleteLater();
    if (files_) files_.release()->deleteLater();
    if (groupCalls_) groupCalls_.release()->deleteLater();
    if (calls_) calls_.release()->deleteLater();
    if (log_) log_.release()->deleteLater();
    if (relays_) relays_.release()->deleteLater();
    if (devices_) devices_.release()->deleteLater();
    if (live_) live_.release()->deleteLater();
    if (chats_) chats_.release()->deleteLater();
    if (people_) people_.release()->deleteLater();
    if (account_) account_.release()->deleteLater();
}

void AppController::quit() {
    if (quitting_) return;
    quitting_ = true;
    if (state_ != State::SignedIn) {
        QCoreApplication::quit();
        return;
    }
    // Tell the server first, but never hang on it: quit when logout returns or after 5 s.
    connect(this, &AppController::busyChanged, this, [this] {
        if (!busy_) QCoreApplication::quit();
    });
    QTimer::singleShot(5000, qApp, &QCoreApplication::quit);
    signOut();
}

void AppController::clearError() {
    setError({});
}

void AppController::rememberHandle(const QString& handle) {
    lastHandle_ = handle;
    QSettings().setValue(kLastHandleKey, lastHandle_);
    emit lastHandleChanged();
}

void AppController::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void AppController::setError(const QString& error) {
    if (error_ == error) return;
    error_ = error;
    emit errorChanged();
}

void AppController::forgetPendingPassword() {
    wipe(pendingPassword_);
    pendingHandle_.clear();
}

}  // namespace redvault::app
