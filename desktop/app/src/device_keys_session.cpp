// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "device_keys_session.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QLocale>
#include <QStandardPaths>

#include "async.hpp"

#ifdef REDVAULT_HAS_E2E_V3
#include <map>
#include <mutex>

#include "file_safety.hpp"
#include "redvault/e2ev3/key_vault.hpp"
#include "redvault/e2ev3/v3_server.hpp"
#include "redvault/util/text.hpp"
#endif

namespace redvault::app {

QVariant V3ChatModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
    return rows_.at(index.row()).toMap().value(QString::fromLatin1(roleNames().value(role)));
}

QHash<int, QByteArray> V3ChatModel::roleNames() const {
    return {{IdRole, "messageId"},       {TextRole, "text"},         {FromMeRole, "fromMe"},     {TimeRole, "time"},
            {StatusRole, "status"},      {SenderRole, "sender"},     {DeletedRole, "deleted"},   {EditedRole, "edited"},
            {ReactionsRole, "reactions"}, {HasFileRole, "hasFile"},  {FileNameRole, "fileName"}, {FileSizeRole, "fileSize"},
            {CanOpenRole, "canOpen"},    {CanReactRole, "canReact"}, {CanEditRole, "canEdit"},   {CanDeleteRole, "canDelete"}};
}

void V3ChatModel::setRows(QVariantList rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    emit countChanged();
}

#ifdef REDVAULT_HAS_E2E_V3

namespace {

using e2ev3::Json;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

QString str(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? qs(it->get<std::string>()) : QString();
}

/// What each security event means to the person reading it (the web panel's wording).
QString eventText(const QString& kind) {
    static const QHash<QString, QString> text = {
        {"device_registered", "A device registered its keys (trusted only once approved)"},
        {"device_list_created", "Device keys were set up for this account"},
        {"device_added", "A device was approved"},
        {"device_removed", "A device was removed"},
        {"device_unregistered", "A device's keys were deleted"},
        {"enrollment_requested", "A new device asked to pair"},
        {"account_key_rotated", "The account key was replaced by a trusted device"},
        {"account_reset_requested", "Someone asked to reset this account's keys"},
        {"account_reset_cancelled", "A reset of this account's keys was cancelled"},
        {"account_reset_completed", "This account's keys were reset"},
        {"recovery_backup_updated", "A recovery backup was saved"},
        {"recovery_backup_downloaded", "The recovery backup was downloaded"},
        {"signal_enabled", "A device turned on Signal sessions (development build)"},
    };
    return text.value(kind, kind);
}

/// An engine refusal worded for the person; the code stays available to the caller.
QString friendly(const e2ev3::EngineError& error) {
    const std::string& code = error.code();
    if (code == "bad_device_list" || code == "equivocation" || code == "rollback") {
        return QStringLiteral("This account's device list doesn't verify. Don't approve anything until you know why.");
    }
    if (code == "account_key_changed" || code == "uncertified_account_key") {
        return QStringLiteral("The account's device keys changed. Compare safety numbers before trusting the new keys.");
    }
    if (code == "bad_recovery_key" || code == "bad_backup") {
        return QStringLiteral("That recovery key doesn't open this account's backup. A recovery key has 64 letters and digits.");
    }
    if (code == "pairing_timeout") return QStringLiteral("The other device didn't answer in time. Start again.");
    if (code == "bad_commitment" || code == "bad_pairing") {
        return QStringLiteral("The pairing didn't check out, so nothing was approved. Start again.");
    }
    if (code == "legacy_key_mismatch") return QStringLiteral("The recovery backup doesn't match this account's current key.");
    return QString::fromUtf8(error.what());
}

/// Converts an engine refusal into an error the shared handler words for the UI.
template <typename F>
auto translated(F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const e2ev3::EngineError& error) {
        if (error.code() == "cancelled") throw api::UserFacingError("");
        throw api::UserFacingError(friendly(error).toStdString());
    }
}

QString pairKind(e2ev3::Protection p) {
    switch (p) {
        case e2ev3::Protection::Dpapi:
            return QStringLiteral("dpapi");
        case e2ev3::Protection::SecretService:
            return QStringLiteral("keyring");
        case e2ev3::Protection::File:
            return QStringLiteral("file");
    }
    return QStringLiteral("file");
}

e2ev3::WaitOptions waitFor(const std::shared_ptr<std::atomic<bool>>& stop) {
    e2ev3::WaitOptions wait;
    wait.cancelled = [stop] { return stop->load(); };
    return wait;
}

/// One chat row, as the web's toMessage: {emoji, count, mine} per emoji, and only the actions that message allows.
QVariantMap chatRow(const e2ev3::ChatRecord& r, const std::string& selfUserId, std::int64_t now) {
    constexpr std::int64_t kEditWindow = 24LL * 3600 * 1000;  // Signal's window, like the other chats
    const bool mine = r.direction == "out";
    const bool failed = r.status == "failed";
    std::map<QString, std::pair<int, bool>> grouped;
    for (const e2ev3::ChatReaction& reaction : r.reactions) {
        auto& held = grouped[qs(reaction.emoji)];
        ++held.first;
        held.second = held.second || reaction.userId == selfUserId;
    }
    QVariantList reactions;
    for (const auto& [emoji, held] : grouped) {
        reactions.append(QVariantMap{{"emoji", emoji}, {"count", held.first}, {"mine", held.second}});
    }
    const QString fileName = r.file ? qs(r.file->filename) : QString();
    return QVariantMap{{"messageId", qs(r.id)},
                       {"text", r.deleted ? QStringLiteral("This message was deleted.") : qs(r.text)},
                       {"fromMe", mine},
                       {"time", QLocale().toString(QDateTime::fromMSecsSinceEpoch(r.ts).toLocalTime(), QLocale::ShortFormat)},
                       {"status", qs(r.status)},
                       {"sender", qs(r.from)},
                       {"deleted", r.deleted},
                       {"edited", r.edited},
                       {"reactions", reactions},
                       {"hasFile", r.file.has_value()},
                       {"fileName", fileName},
                       {"fileSize", r.file ? qs(model::file_message::formatBytes(r.file->originalSize)) : QString()},
                       {"canOpen", r.file.has_value() && file_safety::isSafeToOpen(fileName)},
                       // A failed send never reached anyone, so nothing can react to it or edit it.
                       {"canReact", !failed && !r.deleted},
                       {"canEdit", mine && !failed && !r.deleted && !r.file && now - r.ts < kEditWindow},
                       {"canDelete", mine && !failed && !r.deleted}};
}

}  // namespace

struct DeviceKeysSession::Snapshot {
    bool enabled = false;
    QString status = QStringLiteral("off");
    std::shared_ptr<e2ev3::V3Device> device;
    QVariantList devices;
    QVariantList waiting;
    QVariantMap pendingReset;
    QVariantList events;
    QVariantMap protection;
    bool interrupted = false;
};

namespace {

std::mutex& storeMutex() {
    static std::mutex m;
    return m;
}

}  // namespace

#endif

DeviceKeysSession::DeviceKeysSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, QObject* parent)
    : QObject(parent), api_(std::move(api)), identity_(std::move(identity)) {
    pool_.setMaxThreadCount(1);
    waitPool_.setMaxThreadCount(2);
    chatPool_.setMaxThreadCount(1);
    receiveTimer_.setInterval(30000);
    connect(&receiveTimer_, &QTimer::timeout, this, &DeviceKeysSession::receiveNow);
#ifdef REDVAULT_HAS_E2E_V3
    server_ = std::make_shared<e2ev3::HttpV3Server>(api_);
    // Decrypted files go in a folder of their own for this session, removed at sign-out. No conversation
    // repository: nothing here goes through a protocol-1 message.
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    cacheDir_ = std::filesystem::path(cacheRoot.toStdWString()) / ("session-" + util::randomUuid());
    files_ = std::make_shared<data::FileRepository>(api_, nullptr, cacheDir_);
#endif
}

DeviceKeysSession::~DeviceKeysSession() {
#ifdef REDVAULT_HAS_E2E_V3
    stopWaiting_->store(true);
#endif
    receiveTimer_.stop();
    chatPool_.clear();
    chatPool_.waitForDone();
    pool_.clear();
    waitPool_.clear();
    pool_.waitForDone();
    waitPool_.waitForDone();
#ifdef REDVAULT_HAS_E2E_V3
    std::error_code ignored;
    std::filesystem::remove_all(cacheDir_, ignored);
#endif
}

void DeviceKeysSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit changed();
}

void DeviceKeysSession::setError(const QString& message) {
    if (error_ == message) return;
    error_ = message;
    emit changed();
}

void DeviceKeysSession::clearError() {
    setError({});
}

bool DeviceKeysSession::ready() const {
    return available() && status_ != QStringLiteral("off");
}

#ifndef REDVAULT_HAS_E2E_V3

void DeviceKeysSession::refresh() {}
void DeviceKeysSession::turnOffSignal(const QString&) {}
void DeviceKeysSession::setUp() {}
void DeviceKeysSession::startPairing() {}
void DeviceKeysSession::confirmJoin() {}
void DeviceKeysSession::cancelJoin() {}
void DeviceKeysSession::compareCodes(const QString&) {}
void DeviceKeysSession::approve() {}
void DeviceKeysSession::rejectAnswer() {}
void DeviceKeysSession::revoke(const QString&) {}
void DeviceKeysSession::removeWaiting(const QString&) {}
void DeviceKeysSession::createRecoveryKey() {}
void DeviceKeysSession::dismissRecoveryKey() {}
void DeviceKeysSession::restore(const QString&) {}
void DeviceKeysSession::cancelReset() {}
void DeviceKeysSession::forgetDevice() {}
void DeviceKeysSession::startOver() {}
void DeviceKeysSession::loadSafetyNumber(const QString& contactUserId, const QString&) {
    emit safetyNumberReady(contactUserId, {});
}
void DeviceKeysSession::setDeviceKeysVerified(const QString& contactUserId, const QString&, const QString&, bool) {
    emit safetyNumberReady(contactUserId, {});
}
void DeviceKeysSession::acceptNewDeviceKeys(const QString& contactUserId, const QString&, const QString&, bool) {
    emit safetyNumberReady(contactUserId, {});
}

bool DeviceKeysSession::chatPinned(const QString&) {
    return false;
}

void DeviceKeysSession::openChat(const QString&, const QString&, bool, bool) {}
void DeviceKeysSession::closeChat() {}
void DeviceKeysSession::useDeviceKeys() {}
void DeviceKeysSession::useOlderProtocol() {}
void DeviceKeysSession::sendChat(const QString&) {}
void DeviceKeysSession::retryChat(const QString&) {}
void DeviceKeysSession::reactChat(const QString&, const QString&) {}
void DeviceKeysSession::editChat(const QString&, const QString&) {}
void DeviceKeysSession::deleteChat(const QString&) {}
void DeviceKeysSession::sendFile(const QUrl&) {}
void DeviceKeysSession::openFile(const QString&) {}
void DeviceKeysSession::saveFile(const QString&, const QUrl&) {}
void DeviceKeysSession::receiveNow() {}
void DeviceKeysSession::clearChatError() {}

#else

e2ev3::Backend DeviceKeysSession::backend() {
    std::lock_guard lock(storeMutex());
    // Made on first use, off the UI thread: finding a keyring can take a moment (and may ask for it to be unlocked).
    if (!store_) {
        const std::filesystem::path root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdString();
        const auto folder = root / "device-keys";
        store_ = std::make_shared<e2ev3::DeviceStore>(folder / "state", std::shared_ptr<e2ev3::KeyVault>(e2ev3::makeDefaultVault(folder / "keys")));
    }
    return e2ev3::Backend{store_, server_};
}

void DeviceKeysSession::apply(const Snapshot& s) {
    enabled_ = s.enabled;
    status_ = s.status;
    if (s.device) device_ = s.device;
    devices_ = s.devices;
    waiting_ = s.waiting;
    pendingReset_ = s.pendingReset;
    events_ = s.events;
    protection_ = s.protection;
    interrupted_ = s.interrupted;
    if (status_ == QStringLiteral("trusted")) {
        if (!receiveTimer_.isActive()) receiveTimer_.start();
    } else {
        receiveTimer_.stop();
        if (chatActive_) {
            chatActive_ = false;
            emit chatChanged();
        }
    }
    emit changed();
}

void DeviceKeysSession::run(const char*, std::function<void()> work, std::function<void()> done) {
    if (busy_) return;
    setBusy(true);
    setError({});
    runInBackground(
        pool_, this, [work = std::move(work)] { translated(work); },
        [this, done = std::move(done)]() {
            setBusy(false);
            if (done) done();
            refresh();
        },
        [this](const Failure& failure) {
            setBusy(false);
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setError(failure.message);
            refresh();
        });
}

void DeviceKeysSession::refresh() {
    if (!available() || refreshing_) return;
    refreshing_ = true;
    const std::string userId = identity_->userId;
    auto current = device_;
    auto self = this;
    runInBackground(
        pool_, this,
        [self, userId, current]() -> Snapshot {
            return translated([&]() -> Snapshot {
                Snapshot s;
                const e2ev3::Backend b = self->backend();
                s.protection = QVariantMap{{"kind", pairKind(b.store->protection())},
                                           {"text", qs(e2ev3::describeProtection(b.store->protection()))},
                                           {"weak", e2ev3::isWeakProtection(b.store->protection())}};
                const Json config = b.server->config();
                s.enabled = config.value("enabled", false);
                if (!s.enabled) return s;

                std::shared_ptr<e2ev3::V3Device> device = current;
                if (!device) device = e2ev3::V3Device::open(userId, b);
                s.device = device;
                const Json view = b.server->getDeviceList(userId);
                Json list = view.value("deviceList", Json(nullptr));
                bool linked = false;
                bool trusted = false;
                bool listedHere = false;
                if (device) {
                    linked = device->op("deviceList").value("ok", false);
                    if (linked && !list.is_null()) {
                        const Json checked = device->op("verifyDeviceList", Json{{"deviceList", list}});
                        if (!checked.value("ok", false)) throw e2ev3::EngineError("bad_device_list", "This account's device list doesn't verify.");
                        listedHere = checked.value("thisDeviceListed", true);
                        const Json own = device->op("deviceList");
                        trusted = own.value("ok", false) && own.value("trusted", false);
                        if (own.contains("deviceList") && own.at("deviceList").is_object()) list = own.at("deviceList");
                    }
                }
                const Json pending = view.value("pendingReset", Json(nullptr));
                if (pending.is_object()) s.pendingReset = QVariantMap{{"effectiveAt", str(pending, "effectiveAt")}};

                const Json registered = b.server->listDevices();
                Json enrollments = Json::array();
                if (device && trusted) enrollments = b.server->listEnrollments();
                const Json events = b.server->events();

                const std::string thisId = device ? device->deviceId() : std::string();
                if (list.is_object()) {
                    for (const Json& d : list.value("devices", Json::array())) {
                        const std::string id = d.value("deviceId", std::string());
                        s.devices.append(QVariantMap{{"deviceId", qs(id)},
                                                     {"fingerprint", qs(d.value("ed25519", std::string()).substr(0, 16)) + QStringLiteral("…")},
                                                     {"isThis", id == thisId},
                                                     {"canRemove", trusted && id != thisId},
                                                     {"signal", [&] {
                                                          for (const Json& c : list.value("capabilities", Json::array())) {
                                                              if (c.value("deviceId", std::string()) == id) return true;
                                                          }
                                                          return false;
                                                      }()}});
                    }
                }
                for (const Json& d : registered) {
                    const std::string id = d.value("deviceId", std::string());
                    if (d.value("listed", false) || id == thisId) continue;
                    QString enrollmentId;
                    for (const Json& e : enrollments) {
                        if (e.value("newDeviceId", std::string()) == id) enrollmentId = qs(e.value("id", std::string()));
                    }
                    s.waiting.append(QVariantMap{{"deviceId", qs(id)}, {"enrollmentId", enrollmentId}, {"asking", !enrollmentId.isEmpty()}});
                }
                int shown = 0;
                for (auto it = events.rbegin(); it != events.rend() && shown < 8; ++it, ++shown) {
                    s.events.append(QVariantMap{{"text", eventText(str(*it, "kind"))}, {"when", str(*it, "createdOn")}});
                }

                if (!device && list.is_null()) {
                    s.status = QStringLiteral("setup");
                } else if (!device || !linked) {
                    s.status = QStringLiteral("pair");
                    s.interrupted = device != nullptr;
                } else if (!listedHere) {
                    s.status = QStringLiteral("removed");
                } else {
                    s.status = trusted ? QStringLiteral("trusted") : QStringLiteral("pair");
                }
#ifdef REDVAULT_HAS_E2E_SIGNAL
                // rv-signal-v1 (development builds only): turn it on, declare it in the signed list, top keys up.
                // A failure here never blocks the rest of the device-key screen; the next refresh tries again.
                if (device && linked && listedHere) {
                    try {
                        e2ev3::maintainSignal(*device, config);
                    } catch (const std::exception&) {
                    }
                }
#endif
                return s;
            });
        },
        [this](Snapshot s) {
            refreshing_ = false;
            apply(s);
        },
        [this](const Failure& failure) {
            refreshing_ = false;
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            // The server answering 404/503 for protocol 3 means it is off: nothing to show, nothing to report.
            if (failure.status == 404 || failure.status == 503) {
                status_ = QStringLiteral("off");
                emit changed();
                return;
            }
            setError(failure.message);
        });
}

void DeviceKeysSession::setUp() {
    if (status_ != QStringLiteral("setup")) return;
    const std::string userId = identity_->userId;
    const auto key = identity_->privateKey.view();
    auto legacy = std::make_shared<crypto::Bytes>(key.begin(), key.end());
    auto self = this;
    auto made = std::make_shared<std::shared_ptr<e2ev3::V3Device>>();
    run(
        "setUp",
        [self, userId, legacy, made] {
            *made = e2ev3::setUpFirstDevice(userId, *legacy, self->backend());
            std::fill(legacy->begin(), legacy->end(), 0);
        },
        [this, made] { device_ = *made; });
}

void DeviceKeysSession::startPairing() {
    if (status_ != QStringLiteral("pair") || joining_ || device_) return;
    const std::string userId = identity_->userId;
    auto self = this;
    stopWaiting_->store(false);
    setError({});
    auto made = std::make_shared<std::shared_ptr<e2ev3::V3Device>>();
    auto pairing = std::make_shared<std::shared_ptr<e2ev3::NewDevicePairing>>();
    setBusy(true);
    runInBackground(
        waitPool_, this,
        [self, userId, made, pairing] {
            translated([&] {
                *made = e2ev3::startNewDevice(userId, self->backend());
                *pairing = e2ev3::NewDevicePairing::begin(**made);
            });
        },
        [this, made, pairing] {
            setBusy(false);
            device_ = *made;
            joiningPairing_ = *pairing;
            joining_ = true;
            emit changed();
            auto device = device_;
            auto pair = joiningPairing_;
            auto stop = stopWaiting_;
            runInBackground(
                waitPool_, this,
                [device, pair, stop] {
                    return translated([&] {
                        std::vector<int> codes = pair->codes(waitFor(stop));
                        return codes;
                    });
                },
                [this](std::vector<int> codes) {
                    joinCodes_.clear();
                    for (int c : codes) joinCodes_.append(c);
                    emit changed();
                },
                [this](const Failure& failure) { abandonJoin(failure.message); });
        },
        [this](const Failure& failure) {
            setBusy(false);
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setError(failure.message);
            refresh();
        });
}

void DeviceKeysSession::abandonJoin(const QString& message) {
    auto device = device_;
    auto pair = joiningPairing_;
    joiningPairing_.reset();
    joining_ = false;
    joinCodes_.clear();
    if (!message.isEmpty()) setError(message);
    emit changed();
    // Whatever was half made goes: the keys it published, its enrollment request.
    runInBackground(
        pool_, this,
        [device, pair] {
            if (pair) pair->cancel();
            if (device) device->forget();
        },
        [this] {
            device_.reset();
            refresh();
        },
        [this](const Failure&) {
            device_.reset();
            refresh();
        });
}

void DeviceKeysSession::confirmJoin() {
    if (!joining_ || joinCodes_.isEmpty() || busy_) return;
    auto device = device_;
    auto pair = joiningPairing_;
    auto stop = stopWaiting_;
    setBusy(true);
    runInBackground(
        waitPool_, this, [device, pair, stop] { translated([&] { pair->confirm(waitFor(stop)); }); },
        [this] {
            setBusy(false);
            joining_ = false;
            joinCodes_.clear();
            joiningPairing_.reset();
            refresh();
        },
        [this](const Failure& failure) {
            setBusy(false);
            abandonJoin(failure.message);
        });
}

void DeviceKeysSession::cancelJoin() {
    if (!joining_) return;
    stopWaiting_->store(true);
    abandonJoin({});
}

void DeviceKeysSession::compareCodes(const QString& newDeviceId) {
    if (!device_ || busy_ || !answeringDeviceId_.isEmpty()) return;
    QString enrollmentId;
    for (const QVariant& w : waiting_) {
        const QVariantMap m = w.toMap();
        if (m.value("deviceId").toString() == newDeviceId) enrollmentId = m.value("enrollmentId").toString();
    }
    if (enrollmentId.isEmpty()) return;
    auto device = device_;
    stopWaiting_->store(false);
    auto stop = stopWaiting_;
    auto started = std::make_shared<e2ev3::TrustedPairing::Started>();
    setBusy(true);
    answeringDeviceId_ = newDeviceId;
    emit changed();
    runInBackground(
        waitPool_, this,
        [device, stop, enrollmentId, started] {
            translated([&] { *started = e2ev3::TrustedPairing::begin(*device, enrollmentId.toStdString(), waitFor(stop)); });
        },
        [this, started] {
            setBusy(false);
            answeringPairing_ = std::move(started->pairing);
            answerCodes_.clear();
            for (int c : started->codes) answerCodes_.append(c);
            emit changed();
        },
        [this](const Failure& failure) {
            setBusy(false);
            answeringDeviceId_.clear();
            setError(failure.message);
            refresh();
        });
}

void DeviceKeysSession::approve() {
    if (!answeringPairing_ || busy_) return;
    auto device = device_;
    auto pair = answeringPairing_;
    auto stop = stopWaiting_;
    setBusy(true);
    runInBackground(
        waitPool_, this, [device, pair, stop] { translated([&] { pair->confirm(waitFor(stop)); }); },
        [this] {
            setBusy(false);
            finishAnswer();
        },
        [this](const Failure& failure) {
            setBusy(false);
            finishAnswer();
            setError(failure.message);
        });
}

void DeviceKeysSession::finishAnswer() {
    answeringPairing_.reset();
    answeringDeviceId_.clear();
    answerCodes_.clear();
    emit changed();
    refresh();
}

void DeviceKeysSession::rejectAnswer() {
    if (answeringDeviceId_.isEmpty()) return;
    stopWaiting_->store(true);
    auto pair = answeringPairing_;
    if (pair) {
        runInBackground(
            pool_, this, [pair] { pair->reject(); }, [this] { finishAnswer(); }, [this](const Failure&) { finishAnswer(); });
    } else {
        finishAnswer();
    }
}

void DeviceKeysSession::turnOffSignal(const QString& deviceId) {
    // A signed change to the device list: only a device holding the account key can make it.
    auto device = device_;  // taken here, on the UI thread: device_ is only ever touched there
    if (!device) return;
    run("turnOffSignal", [device, id = deviceId.toStdString()] { e2ev3::turnOffSignal(*device, id); }, [] {});
}

void DeviceKeysSession::revoke(const QString& deviceId) {
    auto device = device_;
    if (!device) return;
    run("revoke", [device, id = deviceId.toStdString()] { e2ev3::revokeDevice(*device, id); });
}

void DeviceKeysSession::removeWaiting(const QString& deviceId) {
    auto server = server_;
    run("removeWaiting", [server, id = deviceId.toStdString()] { server->removeDevice(id); });
}

void DeviceKeysSession::createRecoveryKey() {
    auto device = device_;
    if (!device) return;
    auto key = std::make_shared<std::string>();
    run(
        "recovery", [device, key] { *key = e2ev3::createRecoveryBackup(*device); },
        [this, key] {
            recoveryKey_ = QString::fromStdString(*key);
            emit changed();
        });
}

void DeviceKeysSession::dismissRecoveryKey() {
    recoveryKey_.clear();
    emit changed();
}

void DeviceKeysSession::restore(const QString& recoveryKey) {
    if (device_) return;
    const std::string userId = identity_->userId;
    const std::string text = recoveryKey.toStdString();
    auto self = this;
    auto made = std::make_shared<std::shared_ptr<e2ev3::V3Device>>();
    run(
        "restore", [self, userId, text, made] { *made = e2ev3::restoreWithRecoveryKey(userId, text, self->backend()); },
        [this, made] { device_ = *made; });
}

void DeviceKeysSession::cancelReset() {
    auto device = device_;
    if (!device || pendingReset_.isEmpty()) return;
    run("cancelReset", [device] {
        const Json view = device->server().getDeviceList(device->userId());
        const Json pending = view.value("pendingReset", Json(nullptr));
        if (!pending.is_object()) return;
        const Json refusal = device->must("cancelReset", Json{{"pendingAccountKey", pending.at("accountKey")}});
        device->server().cancelReset(refusal.at("signature").get<std::string>());
    });
}

void DeviceKeysSession::forgetDevice() {
    auto device = device_;
    if (!device) return;
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const std::filesystem::path file = std::filesystem::path(root.toStdString()) / "device-keys" / ("chat-" + device->deviceId());
    run("forget", [device] { device->forget(); }, [this, file] {
        // The device-key history goes with the keys.
        std::error_code ec;
        std::filesystem::remove(file, ec);
        history_.reset();
        closeChat();
        device_.reset();
    });
}

void DeviceKeysSession::startOver() {
    auto device = device_;
    if (!device || joining_) return;
    run(
        "startOver",
        [device] {
            device->op("cancelLink");
            device->forget();
        },
        [this] { device_.reset(); });
}

void DeviceKeysSession::loadSafetyNumber(const QString& contactUserId, const QString& conversationId) {
    updateSafetyNumber(contactUserId, conversationId, {});
}

void DeviceKeysSession::setDeviceKeysVerified(const QString& contactUserId, const QString& conversationId, const QString& accountKey,
                                              bool verified) {
    updateSafetyNumber(contactUserId, conversationId,
                       [contact = contactUserId.toStdString(), key = accountKey.toStdString(), verified](e2ev3::V3Device& device) {
                           e2ev3::setContactVerified(device, contact, key, verified);
                       });
}

void DeviceKeysSession::acceptNewDeviceKeys(const QString& contactUserId, const QString& conversationId, const QString& accountKey,
                                            bool verified) {
    updateSafetyNumber(contactUserId, conversationId,
                       [contact = contactUserId.toStdString(), conversation = conversationId.toStdString(), key = accountKey.toStdString(),
                        verified](e2ev3::V3Device& device) { e2ev3::acceptContactKey(device, contact, key, verified, conversation); });
}

// Runs `change` (if any), then reads the number again, off the UI thread; answers with safetyNumberReady.
void DeviceKeysSession::updateSafetyNumber(const QString& contactUserId, const QString& conversationId,
                                           std::function<void(e2ev3::V3Device&)> change) {
    auto device = device_;
    if (!device || status_ != QStringLiteral("trusted")) {
        emit safetyNumberReady(contactUserId, {});
        return;
    }
    auto out = std::make_shared<QVariantMap>();
    runInBackground(
        pool_, this,
        [device, out, change = std::move(change), contact = contactUserId.toStdString(), conversation = conversationId.toStdString()] {
            try {
                if (change) change(*device);
                if (const auto safety = e2ev3::deviceKeySafety(*device, contact, conversation)) {
                    QStringList groups;
                    for (const auto& group : safety->groups) groups.append(qs(group));
                    (*out)[QStringLiteral("groups")] = groups;
                    (*out)[QStringLiteral("qrText")] = qs(safety->qrText);
                    (*out)[QStringLiteral("accountKey")] = qs(safety->accountKey);
                    (*out)[QStringLiteral("verified")] = safety->verified;
                    (*out)[QStringLiteral("keyChanged")] = safety->keyChanged;
                }
            } catch (const e2ev3::EngineError& error) {
                (*out)[QStringLiteral("problem")] = error.code() == "account_key_changed"
                                                        ? QStringLiteral("Their keys changed again while this was open. Close this and open it again.")
                                                        : QStringLiteral("Couldn't check their device keys. Try again.");
            } catch (const std::exception&) {
                (*out)[QStringLiteral("problem")] = QStringLiteral("Couldn't check their device keys. Try again.");
            }
        },
        [this, out, contactUserId] { emit safetyNumberReady(contactUserId, *out); },
        [this, contactUserId](const Failure&) {
            emit safetyNumberReady(contactUserId, QVariantMap{{QStringLiteral("problem"), QStringLiteral("Couldn't reach the server.")}});
        });
}

// --- Chat (docs/multi-device-threat-model.md section 9) ---

std::shared_ptr<e2ev3::ChatHistory> DeviceKeysSession::history() {
    if (!device_) return nullptr;
    if (history_ && historyDevice_ != device_->deviceId()) history_.reset();
    if (!history_) {
        historyDevice_ = device_->deviceId();
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const std::filesystem::path file = std::filesystem::path(root.toStdString()) / "device-keys" / ("chat-" + device_->deviceId());
        history_ = std::make_shared<e2ev3::ChatHistory>(file, device_->deriveKey("redvault-v3-chat-history"));
    }
    return history_;
}

void DeviceKeysSession::reloadChat() {
    QVariantList rows;
    if (const auto h = history(); h && !chatId_.isEmpty()) {
        const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
        for (const e2ev3::ChatRecord& r : h->list(chatId_.toStdString())) rows.append(chatRow(r, identity_->userId, now));
    }
    chatModel_.setRows(rows);
}

bool DeviceKeysSession::chatPinned(const QString& chatId) {
    const auto h = history();
    return h && h->pinned(chatId.toStdString());
}

void DeviceKeysSession::openChat(const QString& chatId, const QString& contactUserId, bool isGroup, bool eligible) {
    chatId_ = chatId;
    chatContactUser_ = contactUserId;
    chatContact_ = QStringLiteral("unknown");
    chatProtocol_.clear();
    chatNotice_.clear();
    unread_.remove(chatId);
    chatIsGroup_ = isGroup;
    chatEligible_ = eligible && trusted();
    chatError_.clear();
    const auto h = history();
    chatActive_ = chatEligible_ && h && h->pinned(chatId.toStdString());
    reloadChat();
    receiveTimer_.setInterval(chatActive_ ? 5000 : 30000);
    emit chatChanged();
    if (chatActive_) receiveNow();
    if (chatEligible_) checkContact();
}

void DeviceKeysSession::checkContact() {
    // Does the other side have device keys? Their signed list exists (a group: some other member's does).
    auto device = device_;
    if (!device) return;
    const std::string chat = chatId_.toStdString();
    const std::string contact = chatContactUser_.toStdString();
    const bool group = chatIsGroup_;
    auto answer = std::make_shared<bool>(false);
    auto protocol = std::make_shared<std::string>();
    const QString forChat = chatId_;
    runInBackground(
        chatPool_, this,
        [device, chat, contact, group, answer, protocol] {
            try {
                if (!group) *protocol = e2ev3::chatProtocol(*device, chat, contact);
            } catch (const std::exception&) {
            }
            try {
                if (group) {
                    for (const Json& m : device->server().conversationDeviceLists(chat).value("members", Json::array())) {
                        if (m.value("userId", std::string()) != device->userId() && m.contains("deviceList") && m.at("deviceList").is_object()) *answer = true;
                    }
                } else {
                    const Json view = device->server().getDeviceList(contact, chat);
                    *answer = view.contains("deviceList") && view.at("deviceList").is_object();
                }
            } catch (const std::exception&) {
            }
        },
        [this, answer, protocol, forChat] {
            if (forChat != chatId_) return;
            chatContact_ = *answer ? QStringLiteral("yes") : QStringLiteral("no");
            chatProtocol_ = qs(*protocol);
            emit chatChanged();
        },
        [](const Failure&) {});
}

void DeviceKeysSession::closeChat() {
    chatId_.clear();
    chatActive_ = false;
    chatContact_ = QStringLiteral("unknown");
    chatModel_.setRows({});
    receiveTimer_.setInterval(30000);
    emit chatChanged();
}

void DeviceKeysSession::useDeviceKeys() {
    if (!chatEligible_ || chatId_.isEmpty()) return;
    chatActive_ = true;
    chatError_.clear();
    receiveTimer_.setInterval(5000);
    emit chatChanged();
    receiveNow();
}

void DeviceKeysSession::useOlderProtocol() {
    if (const auto h = history(); h && !chatId_.isEmpty()) h->unpin(chatId_.toStdString());
    chatActive_ = false;
    chatError_.clear();
    receiveTimer_.setInterval(30000);
    emit chatChanged();
}

void DeviceKeysSession::clearChatError() {
    if (chatError_.isEmpty()) return;
    chatError_.clear();
    emit chatChanged();
}

void DeviceKeysSession::chatFailed(const Failure& failure, const QString& fallback) {
    if (failure.status == 401) {
        emit sessionExpired();
        return;
    }
    chatError_ = failure.message.isEmpty() ? fallback : failure.message;
}

void DeviceKeysSession::setChatFileActivity(const QString& activity) {
    if (chatFileActivity_ == activity) return;
    chatFileActivity_ = activity;
    emit chatChanged();
}

void DeviceKeysSession::runChat(ChatWork work, const QString& fallback, bool fromComposer) {
    auto device = device_;
    auto h = history();
    if (!device || !h) return;
    if (fromComposer) chatSending_ = true;
    chatError_.clear();
    chatNotice_.clear();
    lastUnreachable_->store(0);
    emit chatChanged();
    runInBackground(
        chatPool_, this,
        [device, h, chat = chatId_.toStdString(), contact = chatTarget(), work = std::move(work)] {
            translated([&] {
                // Anything that arrived first is read before sending (9.6).
                e2ev3::receiveChat(*device, *h);
                work(*device, *h, chat, contact);
            });
        },
        [this, fromComposer] {
            if (fromComposer) chatSending_ = false;
            const int unreachable = lastUnreachable_->load();
            if (unreachable > 0) {
                chatNotice_ = unreachable == 1 ? QStringLiteral("Sent, but one of their devices couldn't be reached. It won't get this message.")
                                               : QStringLiteral("Sent, but %1 of their devices couldn't be reached. They won't get this message.").arg(unreachable);
            }
            reloadChat();
            checkContact();  // the protocol may have changed with the lists this send verified
            emit chatChanged();
            if (fromComposer) emit chatSent(true);
        },
        [this, fromComposer, fallback](const Failure& failure) {
            if (fromComposer) chatSending_ = false;
            chatFailed(failure, fallback);
            reloadChat();  // a failed message is listed, with Retry
            emit chatChanged();
            if (fromComposer) emit chatSent(false);
        });
}

void DeviceKeysSession::sendChat(const QString& text) {
    if (!chatActive_ || chatSending_ || text.trimmed().isEmpty()) return;
    const std::string body = text.trimmed().toStdString();
    runChat(
        [body, unreachable = lastUnreachable_](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            if (contact.empty()) {
                e2ev3::sendGroupChat(device, h, chat, body);
            } else {
                unreachable->store(e2ev3::sendChat(device, h, chat, contact, body).unreachable);
            }
        },
        QStringLiteral("The message wasn't sent."), true);
}

void DeviceKeysSession::retryChat(const QString& messageId) {
    const auto h = history();
    if (!h || chatSending_) return;
    const auto failed = shownMessage(messageId);
    if (!failed || failed->status != "failed") return;
    const std::string chat = chatId_.toStdString();
    h->remove(chat, failed->id);  // sent again under a fresh id and ciphertext, never reused
    reloadChat();
    if (failed->file) {
        runChat(
            [file = *failed->file](e2ev3::V3Device& device, e2ev3::ChatHistory& history, const std::string& conversation, const std::string& contact) {
                e2ev3::sendChatFile(device, history, conversation, contact, file);
            },
            QStringLiteral("The file wasn't sent."), true);
    } else {
        sendChat(qs(failed->text));
    }
}

std::optional<e2ev3::ChatRecord> DeviceKeysSession::shownMessage(const QString& messageId) {
    const auto h = history();
    if (!h || chatId_.isEmpty()) return std::nullopt;
    const std::string id = messageId.toStdString();
    for (e2ev3::ChatRecord& r : h->list(chatId_.toStdString())) {
        if (r.id == id) return std::move(r);
    }
    return std::nullopt;
}

std::optional<model::FileDescriptor> DeviceKeysSession::shownFile(const QString& messageId) {
    const auto message = shownMessage(messageId);
    return message ? message->file : std::nullopt;
}

void DeviceKeysSession::reactChat(const QString& messageId, const QString& emoji) {
    const auto message = shownMessage(messageId);
    if (!chatActive_ || !message || message->deleted || message->status == "failed") return;
    const std::string chosen = emoji.toStdString();
    bool already = false;
    for (const e2ev3::ChatReaction& r : message->reactions) {
        already = already || (r.userId == identity_->userId && r.emoji == chosen);
    }
    const e2ev3::ChatOp op{"reaction", message->id, {}, already ? std::string() : chosen};
    runChat(
        [op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::sendChatOp(device, h, chat, contact, op);
        },
        QStringLiteral("That didn't go through. Check your connection and try again."), false);
}

void DeviceKeysSession::editChat(const QString& messageId, const QString& text) {
    const auto message = shownMessage(messageId);
    const std::string body = text.trimmed().toStdString();
    if (!chatActive_ || chatSending_ || !message || body.empty()) return;
    if (message->direction != "out" || message->deleted || message->file || message->status == "failed") return;
    if (message->text == body) {  // nothing changed: nothing to send, and it doesn't read as edited
        emit chatSent(true);
        return;
    }
    const e2ev3::ChatOp op{"edit", message->id, body, {}};
    runChat(
        [op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::sendChatOp(device, h, chat, contact, op);
        },
        QStringLiteral("That didn't go through. Check your connection and try again."), true);
}

void DeviceKeysSession::deleteChat(const QString& messageId) {
    const auto message = shownMessage(messageId);
    if (!chatActive_ || !message || message->direction != "out" || message->deleted || message->status == "failed") return;
    const e2ev3::ChatOp op{"delete", message->id, {}, {}};
    const std::string objectId = message->file ? message->file->objectId : std::string();
    auto api = api_;
    runChat(
        [op, objectId, api](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::sendChatOp(device, h, chat, contact, op);
            if (objectId.empty()) return;
            try {
                api->deleteFile(objectId);  // best effort: without its key the file is unreadable anyway
            } catch (const std::exception&) {
            }
        },
        QStringLiteral("That didn't go through. Check your connection and try again."), false);
}

void DeviceKeysSession::sendFile(const QUrl& file) {
    auto device = device_;
    auto h = history();
    if (!device || !h || !chatActive_ || !chatFileActivity_.isEmpty()) return;
    const std::string chat = chatId_.toStdString();
    const std::string contact = chatTarget();
    const std::filesystem::path path(file.toLocalFile().toStdWString());
    const QString fileUrl = file.toString();
    const QString fallback = QStringLiteral("The file wasn't sent.");
    chatError_.clear();
    setChatFileActivity(QStringLiteral("Encrypting…"));
    // Stage updates arrive on the worker thread; hop to the UI thread to show them.
    QPointer<DeviceKeysSession> self(this);
    auto onStage = [self](data::FileSendStage stage) {
        QMetaObject::invokeMethod(
            qApp,
            [self, stage] {
                if (!self || self->chatFileActivity_.isEmpty()) return;
                self->setChatFileActivity(stage == data::FileSendStage::Encrypting  ? QStringLiteral("Encrypting…")
                                          : stage == data::FileSendStage::Uploading ? QStringLiteral("Uploading…")
                                                                                    : QStringLiteral("Sending…"));
            },
            Qt::QueuedConnection);
    };
    auto finished = [this, fileUrl, fallback](const Failure* failure) {
        setChatFileActivity({});
        if (failure) chatFailed(*failure, fallback);
        reloadChat();
        emit chatChanged();
        emit chatFileDone(fileUrl);
    };
    // The transfer isn't on the chat thread: a large file mustn't stop messages arriving.
    runInBackground(
        waitPool_, this, [files = files_, chat, path, onStage] { return files->uploadForDeviceKeys(chat, path, onStage); },
        [this, device, h, chat, contact, finished](model::FileDescriptor descriptor) {
            runInBackground(
                chatPool_, this,
                [device, h, chat, contact, descriptor = std::move(descriptor)] {
                    translated([&] {
                        e2ev3::receiveChat(*device, *h);
                        e2ev3::sendChatFile(*device, *h, chat, contact, descriptor);
                    });
                },
                [finished] { finished(nullptr); }, [finished](const Failure& failure) { finished(&failure); });
        },
        [finished](const Failure& failure) { finished(&failure); });
}

void DeviceKeysSession::openFile(const QString& messageId) {
    const auto file = shownFile(messageId);
    if (!file || !chatFileActivity_.isEmpty()) return;
    if (!file_safety::isSafeToOpen(qs(file->filename))) {
        chatError_ = QStringLiteral("For your safety, only pictures, audio, video, text and PDF files open from a chat. Save this one instead.");
        emit chatChanged();
        return;
    }
    setChatFileActivity(QStringLiteral("Decrypting ") + qs(file->filename) + QStringLiteral("…"));
    runInBackground(
        waitPool_, this,
        [files = files_, descriptor = *file] {
            const auto path = files->download(descriptor);
            file_safety::markFromInternet(path);
            return path;
        },
        [this](std::filesystem::path path) {
            setChatFileActivity({});
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(path.wstring())))) {
                chatError_ = QStringLiteral("There's no app on this computer to open that kind of file. Save it instead.");
                emit chatChanged();
            }
        },
        [this](const Failure& failure) {
            setChatFileActivity({});
            chatFailed(failure, QStringLiteral("Download failed. Try again."));
            emit chatChanged();
        });
}

void DeviceKeysSession::saveFile(const QString& messageId, const QUrl& destination) {
    const auto file = shownFile(messageId);
    if (!file || !chatFileActivity_.isEmpty()) return;
    const std::filesystem::path target(destination.toLocalFile().toStdWString());
    setChatFileActivity(QStringLiteral("Decrypting ") + qs(file->filename) + QStringLiteral("…"));
    runInBackground(
        waitPool_, this,
        [files = files_, descriptor = *file, target] {
            const auto decrypted = files->download(descriptor);
            // The save dialog has already asked before replacing an existing file.
            std::filesystem::copy_file(decrypted, target, std::filesystem::copy_options::overwrite_existing);
            file_safety::markFromInternet(target);
            // The decrypted copy in the cache isn't needed once it's saved.
            std::error_code ignored;
            std::filesystem::remove_all(decrypted.parent_path(), ignored);
        },
        [this, target] {
            setChatFileActivity({});
            emit chatFileSaved(QDir::toNativeSeparators(QString::fromStdWString(target.wstring())));
        },
        [this](const Failure& failure) {
            setChatFileActivity({});
            chatFailed(failure, QStringLiteral("Download failed. Try again."));
            emit chatChanged();
        });
}

void DeviceKeysSession::receiveNow() {
    auto device = device_;
    auto h = history();
    if (!device || !h || !trusted() || chatReceiving_) return;
    chatReceiving_ = true;
    auto fresh = std::make_shared<std::vector<e2ev3::ChatRecord>>();
    auto stats = std::make_shared<e2ev3::ReceiveStats>();
    runInBackground(
        chatPool_, this, [device, h, fresh, stats] { translated([&] { *fresh = e2ev3::receiveChat(*device, *h, 0, stats.get()); }); },
        [this, fresh, stats] {
            chatReceiving_ = false;
            if (stats->heldForKeys != chatHeld_) {
                chatHeld_ = stats->heldForKeys;
                emit chatChanged();
            }
            int elsewhere = 0;
            bool reload = false;
            for (const auto& r : *fresh) {
                if (qs(r.conversationId) == chatId_) {
                    reload = true;
                } else if (r.direction == "in" && !r.op) {  // a reaction, edit or delete isn't a new message
                    ++elsewhere;
                    const QString chat = qs(r.conversationId);
                    unread_[chat] = unread_.value(chat).toInt() + 1;
                }
            }
            if (reload) reloadChat();
            if (!fresh->empty()) emit chatChanged();
            if (elsewhere > 0) emit incoming(elsewhere);
        },
        [this](const Failure& failure) {
            chatReceiving_ = false;
            if (failure.status == 401) emit sessionExpired();
        });
}

#endif

}  // namespace redvault::app
