// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

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
 * user. Talking to the onion service (every Tor-only build, docs/tor-by-default.md)
 * the manifest and installer come from that onion service, through the same
 * Tor transport as everything else, and never over the normal internet; while
 * Tor isn't connected nothing is checked. Everything is verified in the core (redvault/update/update.hpp)
 * before it's trusted: the manifest's Ed25519 signature against the key
 * built into this app, newer version only, our download folder only, and
 * the installer's exact size and SHA-256. Installing runs the per-user
 * installer silently and quits (signing out), and the installer starts the
 * new version.
 *
 * On Linux it depends on how the app was installed (installKind):
 * - "appimage": the new .AppImage is downloaded next to the running one, checked,
 *   and moved over it; the app then restarts from it. No password needed, but the
 *   folder must be writable.
 * - "deb": the new .deb is downloaded and checked, then installed with
 *   `pkexec apt-get install`, which asks for the computer's password; the app
 *   restarts once it's in. Without pkexec it's opened in the software installer.
 * - "": anything else (built from source, unpacked by hand): no updates, as before.
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
    /// Installed as a snap (Snap Store / Ubuntu App Center), which does the updating instead.
    Q_PROPERTY(bool fromSnap READ fromSnap CONSTANT)
    /// No update route that stays inside Tor (a Tor-only build pointed at something that isn't an onion service): nothing is checked.
    Q_PROPERTY(bool blockedByTor READ blockedByTor CONSTANT)
    /// Non-empty when the signed update information is expired or has no expiry date.
    Q_PROPERTY(QString freshnessWarning READ freshnessWarning NOTIFY changed)
    /// How this copy updates itself: "windows", "appimage", "deb", or "" (it doesn't).
    Q_PROPERTY(QString installKind READ installKind CONSTANT)

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
    static bool fromSnap();
    bool blockedByTor() const;
    QString freshnessWarning() const;
    static QString installKind();

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
    /// Linux: the checked file is in place (AppImage) or ready to install (.deb).
    void finishLinuxInstall(const std::filesystem::path& file);
    /// Starts `program` once this process has gone, without anything from the old AppImage's environment.
    static bool relaunchAfterQuit(const QString& program);

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
