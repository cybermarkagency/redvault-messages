// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "async.hpp"
#include "redvault/api/api_client.hpp"

namespace redvault::app {

/**
 * The admin tab's Communities section for RedVault's super admins, the
 * desktop counterpart of the web app's screens/AdminCommunities.tsx: the
 * ledger overview and commission rule versions, transactions with refunds
 * and reconciliation, creators (verify, suspend), communities (suspend,
 * close) and the reports RedVault reviews. Made only for an account whose
 * role is super_admin; the server lets nobody else in either
 * (/api/admin/communities) and audits every change.
 *
 * Money is display only: every amount is the server's, in minor units, and
 * the only numbers sent are what the admin typed (a refund amount, a new
 * commission version), which the server checks against its own rules and
 * the ledger.
 */
class CommunityAdminSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in, for super admins")
    /// "overview", "transactions", "creators", "communities" or "reports".
    Q_PROPERTY(QString section READ section NOTIFY sectionChanged)
    /// AdminOverviewDto; empty until loaded.
    Q_PROPERTY(QVariantMap overview READ overview NOTIFY overviewChanged)
    /// AdminTransactionDto[], newest first, as the server sends them.
    Q_PROPERTY(QVariantList transactions READ transactions NOTIFY transactionsChanged)
    /// "", "pending", "matched" or "mismatch".
    Q_PROPERTY(QString transactionFilter READ transactionFilter NOTIFY transactionsChanged)
    Q_PROPERTY(QVariantList creators READ creators NOTIFY creatorsChanged)
    Q_PROPERTY(QVariantList communityList READ communityList NOTIFY communityListChanged)
    /// The open reports RedVault reviews.
    Q_PROPERTY(QVariantList reports READ reports NOTIFY reportsChanged)
    /// The section's list has loaded at least once.
    Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY messagesChanged)
    Q_PROPERTY(QString error READ error NOTIFY messagesChanged)

public:
    explicit CommunityAdminSession(std::shared_ptr<api::ApiClient> api, QObject* parent = nullptr);
    ~CommunityAdminSession() override;

    QString section() const { return section_; }
    QVariantMap overview() const { return overview_; }
    QVariantList transactions() const { return transactions_; }
    QString transactionFilter() const { return transactionFilter_; }
    QVariantList creators() const { return creators_; }
    QVariantList communityList() const { return communityList_; }
    QVariantList reports() const { return reports_; }
    bool loaded() const { return loaded_; }
    bool busy() const { return busy_; }
    QString notice() const { return notice_; }
    QString error() const { return error_; }

    /// Shows a section and (re)loads what it lists.
    Q_INVOKABLE void open(const QString& section);
    Q_INVOKABLE void reload();
    Q_INVOKABLE void clearMessages();

    // --- Overview ---
    /// A new commission version for memberships started from now on; existing memberships keep their rate.
    Q_INVOKABLE void addCommissionRule(const QString& ratePercent, const QString& minDollars, const QString& maxDollars, const QString& note);

    // --- Transactions ---
    Q_INVOKABLE void setTransactionFilter(const QString& filter);
    /// `amountDollars` empty = the whole payment. `reason` is required and kept in the audit log.
    Q_INVOKABLE void refund(const QString& transactionId, const QString& amountDollars, const QString& reason);
    /// Checks recorded transactions against Stripe now.
    Q_INVOKABLE void reconcile();

    // --- Creators and communities ---
    Q_INVOKABLE void setCreatorVerified(const QString& creatorId, bool verified);
    /// "active" (reinstate) or "suspended".
    Q_INVOKABLE void setCreatorStatus(const QString& creatorId, const QString& status);
    /// "suspend", "unsuspend" or "close" (immediately; every member loses access).
    Q_INVOKABLE void communityAction(const QString& communityId, const QString& action);

    // --- Reports ---
    /// "actioned" or "dismissed".
    Q_INVOKABLE void resolveReport(const QString& reportId, const QString& status);

signals:
    void sectionChanged();
    void overviewChanged();
    void transactionsChanged();
    void creatorsChanged();
    void communityListChanged();
    void reportsChanged();
    void loadedChanged();
    void busyChanged();
    void messagesChanged();
    void sessionExpired();

private:
    template <typename Work, typename Done>
    void act(Work work, Done done);
    void load(const QString& section);
    void setLoaded(bool loaded);
    void setBusy(bool busy);
    void setMessages(const QString& error, const QString& notice);
    void fail(const Failure& failure);

    std::shared_ptr<api::ApiClient> api_;
    QThreadPool pool_;
    QString section_ = QStringLiteral("overview");
    int generation_ = 0;
    QVariantMap overview_;
    QVariantList transactions_;
    QString transactionFilter_;
    QVariantList creators_;
    QVariantList communityList_;
    QVariantList reports_;
    bool loaded_ = false;
    bool busy_ = false;
    QString notice_;
    QString error_;
};

}  // namespace redvault::app
