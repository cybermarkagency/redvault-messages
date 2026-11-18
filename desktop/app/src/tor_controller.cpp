// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "tor_controller.hpp"

#include <QDir>
#include <QMetaObject>
#include <QSettings>
#include <QFileInfo>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#ifdef REDVAULT_HAS_TOR
#include "redvault_tor.h"
#endif

namespace redvault::app {

namespace {

#ifdef REDVAULT_HAS_TOR
QJsonObject takeJson(char* text) {
    const QJsonObject object = text != nullptr ? QJsonDocument::fromJson(QByteArray(text)).object() : QJsonObject{};
    if (text != nullptr) rv_tor_free_string(text);
    return object;
}
#endif

}  // namespace

TorController::TorController(QString onionHost, QString stateDir, QObject* parent)
    : QObject(parent), onionHost_(std::move(onionHost)), stateDir_(std::move(stateDir)) {
    onionOnly_->store(QSettings().value(QStringLiteral("network/onionOnly"), false).toBool());
    bridgeMode_ = QSettings().value(QStringLiteral("network/bridgeMode"), QStringLiteral("off")).toString();
    bridgeLines_ = QSettings().value(QStringLiteral("network/bridgeLines")).toString();
    if (!enabled()) return;
    // Arti's guard state and directory cache live with the app's data, readable by this user only.
    // Arti refuses state that other users could read or change: owner-only.
    for (const QString& dir : {stateDir_, stateDir_ + QStringLiteral("/state"), stateDir_ + QStringLiteral("/cache")}) {
        QDir().mkpath(dir);
        QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    connect(&timer_, &QTimer::timeout, this, &TorController::poll);
    startGateway();
}

void TorController::startGateway() {
    state_ = QStringLiteral("connecting");
    progress_ = 0;
    detail_ = bridgeMode_ == QStringLiteral("off") ? QStringLiteral("Starting Tor") : QStringLiteral("Starting Tor with bridges");
    emit changed();
#ifdef REDVAULT_HAS_TOR
    const QString stateDir = stateDir_;
    QJsonObject config{{QStringLiteral("stateDir"), QDir::toNativeSeparators(stateDir + QStringLiteral("/state"))},
                       {QStringLiteral("cacheDir"), QDir::toNativeSeparators(stateDir + QStringLiteral("/cache"))},
                       {QStringLiteral("allowedTargets"), QJsonArray{onionHost_ + QStringLiteral(":80")}}};
#ifdef REDVAULT_HAS_SEALED_ROUTE
    // The sealed backup route (docs/tor-sealed-fallback.md) reaches the website's gateway through a Tor exit:
    // that one https host is the only place outside onion services the gate will go to.
    const QUrl sealedGateway(QStringLiteral(REDVAULT_SEALED_GATEWAY_URL));
    if (sealedGateway.scheme() == QStringLiteral("https") && !sealedGateway.host().isEmpty())
        config.insert(QStringLiteral("allowedExitTargets"), QJsonArray{sealedGateway.host() + QStringLiteral(":443")});
#endif
    if (bridgeMode_ != QStringLiteral("off")) config.insert(QStringLiteral("bridges"), bridgesConfig(bridgeMode_, bridgeLines_));
    char* error = nullptr;
    gateway_ = rv_tor_start(QJsonDocument(config).toJson(QJsonDocument::Compact).constData(), &error);
    if (gateway_ == nullptr) {
        state_ = QStringLiteral("unavailable");
        detail_ = QStringLiteral("Tor couldn't start: %1").arg(error != nullptr ? QString::fromUtf8(error) : QString());
        if (error != nullptr) rv_tor_free_string(error);
        return;
    }
    const QJsonObject endpoint = takeJson(rv_tor_endpoint(gateway_));
    {
        std::lock_guard lock(shared_->mutex);
        shared_->endpoint = net::SocksEndpoint{endpoint.value(QStringLiteral("host")).toString().toStdString(),
                                               endpoint.value(QStringLiteral("port")).toInt(),
                                               endpoint.value(QStringLiteral("username")).toString().toStdString(),
                                               endpoint.value(QStringLiteral("password")).toString().toStdString()};
    }
    timer_.start(1000);
    poll();
#else
    // A .onion server but no gateway in this build: fail closed, never fall back.
    state_ = QStringLiteral("unavailable");
    detail_ = QStringLiteral("This build has no Tor gateway");
#endif
}

TorController::~TorController() {
    stop();
}

void TorController::stop() {
    timer_.stop();
    {
        std::lock_guard lock(shared_->mutex);
        shared_->usable = false;
        shared_->endpoint.reset();
    }
#ifdef REDVAULT_HAS_TOR
    if (gateway_ != nullptr) {
        rv_tor_stop(gateway_);
        gateway_ = nullptr;
    }
#endif
    if (enabled() && state_ != QStringLiteral("unavailable")) {
        state_ = QStringLiteral("unavailable");
        detail_ = QStringLiteral("Tor is stopped");
        emit changed();
    }
}

std::function<std::optional<net::SocksEndpoint>()> TorController::gate() const {
    auto shared = shared_;
    return [shared]() -> std::optional<net::SocksEndpoint> {
        std::lock_guard lock(shared->mutex);
        if (!shared->usable) return std::nullopt;
        return shared->endpoint;
    };
}

void TorController::setQueued(int messages, int files) {
    if (messages == queued_ && files == pausedUploads_) return;
    queued_ = messages;
    pausedUploads_ = files;
    emit queuedChanged();
}

namespace {

/// Where this copy keeps the transport programs: next to the app (Windows,
/// a Linux tarball), or in the package's library folder (AppImage, .deb, snap).
QString transportProgram(const QString& name) {
#ifdef Q_OS_WIN
    const QString file = name + QStringLiteral(".exe");
#else
    const QString file = name;
#endif
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString& dir : {appDir + QStringLiteral("/pt"), appDir + QStringLiteral("/../lib/redvaultmessages/pt"),
                               appDir + QStringLiteral("/../lib/pt")}) {
        const QFileInfo info(dir + QLatin1Char('/') + file);
        if (info.isFile() && info.isExecutable()) return QDir::toNativeSeparators(info.canonicalFilePath());
    }
    return {};
}

const QStringList kBridgeModes{QStringLiteral("off"), QStringLiteral("snowflake"), QStringLiteral("obfs4"), QStringLiteral("custom")};

}  // namespace

bool TorController::bridgesAvailable() const {
    return !transportProgram(QStringLiteral("lyrebird")).isEmpty() && !transportProgram(QStringLiteral("snowflake-client")).isEmpty();
}

QJsonObject TorController::bridgesConfig(const QString& mode, const QString& lines) const {
    QJsonObject bridges{{QStringLiteral("mode"), mode}};
    if (mode == QStringLiteral("custom")) bridges.insert(QStringLiteral("lines"), QJsonArray::fromStringList(lines.split(QLatin1Char('\n'))));
    const QString lyrebird = transportProgram(QStringLiteral("lyrebird"));
    const QString snowflake = transportProgram(QStringLiteral("snowflake-client"));
    if (!lyrebird.isEmpty()) bridges.insert(QStringLiteral("lyrebirdPath"), lyrebird);
    if (!snowflake.isEmpty()) bridges.insert(QStringLiteral("snowflakePath"), snowflake);
    return bridges;
}

QString TorController::setBridges(const QString& mode, const QString& lines) {
    if (!kBridgeModes.contains(mode)) return QStringLiteral("Choose how to connect.");
    if (!enabled()) return QStringLiteral("This copy doesn't use Tor.");
    const QString trimmed = mode == QStringLiteral("custom") ? lines.trimmed() : QString();
#ifdef REDVAULT_HAS_TOR
    if (mode != QStringLiteral("off")) {
        if (!bridgesAvailable()) return QStringLiteral("This copy of the app doesn't include bridge support.");
        const QByteArray json = QJsonDocument(bridgesConfig(mode, trimmed)).toJson(QJsonDocument::Compact);
        const QJsonObject result = takeJson(rv_tor_validate_bridges(json.constData()));
        if (!result.value(QStringLiteral("ok")).toBool())
            return result.value(QStringLiteral("error")).toString(QStringLiteral("Those bridges can't be used."));
    }
#endif
    if (mode == bridgeMode_ && trimmed == bridgeLines_) return {};
    bridgeMode_ = mode;
    bridgeLines_ = trimmed;
    QSettings().setValue(QStringLiteral("network/bridgeMode"), bridgeMode_);
    QSettings().setValue(QStringLiteral("network/bridgeLines"), bridgeLines_);
    emit bridgesChanged();
    // Restart Tor with the new way in: requests wait (queued) meanwhile, never leave another way.
    stop();
    startGateway();
    return {};
}

void TorController::poll() {
#ifdef REDVAULT_HAS_TOR
    if (gateway_ == nullptr) return;
    const QJsonObject status = takeJson(rv_tor_status(gateway_));
    const QString state = status.value(QStringLiteral("state")).toString(QStringLiteral("unavailable"));
    const int progress = status.value(QStringLiteral("progress")).toInt();
    const QString detail = status.value(QStringLiteral("detail")).toString();
    const bool wasConnected = connected();
    // Requests may go out while Tor is connected, and also while it is still
    // connecting (they simply wait inside Tor): only "unavailable" stops them.
    // Either way they can only ever leave through the gateway.
    {
        std::lock_guard lock(shared_->mutex);
        shared_->usable = state != QStringLiteral("unavailable");
    }
    connectedFlag_->store(state == QStringLiteral("connected"));
    if (state == state_ && progress == progress_ && detail == detail_) return;
    state_ = state;
    progress_ = progress;
    detail_ = detail;
    emit changed();
    if (!wasConnected && connected()) emit becameConnected();
#endif
}

}  // namespace redvault::app

namespace redvault::app {

// --- The sealed backup route (docs/tor-sealed-fallback.md) ---

bool TorController::backupRouteAvailable() const {
#ifdef REDVAULT_SEALED_KEY_CONFIG
    return enabled() && sizeof(REDVAULT_SEALED_KEY_CONFIG) > 1;
#else
    return false;
#endif
}

void TorController::setOnionOnly(bool onionOnly) {
    if (onionOnly_->exchange(onionOnly) == onionOnly) return;
    QSettings().setValue(QStringLiteral("network/onionOnly"), onionOnly);
    if (onionOnly) emit onionOnlyChosen();
    emit routeChanged();
}

namespace {
// Not secret: only which route worked last, so the next start doesn't wait for the onion to fail again.
const QString kLastRouteBackupKey = QStringLiteral("network/lastRouteBackup");
}  // namespace

bool TorController::lastRouteWasBackup() const {
    return backupRouteAvailable() && !onionOnly() && QSettings().value(kLastRouteBackupKey, false).toBool();
}

void TorController::reportBackupRoute(bool backup) {
    QMetaObject::invokeMethod(
        this,
        [this, backup] {
            QSettings().setValue(kLastRouteBackupKey, backup);
            if (backupRoute_ == backup) return;
            backupRoute_ = backup;
            emit routeChanged();
        },
        Qt::QueuedConnection);
}

}  // namespace redvault::app
