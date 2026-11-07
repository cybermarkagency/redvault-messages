#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <optional>

#include "redvault/update/update.hpp"

class QQmlEngine;
class QJSEngine;

namespace redvault::app {

class AppController;

/**
 * Checks for, downloads and installs signed updates (exposed to QML as the
 * `Updater` singleton). Checks shortly after start and every 12 hours: a
 * plain GET with no cookie, no identifying headers, and nothing about the
 * user, and never at all from a Tor-only build or against an onion service
 * (the check would leave over the clear internet, and it offers the normal
 * edition). Everything is verified in the core (redvault/update/update.hpp)
 * before it's trusted: the manifest's Ed25519 signature against the key
 * built into this app, newer version only, our download folder only, and
 * the installer's exact size and SHA-256. Installing runs the per-user
 * installer silently and quits (signing out), and the installer starts the
 * new version.
 */
class Updater : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Updater)
    QML_SINGLETON
    Q_PROPERTY(Status status READ status NOTIFY changed)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY changed)
    Q_PROPERTY(QString notes READ notes NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled CONSTANT)
    /// Installed from the Microsoft Store, which does the updating instead.
    Q_PROPERTY(bool fromStore READ fromStore CONSTANT)
    /// A Tor-only build, or a server on an onion address: this app never contacts the update server.
    Q_PROPERTY(bool blockedByTor READ blockedByTor CONSTANT)
    /// Non-empty when the signed update information is expired or has no expiry date.
    Q_PROPERTY(QString freshnessWarning READ freshnessWarning NOTIFY changed)

public:
    enum class Status { Disabled, Idle, Checking, UpToDate, Available, Downloading, Installing, Failed };
    Q_ENUM(Status)

    explicit Updater(AppController* app, QObject* parent = nullptr);
    ~Updater() override;

    static Updater* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    static void setInstance(Updater* instance);

    Status status() const { return status_; }
    QString currentVersion() const;
    QString availableVersion() const;
    QString notes() const;
    QString error() const { return error_; }
    bool enabled() const;
    static bool fromStore();
    bool blockedByTor() const;
    QString freshnessWarning() const;

    Q_INVOKABLE void check();
    Q_INVOKABLE void install();

signals:
    void changed();
    /// A newer version was found (once per version), for a tray notification.
    void updateFound(const QString& version);
    /// The signed update information has expired: someone may be holding back a newer version (once per run).
    void manifestExpired(const QString& warning);

private:
    void setStatus(Status status, const QString& error = {});

    AppController* app_;
    QThreadPool pool_;
    QTimer timer_;
    Status status_ = Status::Idle;
    QString error_;
    std::optional<update::Release> release_;
    update::Freshness freshness_ = update::Freshness::Current;
    QString announcedVersion_;
};

}  // namespace redvault::app
