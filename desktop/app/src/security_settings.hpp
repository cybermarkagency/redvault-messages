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

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "redvault/api/api_client.hpp"
#include "redvault/api/live_api.hpp"
#include "redvault/data/auth_repository.hpp"

namespace redvault::app {

class ChatSession;

/**
 * Settings › Linked devices: everywhere this account is signed in. The
 * server keeps only an id and the day each sign-in expires; the names come
 * from the account's own devices, encrypted with a key derived from the
 * private key (the web app's LinkedDevices.tsx).
 */
class LinkedDevices : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY devicesChanged)
    Q_PROPERTY(QString error READ error NOTIFY devicesChanged)

public:
    LinkedDevices(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, QObject* parent = nullptr);

    QVariantList devices() const { return devices_; }
    bool loaded() const { return loaded_; }
    QString error() const { return error_; }

    Q_INVOKABLE void reload();
    Q_INVOKABLE void rename(const QString& name);
    Q_INVOKABLE void signOut(const QString& sessionId);

signals:
    void devicesChanged();

private:
    void saveLabel(const QString& name, bool reportFailure);

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    QThreadPool pool_;
    QVariantList devices_;
    bool loaded_ = false;
    bool namedOnce_ = false;
    QString error_;
};

/**
 * Settings › Transparency log, checked on this computer (the web app's
 * TransparencyLog.tsx and lib/transparencyLog.ts): the checkpoint's
 * signature under a log key pinned on first use, that the log only grew
 * since last time (a consistency proof), an inclusion proof for every
 * listed entry, and that your own recent messages are in it, using
 * commitments computed here.
 */
class TransparencyLog : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    Q_PROPERTY(bool disabled READ disabled NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantMap checkpoint READ checkpoint NOTIFY changed)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(QVariantList pendingMine READ pendingMine NOTIFY changed)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY changed)
    Q_PROPERTY(bool onlyMine READ onlyMine WRITE setOnlyMine NOTIFY changed)
    Q_PROPERTY(bool live READ live WRITE setLive NOTIFY changed)
    Q_PROPERTY(bool checkingMine READ checkingMine NOTIFY changed)
    Q_PROPERTY(int mineMatched READ mineMatched NOTIFY changed)
    Q_PROPERTY(int mineMismatches READ mineMismatches NOTIFY changed)
    Q_PROPERTY(int failedRows READ failedRows NOTIFY changed)
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY selectedChanged)

public:
    TransparencyLog(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, ChatSession* chats,
                    QObject* parent = nullptr);
    ~TransparencyLog() override;

    bool disabled() const { return disabled_; }
    QString error() const { return error_; }
    QVariantMap checkpoint() const { return checkpointView_; }
    QVariantList entries() const;
    QVariantList pendingMine() const;
    QString search() const { return search_; }
    void setSearch(const QString& search);
    bool onlyMine() const { return onlyMine_; }
    void setOnlyMine(bool on);
    bool live() const { return live_; }
    void setLive(bool on);
    bool checkingMine() const { return checkingMine_; }
    int mineMatched() const;
    int mineMismatches() const;
    int failedRows() const;
    QVariantMap selected() const { return selected_; }

    /// Starts checking (and polling while `live`); call when the page opens.
    Q_INVOKABLE void activate(bool active);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void acceptNewKey();
    Q_INVOKABLE void select(qint64 index);
    Q_INVOKABLE void recheckSelected();
    Q_INVOKABLE void copySelectedProof();
    Q_INVOKABLE void exportTo(const QUrl& file);

signals:
    void changed();
    void selectedChanged();

private:
    struct Mine {
        std::string messageId;
        std::string commitment;
        std::optional<std::int64_t> logIndex;
        QString status;  // "match", "pending", "mismatch"
    };
    void verifyRows();
    void checkMine();
    void loadMineRows();
    QString proofKey(std::int64_t index) const;
    const std::vector<api::live::LogEntry>& rows() const { return onlyMine_ ? mineRows_ : entries_; }

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    QPointer<ChatSession> chats_;
    QThreadPool pool_;
    QTimer liveTimer_;
    bool active_ = false;
    bool disabled_ = false;
    QString error_;
    std::optional<api::live::LogCheckpoint> checkpoint_;
    QVariantMap checkpointView_;
    std::vector<api::live::LogEntry> entries_;
    std::vector<api::live::LogEntry> mineRows_;
    std::vector<Mine> mine_;
    std::map<QString, bool> proofs_;  // "index@size:root" -> checked ok
    QString search_;
    bool onlyMine_ = false;
    bool live_ = true;
    bool checkingMine_ = false;
    bool verifyingRows_ = false;
    std::int64_t mineCheckedAt_ = -1;
    QVariantMap selected_;
    int generation_ = 0;
};

/**
 * Settings › Relay servers: the message relay (this server's API) and the
 * TURN relays calls go through, with a reachability check that only asks
 * each relay for an address (no media, no identity).
 */
class RelayCheck : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    Q_PROPERTY(QString apiHost READ apiHost CONSTANT)
    Q_PROPERTY(QString apiBase READ apiBase CONSTANT)
    Q_PROPERTY(QString apiStatus READ apiStatus NOTIFY changed)
    Q_PROPERTY(QString apiTone READ apiTone NOTIFY changed)
    Q_PROPERTY(QVariantList relays READ relays NOTIFY changed)
    Q_PROPERTY(QString relayError READ relayError NOTIFY changed)

public:
    RelayCheck(std::shared_ptr<api::ApiClient> api, QObject* parent = nullptr);
    ~RelayCheck() override;

    QString apiHost() const;
    QString apiBase() const { return QString::fromStdString(api_->baseUrl()); }
    QString apiStatus() const { return apiStatus_; }
    QString apiTone() const { return apiTone_; }
    QVariantList relays() const;
    QString relayError() const { return relayError_; }

    Q_INVOKABLE void load();
    Q_INVOKABLE void checkAll();

signals:
    void changed();

private:
    struct Relay {
        std::string url;
        QString status;
        QString tone;
    };
    std::shared_ptr<api::ApiClient> api_;
    QThreadPool pool_;
    std::optional<api::live::IceServer> server_;
    std::vector<Relay> relays_;
    QString relayError_;
    QString apiStatus_ = QStringLiteral("Not checked");
    QString apiTone_ = QStringLiteral("muted");
};

}  // namespace redvault::app
