#include "tor_controller.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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
    : QObject(parent), onionHost_(std::move(onionHost)) {
    if (!enabled()) return;
    state_ = QStringLiteral("connecting");
    detail_ = QStringLiteral("Starting Tor");
#ifdef REDVAULT_HAS_TOR
    // Arti's guard state and directory cache live with the app's data, readable by this user only.
    // Arti refuses state that other users could read or change: owner-only.
    for (const QString& dir : {stateDir, stateDir + QStringLiteral("/state"), stateDir + QStringLiteral("/cache")}) {
        QDir().mkpath(dir);
        QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    QJsonObject config{{QStringLiteral("stateDir"), QDir::toNativeSeparators(stateDir + QStringLiteral("/state"))},
                       {QStringLiteral("cacheDir"), QDir::toNativeSeparators(stateDir + QStringLiteral("/cache"))},
                       {QStringLiteral("allowedTargets"), QJsonArray{onionHost_ + QStringLiteral(":80")}}};
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
    connect(&timer_, &QTimer::timeout, this, &TorController::poll);
    timer_.start(1000);
    poll();
#else
    Q_UNUSED(stateDir);
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
    if (state == state_ && progress == progress_ && detail == detail_) return;
    state_ = state;
    progress_ = progress;
    detail_ = detail;
    emit changed();
    if (!wasConnected && connected()) emit becameConnected();
#endif
}

}  // namespace redvault::app
