// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "updater.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QUrl>

#include <fstream>
#include <memory>

#include "app_controller.hpp"
#include "async.hpp"
#ifdef REDVAULT_HAS_SEALED_ROUTE
#include "redvault/e2ev3/sealed_route.hpp"
#endif
#include "redvault/crypto/base64.hpp"
#include "redvault/net/curl_transport.hpp"
#include "redvault/util/text.hpp"
#include "tor_controller.hpp"
#include "update_key.hpp"

#ifdef _WIN32
#include <windows.h>
#include <appmodel.h>
#endif

#ifndef REDVAULT_VERSION
#define REDVAULT_VERSION "0.0.0"
#endif

namespace redvault::app {

namespace fs = std::filesystem;

namespace {

Updater* gInstance = nullptr;

#ifdef REDVAULT_TOR_ONLY
constexpr bool kTorOnlyBuild = true;
#else
constexpr bool kTorOnlyBuild = false;
#endif

constexpr int kFirstCheckMs = 15'000;
constexpr int kRecheckMs = 12 * 60 * 60 * 1000;

/// Updates come from our own server only, like everything else the app fetches: over Tor, the app's own
/// Tor-only transport to the onion service (no other host, no direct connection); otherwise the website.
net::CurlTransport::Options updateTransportOptions(const AppController& app, const update::UpdateRoute& route) {
    if (route.viaTorToWebsite && app.tor() != nullptr) {
        // On the backup route: the published https files, through Tor to that one website (the same host as the
        // sealed gateway, the only one Tor lets the app reach outside onion services). Never without Tor.
        net::CurlTransport::Options options;
        if (const auto url = net::parseUrl(route.manifestUrl)) options.allowedHost = url->host;
        options.connectTimeout = std::chrono::seconds(90);
        options.stallTimeout = std::chrono::seconds(60);
        options.torGate = app.tor()->gate();
        options.httpsOverTor = true;
        return options;
    }
    if (route.viaTor) return app.transportOptions(std::chrono::seconds(90), std::chrono::seconds(60));
    net::CurlTransport::Options options;
    if (const auto url = net::parseUrl(route.manifestUrl)) options.allowedHost = url->host;
    return options;
}

/// How this copy was installed, which decides what an update downloads and how it's put in place.
enum class Kind { None, Windows, AppImage, Deb };

/// The packaged .deb puts the app in /opt/redvaultmessages (packaging/linux/make-deb.sh).
constexpr const char* kDebPrefix = "/opt/redvaultmessages/";
constexpr const char* kDebLauncher = "/usr/bin/redvaultmessages";

Kind detectKind() {
#ifdef _WIN32
    return Kind::Windows;
#elif defined(__linux__)
    // The Snap Store (Ubuntu App Center) updates the snap itself, and $SNAP is read-only.
    if (Updater::fromSnap()) return Kind::None;
    // The AppImage runtime names the file it was started from.
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (!appImage.isEmpty() && appImage.endsWith(QStringLiteral(".AppImage")) && QFileInfo(appImage).isFile()) return Kind::AppImage;
    // Installed by apt from our package (not just unpacked there by hand).
    if (QCoreApplication::applicationFilePath().startsWith(QLatin1String(kDebPrefix)) &&
        QFileInfo::exists(QStringLiteral("/var/lib/dpkg/info/redvaultmessages.list"))) {
        return Kind::Deb;
    }
    return Kind::None;
#else
    return Kind::None;
#endif
}

Kind kind() {
    static const Kind value = detectKind();
    return value;
}

const char* manifestUrl() {
    switch (kind()) {
        case Kind::AppImage: return update_config::kLinuxAppImageManifestUrl;
        case Kind::Deb: return update_config::kLinuxDebManifestUrl;
        default: return update_config::kManifestUrl;
    }
}

std::string_view product() {
    switch (kind()) {
        case Kind::AppImage: return update::kLinuxAppImageProduct;
        case Kind::Deb: return update::kLinuxDebProduct;
        default: return update::kProduct;
    }
}

fs::path toPath(const QString& path) {
#ifdef _WIN32
    return fs::path(path.toStdWString());
#else
    return fs::path(QFile::encodeName(path).toStdString());
#endif
}

std::optional<update::UpdateRoute> routeFor(const AppController& app) {
#ifdef REDVAULT_HAS_SEALED_ROUTE
    // The onion can't be reached (the app is on the sealed backup route): updates come from the website, over Tor,
    // but only when it is the gateway's own host, the one place outside onion services Tor lets the app go.
    if (app.sealedRoute_ && app.sealedRoute_->onBackup()) {
        const auto gateway = net::parseUrl(REDVAULT_SEALED_GATEWAY_URL);
        const auto website = net::parseUrl(manifestUrl());
        if (gateway && website && gateway->host == website->host) {
            if (auto route = update::websiteRouteOverTor(app.apiBase(), kTorOnlyBuild, manifestUrl(), update_config::kDownloadPrefix)) {
                return route;
            }
        }
    }
#endif
    return update::updateRoute(app.apiBase(), kTorOnlyBuild, manifestUrl(), update_config::kDownloadPrefix);
}

crypto::Bytes publicKey() {
    try {
        return crypto::base64Decode(update_config::kPublicKeyB64);
    } catch (const crypto::CryptoError&) {
        return {};
    }
}

}  // namespace

Updater::Updater(AppController* app, QObject* parent) : QObject(parent), app_(app) {
    pool_.setMaxThreadCount(1);
    if (!enabled()) {
        status_ = Status::Disabled;
        return;
    }
    timer_.setSingleShot(false);
    timer_.setInterval(kRecheckMs);
    connect(&timer_, &QTimer::timeout, this, &Updater::check);
    timer_.start();
    QTimer::singleShot(kFirstCheckMs, this, &Updater::check);
    // Over Tor, a check waits for the connection (nothing is sent while it's down): check once it's up.
    if (app_->tor() != nullptr && app_->tor()->enabled()) {
        connect(app_->tor(), &TorController::becameConnected, this, [this] {
            if (status_ == Status::Idle || status_ == Status::Failed) check();
        });
    }
}

Updater::~Updater() {
    pool_.clear();
    pool_.waitForDone();
}

Updater* Updater::create(QQmlEngine*, QJSEngine*) {
    QJSEngine::setObjectOwnership(gInstance, QJSEngine::CppOwnership);
    return gInstance;
}

void Updater::setInstance(Updater* instance) {
    gInstance = instance;
}

QString Updater::currentVersion() const {
    return QStringLiteral(REDVAULT_VERSION);
}

QString Updater::availableVersion() const {
    return release_ ? QString::fromStdString(release_->version) : QString();
}

QString Updater::notes() const {
    return release_ ? QString::fromStdString(release_->notes) : QString();
}

bool Updater::fromStore() {
#ifdef _WIN32
    // Only a packaged (MSIX) app has a package identity; the Inno Setup install doesn't.
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
#else
    return false;
#endif
}

bool Updater::fromSnap() {
#ifdef __linux__
    // snapd sets both for every app it starts.
    return !qEnvironmentVariable("SNAP").isEmpty() && !qEnvironmentVariable("SNAP_NAME").isEmpty();
#else
    return false;
#endif
}

bool Updater::blockedByTor() const {
    return !routeFor(*app_).has_value();
}

QString Updater::freshnessWarning() const {
    switch (freshness_) {
        case update::Freshness::Current: return {};
        case update::Freshness::Expired:
            return QStringLiteral("The update information from the server has expired, so there may be a newer version this app can't see. "
                                  "Check the website.");
        case update::Freshness::NoExpiry:
            return QStringLiteral("The update information from the server has no expiry date, so this app can't tell how recent it is.");
    }
    return {};
}

bool Updater::enabled() const {
#ifdef _WIN32
    // The Store forbids apps that update themselves, and would undo it anyway.
    return !blockedByTor() && !fromStore() && publicKey().size() == update::kPublicKeyBytes;
#else
    // Linux: an AppImage or our .deb package. Anything else (built from source, unpacked by hand) updates by hand.
    return kind() != Kind::None && !blockedByTor() && publicKey().size() == update::kPublicKeyBytes;
#endif
}

QString Updater::installKind() {
    switch (kind()) {
        case Kind::Windows: return fromStore() ? QString() : QStringLiteral("windows");
        case Kind::AppImage: return QStringLiteral("appimage");
        case Kind::Deb: return QStringLiteral("deb");
        case Kind::None: break;
    }
    return {};
}

void Updater::check() {
    if (!enabled() || status_ == Status::Checking || status_ == Status::Downloading || status_ == Status::Installing) {
        return;
    }
    // Once an update is found, keep offering it rather than re-checking over it.
    if (status_ == Status::Available) return;
    const auto route = routeFor(*app_);
    if (!route) return;
    // Tor isn't connected yet: nothing goes out, and becameConnected checks again.
    if (route->viaTor && (app_->tor() == nullptr || !app_->tor()->connected())) return;
    setStatus(Status::Checking);
    update::UpdateSource source{manifestUrl(), publicKey(), REDVAULT_VERSION, update_config::kDownloadPrefix,
                                app_->apiBase(), kTorOnlyBuild, QDateTime::currentMSecsSinceEpoch()};
    source.product = std::string(product());
    source.route = *route;
    runInBackground(
        pool_, this,
        [source, options = updateTransportOptions(*app_, *route)]() {
            net::CurlTransport transport(options);
            return update::fetchManifest(transport, source);
        },
        [this](update::ManifestCheck check) {
            const bool newlyExpired = check.freshness == update::Freshness::Expired && freshness_ != check.freshness;
            freshness_ = check.freshness;
            if (newlyExpired) emit manifestExpired(freshnessWarning());
            release_ = std::move(check.release);
            if (!release_) {
                setStatus(Status::UpToDate);
                return;
            }
            setStatus(Status::Available);
            const QString version = availableVersion();
            if (version != announcedVersion_) {
                announcedVersion_ = version;
                emit updateFound(version);
            }
        },
        [this](const Failure& failure) {
            // Offline or a bad manifest: say so in Settings, try again at the next check.
            setStatus(Status::Failed, failure.message);
        });
}

void Updater::install() {
    if (!enabled() || status_ != Status::Available || !release_) return;
    const auto route = routeFor(*app_);
    if (!route) return;
    // Over Tor, the onion service's copy of the signed installer (same size and SHA-256, checked below).
    const std::string url = update::installerUrl(*release_, *route, update_config::kDownloadPrefix);
    if (url.empty()) return;
    setStatus(Status::Downloading);
    const update::Release release = *release_;
    // Where the download goes. An AppImage's goes next to the running file (a hidden .part), so moving it over the
    // old one is a rename on the same filesystem; everything else gets a fresh folder of its own in the temp folder.
    fs::path file;
    bool ownFolder = true;
    if (kind() == Kind::AppImage) {
        file = toPath(qEnvironmentVariable("APPIMAGE")).parent_path() /
               (".redvaultmessages-" + release.version + "-update.AppImage.part");
        ownFolder = false;
    } else {
        const fs::path folder = toPath(QStandardPaths::writableLocation(QStandardPaths::TempLocation)) /
                                ("redvault-update-" + util::randomUuid());
        file = folder / (kind() == Kind::Deb ? "redvaultmessages_" + release.version + "_amd64.deb"
                                             : "redvaultmessages-Setup-" + release.version + ".exe");
    }
    runInBackground(
        pool_, this,
        [release, file, ownFolder, url, options = updateTransportOptions(*app_, *route)]() {
            const auto discard = [&] {
                std::error_code ignored;
                if (ownFolder) fs::remove_all(file.parent_path(), ignored);
                else fs::remove(file, ignored);
            };
            try {
                if (ownFolder) {
                    fs::create_directories(file.parent_path());
#ifndef _WIN32
                    // apt reads the package as its own user: let it, rather than warn.
                    fs::permissions(file.parent_path(), fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                                            fs::perms::others_read | fs::perms::others_exec);
#endif
                }
                {
                    std::ofstream out(file, std::ios::binary | std::ios::trunc);
                    if (!out) {
                        throw update::UpdateError(ownFolder ? std::string("Couldn't save the update")
                                                            : "Can't save the update next to the app, in " + file.parent_path().string() +
                                                                  ". Download the new version from the website instead.");
                    }
                    net::CurlTransport transport(options);
                    const auto response = transport.sendStreaming(net::HttpRequest{"GET", url, {}, {}}, {.download = &out});
                    if (response.status != 200) throw update::UpdateError("The download failed (" + std::to_string(response.status) + ")");
                }
                // Only the exact file the signed manifest describes is ever run or installed.
                if (!update::installerMatches(file, release)) {
                    throw update::UpdateError("The downloaded update didn't match its signature, so it wasn't installed");
                }
            } catch (...) {
                discard();
                throw;
            }
            return file;
        },
        [this](fs::path installer) {
            if (kind() != Kind::Windows) {
                finishLinuxInstall(installer);
                return;
            }
            setStatus(Status::Installing);
            // Per-user installer, silent: it closes this app if needed, replaces
            // it, and starts the new version when it's done.
            const bool started = QProcess::startDetached(
                QString::fromStdWString(installer.wstring()),
                {QStringLiteral("/SILENT"), QStringLiteral("/SUPPRESSMSGBOXES"), QStringLiteral("/NORESTART"),
                 QStringLiteral("/CLOSEAPPLICATIONS")});
            if (!started) {
                setStatus(Status::Failed, QStringLiteral("Couldn't start the installer"));
                return;
            }
            // Quitting signs out (keys and session live only in memory).
            app_->quit();
        },
        [this](const Failure& failure) {
            setStatus(Status::Available);
            error_ = failure.message;
            emit changed();
        });
}

void Updater::finishLinuxInstall(const std::filesystem::path& file) {
    const QString version = availableVersion();
    if (kind() == Kind::AppImage) {
        // Same folder, so this is a rename: the running copy keeps its (now unlinked) file until it quits.
        const QString current = qEnvironmentVariable("APPIMAGE");
        std::error_code error;
        fs::permissions(file, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                                  fs::perms::others_exec, error);
        if (!error) fs::rename(file, toPath(current), error);
        if (error) {
            std::error_code ignored;
            fs::remove(file, ignored);
            setStatus(Status::Available);
            error_ = QStringLiteral("Couldn't replace %1: %2").arg(current, QString::fromStdString(error.message()));
            emit changed();
            return;
        }
        setStatus(Status::Installing);
        if (!relaunchAfterQuit(current)) {
            setStatus(Status::Failed, QStringLiteral("Updated to %1. Close RedVaultMessages and open it again to use it.").arg(version));
            return;
        }
        // Quitting signs out (keys and session live only in memory).
        app_->quit();
        return;
    }

    // The .deb: apt installs it as root, after the system asks for the password (polkit's own prompt).
    setStatus(Status::Installing);
    const QString package = QString::fromStdString(file.string());
    auto* process = new QProcess(this);
    process->setProgram(QStringLiteral("pkexec"));
    process->setArguments({QStringLiteral("/usr/bin/apt-get"), QStringLiteral("install"), QStringLiteral("-y"), package});
    process->setProcessChannelMode(QProcess::MergedChannels);
    const auto openInSoftwareInstaller = [this, package] {
        // No way to ask for the password from here: hand the checked package to the desktop's own installer.
        QDesktopServices::openUrl(QUrl::fromLocalFile(package));
        setStatus(Status::Available);
        error_ = QStringLiteral("The update is open in your software installer: install it there, then open RedVaultMessages again.");
        emit changed();
    };
    connect(process, &QProcess::errorOccurred, this, [process, openInSoftwareInstaller](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        process->deleteLater();
        openInSoftwareInstaller();
    });
    connect(process, &QProcess::finished, this,
            [this, process, file, version, openInSoftwareInstaller](int code, QProcess::ExitStatus exitStatus) {
                process->deleteLater();
                const auto removeDownload = [&file] {
                    std::error_code ignored;
                    fs::remove_all(file.parent_path(), ignored);
                };
                if (exitStatus == QProcess::NormalExit && code == 0) {
                    removeDownload();
                    if (relaunchAfterQuit(QString::fromLatin1(kDebLauncher))) {
                        app_->quit();
                    } else {
                        setStatus(Status::Failed, QStringLiteral("Updated to %1. Close RedVaultMessages and open it again to use it.").arg(version));
                    }
                    return;
                }
                // pkexec: 126, the password prompt was closed; 127, not allowed or no prompt available on this desktop.
                if (exitStatus == QProcess::NormalExit && code == 127) {
                    openInSoftwareInstaller();
                    return;
                }
                removeDownload();
                setStatus(Status::Available);
                error_ = code == 126 ? QStringLiteral("The update wasn't installed: the password prompt was closed.")
                                     : QStringLiteral("The system couldn't install the update (apt-get stopped with %1).").arg(code);
                emit changed();
            });
    process->start();
}

bool Updater::relaunchAfterQuit(const QString& program) {
    // An AppImage's runtime points variables (PATH, plugin paths...) into its own mount, which goes away
    // with this process: the new copy starts with none of that.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString appDir = env.value(QStringLiteral("APPDIR"));
    if (!appDir.isEmpty()) {
        for (const QString& key : env.keys()) {
            const QString value = env.value(key);
            if (!value.contains(appDir)) continue;
            QStringList kept;
            for (const QString& part : value.split(QLatin1Char(':'))) {
                if (!part.contains(appDir)) kept << part;
            }
            if (kept.isEmpty() || kept.join(QString()).isEmpty()) env.remove(key);
            else env.insert(key, kept.join(QLatin1Char(':')));
        }
    }
    for (const char* key : {"APPDIR", "APPIMAGE", "ARGV0", "OWD"}) env.remove(QString::fromLatin1(key));

    // Waits (up to a minute) for this process to be gone, so the new one never meets the old one's locks.
    QProcess launcher;
    launcher.setProgram(QStringLiteral("/bin/sh"));
    launcher.setArguments({QStringLiteral("-c"),
                           QStringLiteral("i=0; while kill -0 \"$1\" 2>/dev/null && [ $i -lt 120 ]; do sleep 0.5; i=$((i+1)); done; "
                                          "exec \"$0\""),
                           program, QString::number(QCoreApplication::applicationPid())});
    launcher.setProcessEnvironment(env);
    launcher.setWorkingDirectory(QDir::homePath());
    launcher.setStandardOutputFile(QProcess::nullDevice());
    launcher.setStandardErrorFile(QProcess::nullDevice());
    return launcher.startDetached();
}

void Updater::setStatus(Status status, const QString& error) {
    status_ = status;
    error_ = error;
    emit changed();
}

}  // namespace redvault::app
