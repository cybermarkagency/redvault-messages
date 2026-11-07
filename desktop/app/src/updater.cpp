#include "updater.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QJSEngine>
#include <QProcess>
#include <QQmlEngine>
#include <QStandardPaths>

#include <fstream>
#include <memory>

#include "app_controller.hpp"
#include "async.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/net/curl_transport.hpp"
#include "redvault/util/text.hpp"
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

/// Updates come from our own server only, like everything else the app fetches.
net::CurlTransport::Options updateTransportOptions() {
    net::CurlTransport::Options options;
    if (const auto url = net::parseUrl(update_config::kManifestUrl)) options.allowedHost = url->host;
    return options;
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

bool Updater::blockedByTor() const {
    return !update::updatesPermitted(app_->apiBase(), kTorOnlyBuild);
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
    // Updates are Windows installers; other platforms update by hand.
    return false;
#endif
}

void Updater::check() {
    if (!enabled() || status_ == Status::Checking || status_ == Status::Downloading || status_ == Status::Installing) {
        return;
    }
    // Once an update is found, keep offering it rather than re-checking over it.
    if (status_ == Status::Available) return;
    setStatus(Status::Checking);
    update::UpdateSource source{update_config::kManifestUrl, publicKey(), REDVAULT_VERSION, update_config::kDownloadPrefix,
                                app_->apiBase(), kTorOnlyBuild, QDateTime::currentMSecsSinceEpoch()};
    runInBackground(
        pool_, this,
        [source]() {
            net::CurlTransport transport(updateTransportOptions());
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
    setStatus(Status::Downloading);
    const update::Release release = *release_;
    const fs::path folder = fs::path(QStandardPaths::writableLocation(QStandardPaths::TempLocation).toStdWString()) /
                            ("redvault-update-" + util::randomUuid());
    runInBackground(
        pool_, this,
        [release, folder]() {
            fs::create_directories(folder);
            const fs::path installer = folder / ("redvaultmessages-Setup-" + release.version + ".exe");
            {
                std::ofstream out(installer, std::ios::binary | std::ios::trunc);
                if (!out) throw update::UpdateError("Couldn't save the update");
                net::CurlTransport transport(updateTransportOptions());
                const auto response = transport.sendStreaming(net::HttpRequest{"GET", release.url, {}, {}}, {.download = &out});
                if (response.status != 200) throw update::UpdateError("The download failed (" + std::to_string(response.status) + ")");
            }
            // Only the exact installer the signed manifest describes is ever run.
            if (!update::installerMatches(installer, release)) {
                std::error_code ignored;
                fs::remove_all(folder, ignored);
                throw update::UpdateError("The downloaded update didn't match its signature, so it wasn't installed");
            }
            return installer;
        },
        [this](fs::path installer) {
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

void Updater::setStatus(Status status, const QString& error) {
    status_ = status;
    error_ = error;
    emit changed();
}

}  // namespace redvault::app
