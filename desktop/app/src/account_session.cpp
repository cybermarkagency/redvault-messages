// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "account_session.hpp"

#include <QClipboard>
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QFileInfo>
#include <QLocale>
#include <QMimeData>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

#include "avatar_jpeg.hpp"
#include "local_store.hpp"
#include "redvault/data/sign_in.hpp"
#include "redvault/model/file_message.hpp"

namespace redvault::app {

namespace {

constexpr int kMinPasswordLength = 8;

/// Overwrites a QString's buffer before it's released. Best effort: Qt may have made copies.
void wipe(QString& text) {
    text.fill(QChar(u'\0'));
    text.clear();
}

}  // namespace

AccountSession::AccountSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                               QObject* parent)
    : QObject(parent),
      api_(std::move(api)),
      identity_(std::move(identity)),
      account_(std::make_shared<data::AccountRepository>(api_)) {
    pool_.setMaxThreadCount(1);
    // Like Android: re-read the profile (incl. plan) every minute.
    refreshTimer_.setInterval(60 * 1000);
    connect(&refreshTimer_, &QTimer::timeout, this, &AccountSession::reload);
    refreshTimer_.start();
    // A plan change on its way is checked every few seconds instead.
    planWatchTimer_.setInterval(4 * 1000);
    connect(&planWatchTimer_, &QTimer::timeout, this, [this]() {
        if (QDateTime::currentMSecsSinceEpoch() > watchUntilMs_) {
            planWatchTimer_.stop();
            watchedTier_.clear();
            return;
        }
        reload();
    });
    reload();
}

AccountSession::~AccountSession() {
    pool_.clear();
    pool_.waitForDone();
    dismissSecrets();
}

bool AccountSession::readReceipts() const {
    return local_store::readReceipts();
}

template <typename Work, typename Done>
void AccountSession::action(const QString& name, Work work, Done onDone) {
    if (busy_) return;
    setBusy(true);
    setMessages({}, {});
    runInBackground(
        pool_, this, std::move(work),
        [this, name, onDone = std::move(onDone)](auto&&... result) mutable {
            setBusy(false);
            onDone(std::forward<decltype(result)>(result)...);
            emit done(name);
        },
        [this](const Failure& failure) {
            setBusy(false);
            // A wrong password is a 401 too (invalid_credentials), but the session is fine: say so instead of signing out.
            const bool wrongPassword = failure.code == QStringLiteral("invalid_credentials") ||
                                       failure.code == QStringLiteral("invalid_password");
            if (failure.status == 401 && !wrongPassword) {
                emit sessionExpired();
                return;
            }
            // The server words wrong-password and wrong-code refusals itself; keep a clear fallback.
            QString message = failure.message;
            if (failure.code == QStringLiteral("invalid_code")) message = QStringLiteral("Wrong code. Check your authenticator app and try again.");
            if (failure.code == QStringLiteral("invalid_credentials") || failure.code == QStringLiteral("invalid_password")) {
                message = QStringLiteral("That password isn't right.");
            }
            setMessages(message, {});
        });
}

void AccountSession::reload() {
    auto account = account_;
    runInBackground(
        pool_, this, [account]() { return account->me(); },
        [this](api::MeResult me) {
            const bool sharedBefore = showOnlineStatus();
            me_ = std::move(me);
            loaded_ = true;
            emit accountChanged();
            if (sharedBefore != showOnlineStatus() || !planLoadedOnce_) emit onlineStatusChanged(showOnlineStatus());
            planLoadedOnce_ = true;
            emit planChanged();
            checkWatchedPlan();
            reloadPlanDetails();
        },
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
        });
}

// Pro: storage use. Free has no storage (sending files is Pro-only), but may upgrade.
void AccountSession::reloadPlanDetails() {
    auto api = api_;
    runInBackground(
        pool_, this, [api]() { return api->billingConfig(); },
        [this](api::BillingConfigDto config) {
            paymentLink_ = QString::fromStdString(config.paymentLink.value_or(""));
            premiumPaymentLink_ = QString::fromStdString(config.premiumPaymentLink.value_or(""));
            filesStorage_ = config.filesStorage;
            emit storageChanged();
            emit planChanged();
        },
        [](const Failure&) {});
    if (!pro()) {
        storage_.reset();
        emit storageChanged();
        planStatus_.reset();
        emit planChanged();
        return;
    }
    runInBackground(
        pool_, this, [api]() { return api->fileStorageUsage(); },
        [this](api::FileStorageUsageDto usage) {
            storage_ = usage;
            emit storageChanged();
        },
        [](const Failure&) {});
    // An older server has no /billing/status; the plan then shows without an end date or buttons.
    runInBackground(
        pool_, this, [api]() { return api->planStatus(); },
        [this](api::PlanStatusDto status) {
            planStatus_ = std::move(status);
            emit planChanged();
        },
        [](const Failure&) {});
}

QString AccountSession::plan() const {
    if (premium()) return QStringLiteral("Premium");
    return pro() ? QStringLiteral("Pro") : QStringLiteral("Free");
}

void AccountSession::setShowOnlineStatus(bool enabled) {
    auto api = api_;
    action(QStringLiteral("onlineStatus"),
           [api, enabled]() {
               api::SettingsRequest body;
               body.showOnlineStatus = enabled;
               api->updateSettings(body);
           },
           [this, enabled]() {
               if (me_) me_->showOnlineStatus = enabled;
               emit accountChanged();
               emit onlineStatusChanged(enabled);
           });
}

void AccountSession::setTransparencyMode(bool enabled) {
    auto api = api_;
    action(QStringLiteral("transparencyMode"),
           [api, enabled]() {
               api::SettingsRequest body;
               body.transparencyMode = enabled;
               api->updateSettings(body);
           },
           [this, enabled]() {
               if (me_) me_->transparencyMode = enabled;
               emit accountChanged();
           });
}

void AccountSession::switchToFree() {
    auto api = api_;
    action(QStringLiteral("switchToFree"), [api]() { return api->cancelPlan(); },
           [this](api::PlanStatusDto status) {
               planStatus_ = std::move(status);
               emit planChanged();
               setMessages({}, QStringLiteral("Your subscription won't renew, and you won't be charged again."));
           });
}

void AccountSession::keepPlan() {
    auto api = api_;
    action(QStringLiteral("keepPlan"), [api]() { return api->resumePlan(); },
           [this](api::PlanStatusDto status) {
               planStatus_ = std::move(status);
               emit planChanged();
               setMessages({}, QStringLiteral("You're staying on %1. Your subscription renews as before.").arg(plan()));
           });
}

model::plans::PlanInputs AccountSession::planInputs() const {
    return {.tier = tierId(),
            .status = planStatus_,
            .proCheckout = !paymentLink_.isEmpty(),
            .premiumCheckout = !premiumPaymentLink_.isEmpty(),
            .filesStorage = filesStorage_};
}

QVariantList AccountSession::planChoices() const {
    const auto qs = [](const std::string& text) { return QString::fromStdString(text); };
    QVariantList out;
    for (const auto& c : model::plans::choices(planInputs())) {
        QStringList includes;
        for (const auto& line : c.includes) includes << qs(line);
        QString action;
        switch (c.action) {
            case model::plans::PlanAction::Checkout: action = QStringLiteral("checkout"); break;
            case model::plans::PlanAction::Switch: action = QStringLiteral("switch"); break;
            case model::plans::PlanAction::Cancel: action = QStringLiteral("cancel"); break;
            case model::plans::PlanAction::Keep: action = QStringLiteral("keep"); break;
            case model::plans::PlanAction::None: break;
        }
        out.append(QVariantMap{{QStringLiteral("tier"), qs(c.tier)},
                               {QStringLiteral("name"), qs(c.name)},
                               {QStringLiteral("price"), qs(c.price)},
                               {QStringLiteral("per"), qs(c.per)},
                               {QStringLiteral("includes"), includes},
                               {QStringLiteral("current"), c.current},
                               {QStringLiteral("status"), qs(c.status)},
                               {QStringLiteral("action"), action},
                               {QStringLiteral("actionLabel"), qs(c.actionLabel)},
                               {QStringLiteral("confirm"), qs(c.confirm)},
                               {QStringLiteral("note"), qs(c.note)}});
    }
    return out;
}

QString AccountSession::planPriceNote() const {
    return QString::fromUtf8(model::plans::kPriceNote);
}

void AccountSession::choosePlan(const QString& tier) {
    const std::string target = tier.toStdString();
    const auto all = model::plans::choices(planInputs());
    const auto it = std::find_if(all.begin(), all.end(), [&](const auto& c) { return c.tier == target; });
    if (it == all.end()) return;
    const QString name = QString::fromStdString(it->name);
    switch (it->action) {
        case model::plans::PlanAction::Checkout:
            openCheckout(target);
            setMessages({}, QStringLiteral("Finish paying in your browser. Your plan changes to %1 here as soon as Stripe "
                                           "confirms the payment.")
                                .arg(name));
            watchForPlan(target);
            return;
        case model::plans::PlanAction::Switch: {
            auto api = api_;
            action(QStringLiteral("switchPlan"), [api, target]() { return api->switchPlan(target); },
                   [this, target, name](api::PlanStatusDto status) {
                       const bool done = status.tier == target;
                       planStatus_ = std::move(status);
                       emit planChanged();
                       if (done) {
                           setMessages({}, QStringLiteral("You're on %1 now.").arg(name));
                       } else {
                           // The difference is still being charged; Stripe's webhook finishes the change.
                           setMessages({}, QStringLiteral("Your plan changes to %1 as soon as the payment goes through.").arg(name));
                           watchForPlan(target);
                       }
                       reload();
                   });
            return;
        }
        case model::plans::PlanAction::Cancel:
            switchToFree();
            return;
        case model::plans::PlanAction::Keep:
            keepPlan();
            return;
        case model::plans::PlanAction::None:
            return;
    }
}

void AccountSession::openCheckout(const std::string& tier) {
    // The web app's paymentLinkFor: Stripe hands client_reference_id back to the webhook.
    QUrl url(tier == "premium" ? premiumPaymentLink_ : paymentLink_);
    if (url.isEmpty()) return;
    QUrlQuery query(url);
    query.removeAllQueryItems(QStringLiteral("client_reference_id"));
    query.addQueryItem(QStringLiteral("client_reference_id"), QString::fromStdString(identity_->userId));
    url.setQuery(query);
    // The address comes from the server: only Stripe's own payment page opens from here.
    if (!net::isCheckoutUrl(url.toString(QUrl::FullyEncoded).toStdString())) {
        setMessages(QStringLiteral("Checkout isn't available right now."), {});
        return;
    }
    QDesktopServices::openUrl(url);
}

void AccountSession::watchForPlan(const std::string& tier) {
    watchedTier_ = tier;
    // Long enough to fill in a card form; the minute-by-minute refresh carries on after.
    watchUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 20 * 60 * 1000;
    planWatchTimer_.start();
}

void AccountSession::checkWatchedPlan() {
    if (watchedTier_.empty() || tierId() != watchedTier_) return;
    planWatchTimer_.stop();
    watchedTier_.clear();
    setMessages({}, QStringLiteral("You're on %1 now.").arg(plan()));
}

// Premium includes everything Pro has.
bool AccountSession::pro() const {
    const auto& tier = me_ ? me_->subscriptionTier : identity_->subscriptionTier;
    return tier == "pro" || tier == "premium";
}

QString AccountSession::storageText() const {
    if (!storage_) return {};
    const auto bytes = [](std::int64_t n) { return QString::fromStdString(model::file_message::formatBytes(n)); };
    return QStringLiteral("%1 of %2 used · %3 left")
        .arg(bytes(storage_->used), bytes(storage_->limit), bytes(std::max<std::int64_t>(0, storage_->limit - storage_->used)));
}

double AccountSession::storageFraction() const {
    if (!storage_ || storage_->limit <= 0) return 0;
    return std::clamp(static_cast<double>(storage_->used) / static_cast<double>(storage_->limit), 0.0, 1.0);
}

void AccountSession::setReadReceipts(bool enabled) {
    local_store::setReadReceipts(enabled);
    emit readReceiptsChanged();
}

void AccountSession::setAcceptMessageRequests(bool enabled) {
    auto account = account_;
    action(QStringLiteral("messageRequests"), [account, enabled]() { account->setAcceptMessageRequests(enabled); },
           [this, enabled]() {
               if (me_) me_->acceptMessageRequests = enabled;
               emit accountChanged();
           });
}

void AccountSession::changePassword(const QString& current, const QString& next, const QString& confirm) {
    if (current.isEmpty()) return setMessages(QStringLiteral("Enter your current password."), {});
    if (next.size() < kMinPasswordLength) {
        return setMessages(QStringLiteral("Use at least %1 characters for the new password.").arg(kMinPasswordLength), {});
    }
    if (next != confirm) return setMessages(QStringLiteral("The new passwords don't match."), {});
    auto account = account_;
    auto identity = identity_;
    const std::string cur = current.toStdString();
    const std::string nxt = next.toStdString();
    action(QStringLiteral("password"), [account, identity, cur, nxt]() { account->changePassword(*identity, cur, nxt); },
           [this]() {
               setMessages({}, QStringLiteral("Password changed. Your other devices have been signed out."));
           });
}

void AccountSession::createRecoveryKey(const QString& password) {
    if (password.isEmpty()) return setMessages(QStringLiteral("Enter your password."), {});
    auto account = account_;
    auto identity = identity_;
    const std::string pw = password.toStdString();
    action(QStringLiteral("recoveryKey"), [account, identity, pw]() { return account->createRecoveryKey(*identity, pw); },
           [this](std::string key) {
               newRecoveryKey_ = QString::fromStdString(key);
               if (me_) me_->hasRecoveryKey = true;
               emit secretsChanged();
               emit accountChanged();
           });
}

void AccountSession::startMfaSetup() {
    auto account = account_;
    action(QStringLiteral("mfaSetup"), [account]() { return account->startMfaSetup(); },
           [this](api::MfaSetupResult setup) {
               // Shown as groups of four so it's easy to type into an authenticator app.
               QString grouped;
               const QString secret = QString::fromStdString(setup.secretBase32);
               for (int i = 0; i < secret.size(); i += 4) {
                   if (i > 0) grouped += u' ';
                   grouped += secret.mid(i, 4);
               }
               mfaSecret_ = grouped;
               mfaUri_ = QString::fromStdString(setup.otpauthUri);
               emit secretsChanged();
           });
}

void AccountSession::confirmMfa(const QString& code) {
    const QString trimmed = code.trimmed();
    if (trimmed.isEmpty()) return setMessages(QStringLiteral("Enter the 6-digit code from your authenticator app."), {});
    auto account = account_;
    const std::string c = trimmed.toStdString();
    action(QStringLiteral("mfaConfirm"), [account, c]() { account->confirmMfa(c); },
           [this]() {
               wipe(mfaSecret_);
               wipe(mfaUri_);
               if (me_) me_->mfaEnabled = true;
               emit secretsChanged();
               emit accountChanged();
               setMessages({}, QStringLiteral("Two-factor authentication is on."));
           });
}

void AccountSession::disableMfa(const QString& password) {
    if (password.isEmpty()) return setMessages(QStringLiteral("Enter your password."), {});
    auto account = account_;
    auto identity = identity_;
    const std::string pw = password.toStdString();
    action(QStringLiteral("mfaDisable"), [account, identity, pw]() { account->disableMfa(*identity, pw); },
           [this]() {
               if (me_) me_->mfaEnabled = false;
               emit accountChanged();
               setMessages({}, QStringLiteral("Two-factor authentication is off."));
           });
}

void AccountSession::changeHandle(const QString& handle) {
    const QString trimmed = handle.trimmed();
    if (trimmed.size() < 3 || trimmed.size() > 64) {
        return setMessages(QStringLiteral("Pick a handle of 3 to 64 characters."), {});
    }
    auto api = api_;
    const std::string username = trimmed.toStdString();
    action(QStringLiteral("handle"), [api, username]() { api->changeUsername({.username = username}); },
           [this, trimmed]() {
               if (me_) {
                   data::moveSignInPin(me_->username, trimmed.toStdString());
                   me_->username = trimmed.toStdString();
               }
               emit accountChanged();
               emit handleChanged(trimmed);
               setMessages({}, QStringLiteral("Your handle is now %1.").arg(trimmed));
           });
}

void AccountSession::deleteAccount(const QString& password) {
    if (password.isEmpty()) return setMessages(QStringLiteral("Enter your password."), {});
    auto account = account_;
    auto identity = identity_;
    const std::string pw = password.toStdString();
    action(QStringLiteral("deleteAccount"), [account, identity, pw]() { account->deleteAccount(*identity, pw); },
           [this]() { emit accountDeleted(); });
}

QString AccountSession::avatarUrl() const {
    return me_ ? QString::fromStdString(me_->avatarUrl.value_or("")) : QString();
}

void AccountSession::uploadAvatar(const QUrl& imageFile) {
    auto account = account_;
    const QString path = imageFile.isLocalFile() ? imageFile.toLocalFile() : imageFile.toString();
    action(QStringLiteral("avatar"), [account, path]() { return account->uploadAvatar(avatarJpegBase64(path)); },
           [this](std::optional<std::string> url) {
               if (me_) me_->avatarUrl = std::move(url);
               emit accountChanged();
               setMessages({}, QStringLiteral("Profile photo updated."));
           });
}

void AccountSession::removeAvatar() {
    auto account = account_;
    action(QStringLiteral("avatar"), [account]() { account->removeAvatar(); },
           [this]() {
               if (me_) me_->avatarUrl.reset();
               emit accountChanged();
               setMessages({}, QStringLiteral("Profile photo removed."));
           });
}

void AccountSession::dismissSecrets() {
    if (newRecoveryKey_.isEmpty() && mfaSecret_.isEmpty() && mfaUri_.isEmpty()) return;
    wipe(newRecoveryKey_);
    wipe(mfaSecret_);
    wipe(mfaUri_);
    emit secretsChanged();
}

void AccountSession::clearMessages() {
    setMessages({}, {});
}

void AccountSession::copySecret(const QString& text) {
    auto* mime = new QMimeData;
    mime->setText(text);
#ifdef _WIN32
    // Registered clipboard formats Windows checks before recording a copy.
    const QByteArray zero(4, '\0');
    mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\""),
                  zero);
    mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanIncludeInClipboardHistory\""), zero);
    mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanUploadToCloudClipboard\""), zero);
#endif
    QGuiApplication::clipboard()->setMimeData(mime);
}

void AccountSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void AccountSession::setMessages(const QString& error, const QString& notice) {
    if (error_ == error && notice_ == notice) return;
    error_ = error;
    notice_ = notice;
    emit messagesChanged();
}

}  // namespace redvault::app
