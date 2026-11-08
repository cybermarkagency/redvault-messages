// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "redvault/api/api_client.hpp"

namespace redvault::app {

/**
 * The Inbox: messages and polls the Redvault team sends from the web admin
 * panel (/api/announcements), the desktop counterpart of the web app's
 * screens/Announcements.tsx. Unlike chats these are NOT end-to-end
 * encrypted (the server counts votes), and the page says so. Checked every
 * minute for the nav badge; opening the page marks everything read.
 */
class InboxSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    /// Newest first: {id, kind ("message"|"poll"), body, allowReplies, when, read, options [{id,label}],
    /// myOptionId, replies [{id, body, when}], repliesLoaded}.
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(int unread READ unread NOTIFY itemsChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY itemsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit InboxSession(std::shared_ptr<api::ApiClient> api, QObject* parent = nullptr);
    ~InboxSession() override;

    QVariantList items() const { return items_; }
    int unread() const;
    bool loaded() const { return loaded_; }
    bool busy() const { return busy_; }
    QString error() const { return error_; }

    Q_INVOKABLE void refresh();
    /// The page is open: everything in it counts as read (like the web app).
    Q_INVOKABLE void markAllRead();
    /// Fetches an item's replies (only items that allow them have any).
    Q_INVOKABLE void loadReplies(const QString& id);
    /// One vote per poll, final.
    Q_INVOKABLE void vote(const QString& id, const QString& optionId);
    /// Goes to the team only; nobody else sees it.
    Q_INVOKABLE void reply(const QString& id, const QString& body);
    Q_INVOKABLE void clearError();

signals:
    void itemsChanged();
    void busyChanged();
    void errorChanged();
    /// A reply was sent (the page clears its draft).
    void replied(const QString& id);
    void sessionExpired();

private:
    void setBusy(bool busy);
    void setError(const QString& error);
    void update(const QString& id, const QString& key, const QVariant& value);
    void fail(int status, const QString& message);

    std::shared_ptr<api::ApiClient> api_;
    QThreadPool pool_;
    QTimer timer_;
    QVariantList items_;
    bool loaded_ = false;
    bool busy_ = false;
    bool refreshing_ = false;
    QString error_;
};

}  // namespace redvault::app
