// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/share_repository.hpp"

namespace redvault::app {

class FilesSession;

/**
 * Public share links for Files (docs/public-share-links.md): links anyone can
 * open in a browser, to a file or a folder, view-only or taking uploads; and
 * the files people upload through them, waiting in Incoming for review. Same
 * protocol as the web and Android apps (data/share_repository.hpp).
 *
 * Folder links follow Files: after every Files refresh the links' items are
 * brought in line (new files, renames, moves, the Trash). Incoming is checked
 * every minute; new arrivals raise the Files badge and, once per batch, a
 * notification naming the link (its name only lives on the owner's devices).
 */
class SharesSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(QVariantList links READ links NOTIFY changed)
    Q_PROPERTY(QVariantList incoming READ incoming NOTIFY changed)
    Q_PROPERTY(int incomingCount READ incomingCount NOTIFY changed)
    Q_PROPERTY(int activeLinks READ activeLinks NOTIFY changed)
    /// A link operation is running (Argon2id for passwords takes a moment).
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY messagesChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY messagesChanged)

public:
    SharesSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, FilesSession* files,
                  QString webAppBase, QObject* parent = nullptr);
    ~SharesSession() override;

    bool loaded() const { return loaded_; }
    QVariantList links() const;
    QVariantList incoming() const;
    int incomingCount() const { return static_cast<int>(incoming_.size()); }
    int activeLinks() const;
    bool busy() const { return busy_; }
    QString error() const { return error_; }
    QString notice() const { return notice_; }

    Q_INVOKABLE void refresh();
    /// The links made to one file or folder (for the Share dialog).
    Q_INVOKABLE QVariantList linksFor(const QString& itemId) const;
    /**
     * A new link. `options`: permission ("viewer"/"contributor"), label,
     * password (empty: none), expiresAtMs (0: never), maxDownloads,
     * maxFileSizeMb, allowanceMb, maxFiles (0 or empty: no limit).
     * Emits linkReady with its URL.
     */
    Q_INVOKABLE void createLink(const QString& itemId, const QVariantMap& options);
    Q_INVOKABLE void copyLink(const QString& linkId);
    /// Same keys as createLink, minus the password; `expiresAtMs` -1 keeps the current expiry.
    Q_INVOKABLE void saveSettings(const QString& linkId, const QVariantMap& options);
    Q_INVOKABLE void turnOffUploads(const QString& linkId);
    /// Empty removes the password.
    Q_INVOKABLE void setPassword(const QString& linkId, const QString& password);
    Q_INVOKABLE void regenerate(const QString& linkId, const QString& password);
    Q_INVOKABLE void revoke(const QString& linkId);
    Q_INVOKABLE void remove(const QString& linkId);

    /// Folders an incoming file can be added to: {id, name (path)}; "" is the top level.
    Q_INVOKABLE QVariantList folderChoices() const;
    Q_INVOKABLE void downloadIncoming(const QString& id, const QUrl& destination);
    Q_INVOKABLE void acceptIncoming(const QString& id, const QString& folderId);
    Q_INVOKABLE void rejectIncoming(const QString& id);
    /// A file type that can run code when opened (warn before saving one someone uploaded).
    Q_INVOKABLE bool isRisky(const QString& name) const;
    Q_INVOKABLE void clearMessages();
    /// While the Incoming page is on screen, new arrivals don't notify.
    Q_INVOKABLE void setIncomingVisible(bool visible);

signals:
    void changed();
    void busyChanged();
    void messagesChanged();
    /// A link was made (or remade): its address, for the dialog to show and copy.
    void linkReady(const QString& url, const QString& permission, bool hasPassword);
    /// Files arrived through links since the last check; `text` is ready to show.
    void incomingArrived(const QString& text);
    void sessionExpired();

private:
    void apply(data::ShareRepository::Listing listing);
    void reconcileSoon();
    const data::OwnerShare* findLink(const QString& id) const;
    const data::IncomingFile* findIncoming(const QString& id) const;
    void setBusy(bool busy);
    void setMessages(const QString& error, const QString& notice);
    template <typename Work, typename Done>
    void run(Work work, Done done, const QString& doneNotice = {});

    std::shared_ptr<data::ShareRepository> repository_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    QPointer<FilesSession> files_;
    QThreadPool pool_;
    QTimer checkTimer_;
    QTimer reconcileTimer_;
    bool loaded_ = false;
    bool busy_ = false;
    bool reconciling_ = false;
    bool incomingVisible_ = false;
    int generation_ = 0;
    std::string lastSignature_;
    QString error_;
    QString notice_;
    std::vector<data::OwnerShare> links_;
    std::vector<data::IncomingFile> incoming_;
};

}  // namespace redvault::app
