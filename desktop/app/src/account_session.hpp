// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "async.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/data/account_repository.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/model/plan_choices.hpp"

namespace redvault::app {

/**
 * Account settings (Android's ui/settings and ui/recovery): read receipts,
 * message requests, password, recovery key and two-factor. Secrets shown
 * once (a new recovery key, a two-factor setup key) are held only until the
 * dialog showing them is dismissed. The plan is re-read every minute, so an
 * upgrade shows up without signing in again.
 */
class AccountSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    Q_PROPERTY(bool loaded READ loaded NOTIFY accountChanged)
    Q_PROPERTY(bool mfaEnabled READ mfaEnabled NOTIFY accountChanged)
    Q_PROPERTY(bool hasRecoveryKey READ hasRecoveryKey NOTIFY accountChanged)
    Q_PROPERTY(bool acceptMessageRequests READ acceptMessageRequests NOTIFY accountChanged)
    Q_PROPERTY(bool readReceipts READ readReceipts NOTIFY readReceiptsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY messagesChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY messagesChanged)
    Q_PROPERTY(QString newRecoveryKey READ newRecoveryKey NOTIFY secretsChanged)
    Q_PROPERTY(QString mfaSecret READ mfaSecret NOTIFY secretsChanged)
    Q_PROPERTY(QString mfaUri READ mfaUri NOTIFY secretsChanged)
    Q_PROPERTY(QString avatarUrl READ avatarUrl NOTIFY accountChanged)
    Q_PROPERTY(bool pro READ pro NOTIFY accountChanged)
    Q_PROPERTY(QString plan READ plan NOTIFY accountChanged)
    /// "free", "pro" or "premium".
    Q_PROPERTY(QString tier READ tier NOTIFY accountChanged)
    Q_PROPERTY(bool premium READ premium NOTIFY accountChanged)
    /// Share when you're online with your contacts (and see theirs). Off by default.
    Q_PROPERTY(bool showOnlineStatus READ showOnlineStatus NOTIFY accountChanged)
    /// Show the ciphertext of your outgoing messages in the public transparency feed. Off by default.
    Q_PROPERTY(bool transparencyMode READ transparencyMode NOTIFY accountChanged)
    /// The plan chooser: Free, Pro and Premium, each a map of tier, name, price, per, includes,
    /// current, status, action ("checkout", "switch", "cancel", "keep" or ""), actionLabel, confirm and note.
    Q_PROPERTY(QVariantList planChoices READ planChoices NOTIFY planChanged)
    Q_PROPERTY(QString planPriceNote READ planPriceNote CONSTANT)
    /// Pro only: "1.2 MB of 10 GB used · 9.9 GB left", empty until loaded.
    Q_PROPERTY(QString storageText READ storageText NOTIFY storageChanged)
    Q_PROPERTY(double storageFraction READ storageFraction NOTIFY storageChanged)
    Q_PROPERTY(bool storageFull READ storageFull NOTIFY storageChanged)

public:
    AccountSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                   QObject* parent = nullptr);
    ~AccountSession() override;

    bool loaded() const { return loaded_; }
    bool mfaEnabled() const { return me_ ? me_->mfaEnabled : false; }
    bool hasRecoveryKey() const { return me_ ? me_->hasRecoveryKey : false; }
    bool acceptMessageRequests() const { return me_ ? me_->acceptMessageRequests : true; }
    bool readReceipts() const;
    bool busy() const { return busy_; }
    QString error() const { return error_; }
    QString notice() const { return notice_; }
    QString newRecoveryKey() const { return newRecoveryKey_; }
    QString mfaSecret() const { return mfaSecret_; }
    QString mfaUri() const { return mfaUri_; }
    QString avatarUrl() const;
    bool pro() const;
    QString plan() const;
    QString tier() const { return QString::fromStdString(tierId()); }
    std::string tierId() const { return me_ ? me_->subscriptionTier : identity_->subscriptionTier; }
    bool premium() const { return tierId() == "premium"; }
    bool showOnlineStatus() const { return me_ ? me_->showOnlineStatus : false; }
    bool transparencyMode() const { return me_ ? me_->transparencyMode : false; }
    QVariantList planChoices() const;
    QString planPriceNote() const;
    QString storageText() const;
    double storageFraction() const;
    bool storageFull() const { return storage_ && storage_->used >= storage_->limit; }

    Q_INVOKABLE void reload();
    Q_INVOKABLE void setReadReceipts(bool enabled);
    Q_INVOKABLE void setAcceptMessageRequests(bool enabled);
    Q_INVOKABLE void changePassword(const QString& current, const QString& next, const QString& confirm);
    Q_INVOKABLE void createRecoveryKey(const QString& password);
    Q_INVOKABLE void startMfaSetup();
    Q_INVOKABLE void confirmMfa(const QString& code);
    Q_INVOKABLE void disableMfa(const QString& password);
    /// A new handle (3-64 characters). Keys and chats stay the same; people find you by the new one.
    Q_INVOKABLE void changeHandle(const QString& handle);
    /// Deletes the account for good (the server checks the password), then signs out.
    Q_INVOKABLE void deleteAccount(const QString& password);
    /// Center-crops, scales to 128 px and re-encodes as JPEG (dropping EXIF: location, camera, time), then uploads.
    Q_INVOKABLE void uploadAvatar(const QUrl& imageFile);
    Q_INVOKABLE void removeAvatar();
    /// Does what the chooser's button for `tier` says (see planChoices): opens Stripe's
    /// checkout, switches the card subscription, switches to Free at the period's end, or keeps it.
    Q_INVOKABLE void choosePlan(const QString& tier);
    Q_INVOKABLE void setShowOnlineStatus(bool enabled);
    Q_INVOKABLE void setTransparencyMode(bool enabled);
    /// Forgets a shown-once secret (recovery key, two-factor setup key) once its dialog closes.
    Q_INVOKABLE void dismissSecrets();
    Q_INVOKABLE void clearMessages();
    /// Copies text, flagged so Windows keeps it out of clipboard history and cloud clipboard sync.
    Q_INVOKABLE static void copySecret(const QString& text);

signals:
    void accountChanged();
    void readReceiptsChanged();
    void busyChanged();
    void messagesChanged();
    void secretsChanged();
    void storageChanged();
    void planChanged();
    /// Whether this account shares its online status (LiveSession follows it).
    void onlineStatusChanged(bool shared);
    /// An action finished successfully (dialogs close or advance on this).
    void done(const QString& action);
    void sessionExpired();
    /// The handle was changed on the server.
    void handleChanged(const QString& handle);
    /// The account no longer exists: the app signs out.
    void accountDeleted();

private:
    template <typename Work, typename Done>
    void action(const QString& name, Work work, Done onDone);
    void setBusy(bool busy);
    void setMessages(const QString& error, const QString& notice);
    void reloadPlanDetails();
    model::plans::PlanInputs planInputs() const;
    /// The card subscription stops renewing; the paid plan lasts until the period ends, then Free.
    void switchToFree();
    /// Undoes switchToFree() while the paid period lasts.
    void keepPlan();
    /// Opens Stripe's checkout for `tier` in the browser, tagged with this account's id
    /// so the server's webhook knows whom to upgrade.
    void openCheckout(const std::string& tier);
    /// Checks every few seconds until the account is on `tier` (a checkout or a pending switch).
    void watchForPlan(const std::string& tier);
    void checkWatchedPlan();

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    std::shared_ptr<data::AccountRepository> account_;
    QThreadPool pool_;
    std::optional<api::MeResult> me_;
    std::optional<api::FileStorageUsageDto> storage_;
    std::optional<api::PlanStatusDto> planStatus_;
    QString paymentLink_;
    QString premiumPaymentLink_;
    std::optional<api::FilesStorageDto> filesStorage_;
    QTimer refreshTimer_;
    /// While a plan change is on its way (checkout in the browser, or a switch awaiting payment).
    QTimer planWatchTimer_;
    std::string watchedTier_;
    qint64 watchUntilMs_ = 0;
    bool loaded_ = false;
    bool planLoadedOnce_ = false;
    bool busy_ = false;
    QString error_;
    QString notice_;
    QString newRecoveryKey_;
    QString mfaSecret_;
    QString mfaUri_;
};

}  // namespace redvault::app
