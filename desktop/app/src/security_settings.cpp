// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "security_settings.hpp"

#include <QClipboard>
#include <QDate>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>

#include "async.hpp"
#include "chat_session.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/call_crypto.hpp"
#include "redvault/crypto/commitment.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/data/conversation_repository.hpp"
#include "redvault/rtc/call_media.hpp"

namespace redvault::app {

namespace live = api::live;
namespace calls = crypto::calls;

// --- Linked devices ---

namespace {

/// Until you rename it. Only the platform, nothing that identifies the machine.
QString defaultDeviceName() {
#if defined(_WIN32)
    return QStringLiteral("Desktop app · Windows");
#else
    return QStringLiteral("Desktop app · Linux");
#endif
}

}  // namespace

LinkedDevices::LinkedDevices(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, QObject* parent)
    : QObject(parent), api_(std::move(api)), identity_(std::move(identity)) {
    pool_.setMaxThreadCount(1);
}

void LinkedDevices::reload() {
    auto api = api_;
    auto identity = identity_;
    runInBackground(
        pool_, this,
        [api, identity] {
            const auto key = calls::deriveLocalKey(identity->privateKey.view(), "redvault-device-label-v1");
            QVariantList out;
            for (const auto& s : live::listSessions(*api)) {
                QString name;
                if (s.label) {
                    try {
                        name = QString::fromStdString(crypto::decryptWithConversationKey(crypto::base64Decode(s.label->ciphertext),
                                                                                         crypto::base64Decode(s.label->nonce), key.view()));
                    } catch (const std::exception&) {
                        name.clear();
                    }
                }
                const QDate expires = QDate::fromString(QString::fromStdString(s.expiresOn).left(10), Qt::ISODate);
                out.append(QVariantMap{
                    {"id", QString::fromStdString(s.id)},
                    {"current", s.current},
                    {"awaitingTwoFactor", s.awaitingTwoFactor},
                    {"expiresOn", expires.isValid() ? QLocale().toString(expires, QStringLiteral("d MMMM yyyy")) : QString::fromStdString(s.expiresOn)},
                    {"name", name},
                });
            }
            // This device first.
            std::stable_sort(out.begin(), out.end(), [](const QVariant& a, const QVariant& b) {
                return a.toMap().value("current").toBool() && !b.toMap().value("current").toBool();
            });
            return out;
        },
        [this](QVariantList devices) {
            devices_ = std::move(devices);
            loaded_ = true;
            error_.clear();
            emit devicesChanged();
            // Name this device the first time it's seen here (once: a failed save isn't retried in a loop).
            for (const auto& d : devices_) {
                const auto row = d.toMap();
                if (row.value("current").toBool() && row.value("name").toString().isEmpty() && !namedOnce_) {
                    namedOnce_ = true;
                    saveLabel(defaultDeviceName(), false);
                }
            }
        },
        [this](const Failure&) {
            error_ = QStringLiteral("Couldn't load your devices. Try again.");
            emit devicesChanged();
        });
}

void LinkedDevices::saveLabel(const QString& name, bool reportFailure) {
    auto api = api_;
    auto identity = identity_;
    const std::string text = name.trimmed().left(60).toStdString();
    runInBackground(
        pool_, this,
        [api, identity, text] {
            const auto key = calls::deriveLocalKey(identity->privateKey.view(), "redvault-device-label-v1");
            const auto sealed = crypto::encryptWithConversationKey(text, key.view());
            live::setSessionLabel(*api, {crypto::base64Encode(sealed.ciphertext), crypto::base64Encode(sealed.nonce)});
        },
        [this] { reload(); },
        [this, reportFailure](const Failure&) {
            if (!reportFailure) return;
            error_ = QStringLiteral("Couldn't rename this device.");
            emit devicesChanged();
        });
}

void LinkedDevices::rename(const QString& name) {
    if (name.trimmed().isEmpty()) return;
    saveLabel(name, true);
}

void LinkedDevices::signOut(const QString& sessionId) {
    auto api = api_;
    const std::string id = sessionId.toStdString();
    runInBackground(
        pool_, this, [api, id] { live::signOutSession(*api, id); }, [this] { reload(); },
        [this](const Failure&) {
            error_ = QStringLiteral("Couldn't sign that device out. Try again.");
            emit devicesChanged();
        });
}

// --- Transparency log ---

namespace {

constexpr int kLivePollMs = 15000;
constexpr int kMaxOwnChecks = 40;
constexpr int kOwnChats = 8;

/// What this computer verified before, per server (each server has its own log and key).
QString logSetting(const api::ApiClient& api, const char* name) {
    return QStringLiteral("transparencyLog/") + QUrl(QString::fromStdString(api.baseUrl())).host() + u'/' + QLatin1String(name);
}

QString keyFingerprint(const std::string& publicKeyB64) {
    try {
        const std::string hex = calls::merkle::toHex(crypto::base64Decode(publicKeyB64)).substr(0, 16);
        QString out;
        for (std::size_t i = 0; i < hex.size(); i += 4) {
            if (i) out += u' ';
            out += QString::fromStdString(hex.substr(i, 4));
        }
        return out;
    } catch (const std::exception&) {
        return {};
    }
}

QString shortHex(const std::string& hex) {
    return hex.size() > 30 ? QString::fromStdString(hex.substr(0, 16) + "…" + hex.substr(hex.size() - 10)) : QString::fromStdString(hex);
}

bool inclusionOk(api::ApiClient& api, const std::string& commitmentHex, std::int64_t index, const live::LogCheckpoint& cp,
                 std::vector<std::string>* pathOut = nullptr) {
    if (index >= cp.treeSize) return false;
    const auto proof = live::inclusionProof(api, index, cp.treeSize);
    if (pathOut) *pathOut = proof.path;
    const auto leaf = calls::merkle::leafHash(calls::merkle::fromHex(commitmentHex));
    if (calls::merkle::toHex(leaf) != proof.leafHash) return false;
    std::vector<crypto::Bytes> path;
    for (const auto& h : proof.path) path.push_back(calls::merkle::fromHex(h));
    return calls::merkle::verifyInclusion(leaf, static_cast<std::uint64_t>(index), static_cast<std::uint64_t>(cp.treeSize), path,
                                          calls::merkle::fromHex(cp.rootHash));
}

struct Checked {
    std::optional<live::LogCheckpoint> checkpoint;
    QString keyStatus;  // "pinned", "first-use", "changed"
    int consistent = -1;  // -1: nothing to compare with; 0: failed; 1: checked
    std::vector<live::LogEntry> entries;
};

class LogCheckError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace

TransparencyLog::TransparencyLog(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, ChatSession* chats,
                                 QObject* parent)
    : QObject(parent), api_(std::move(api)), identity_(std::move(identity)), chats_(chats) {
    pool_.setMaxThreadCount(2);
    liveTimer_.setInterval(kLivePollMs);
    connect(&liveTimer_, &QTimer::timeout, this, &TransparencyLog::refresh);
}

TransparencyLog::~TransparencyLog() {
    pool_.clear();
    pool_.waitForDone(5000);
}

void TransparencyLog::activate(bool active) {
    active_ = active;
    if (active) {
        refresh();
        if (live_) liveTimer_.start();
    } else {
        liveTimer_.stop();
    }
}

void TransparencyLog::setLive(bool on) {
    if (live_ == on) return;
    live_ = on;
    if (on && active_ && !disabled_) liveTimer_.start();
    else liveTimer_.stop();
    emit changed();
}

void TransparencyLog::setSearch(const QString& search) {
    if (search_ == search) return;
    search_ = search;
    emit changed();
    refresh();
}

void TransparencyLog::setOnlyMine(bool on) {
    if (onlyMine_ == on) return;
    onlyMine_ = on;
    emit changed();
    if (on) loadMineRows();
    verifyRows();
}

void TransparencyLog::refresh() {
    auto api = api_;
    const std::string search = search_.trimmed().toStdString();
    // What this computer verified before: read on the UI thread, written back only after the checks pass.
    QSettings settings;
    const std::string pinned = settings.value(logSetting(*api_, "publicKey")).toString().toStdString();
    const std::int64_t lastSize = settings.value(logSetting(*api_, "lastTreeSize"), -1).toLongLong();
    const std::string lastRoot = settings.value(logSetting(*api_, "lastRootHash")).toString().toStdString();
    const int generation = ++generation_;
    runInBackground(
        pool_, this,
        [api, search, pinned, lastSize, lastRoot] {
            Checked out;
            out.checkpoint = live::latestCheckpoint(*api);
            if (out.checkpoint) {
                const auto& cp = *out.checkpoint;
                if (!calls::checkpointSignatureValid(cp.treeSize, cp.rootHash, cp.signedAt, cp.signature, cp.publicKey)) {
                    throw LogCheckError("The latest checkpoint's signature doesn't check out.");
                }
                out.keyStatus = pinned.empty() ? QStringLiteral("first-use") : pinned == cp.publicKey ? QStringLiteral("pinned") : QStringLiteral("changed");
                if (out.keyStatus != QStringLiteral("changed") && lastSize >= 0 && !lastRoot.empty()) {
                    if (lastSize == cp.treeSize) {
                        out.consistent = lastRoot == cp.rootHash ? 1 : 0;
                    } else if (lastSize > cp.treeSize) {
                        out.consistent = 0;  // the log shrank: rolled back or forked
                    } else {
                        std::vector<crypto::Bytes> path;
                        for (const auto& h : live::consistencyProof(*api, lastSize, cp.treeSize)) path.push_back(calls::merkle::fromHex(h));
                        out.consistent = calls::merkle::verifyConsistency(static_cast<std::uint64_t>(lastSize), static_cast<std::uint64_t>(cp.treeSize),
                                                                          calls::merkle::fromHex(lastRoot), calls::merkle::fromHex(cp.rootHash), path)
                                             ? 1
                                             : 0;
                    }
                }
            }
            out.entries = live::logEntries(*api, std::nullopt, 50, search);
            return out;
        },
        [this, generation](Checked result) {
            if (generation != generation_) return;
            disabled_ = false;
            error_.clear();
            const bool newCheckpoint = !checkpoint_ || !result.checkpoint || checkpoint_->treeSize != result.checkpoint->treeSize ||
                                       checkpoint_->rootHash != result.checkpoint->rootHash;
            checkpoint_ = result.checkpoint;
            checkpointView_.clear();
            if (checkpoint_) {
                const auto& cp = *checkpoint_;
                QSettings settings;
                if (result.keyStatus == QStringLiteral("first-use")) settings.setValue(logSetting(*api_, "publicKey"), QString::fromStdString(cp.publicKey));
                if (result.keyStatus != QStringLiteral("changed") && result.consistent != 0) {
                    settings.setValue(logSetting(*api_, "lastTreeSize"), static_cast<qlonglong>(cp.treeSize));
                    settings.setValue(logSetting(*api_, "lastRootHash"), QString::fromStdString(cp.rootHash));
                }
                checkpointView_ = QVariantMap{
                    {"treeSize", static_cast<qlonglong>(cp.treeSize)},
                    {"rootHash", QString::fromStdString(cp.rootHash)},
                    {"rootShort", shortHex(cp.rootHash)},
                    {"signedAt", QString::fromStdString(cp.signedAt)},
                    {"keyFingerprint", keyFingerprint(cp.publicKey)},
                    {"keyStatus", result.keyStatus},
                    {"consistent", result.consistent},
                };
            }
            entries_ = std::move(result.entries);
            emit changed();
            verifyRows();
            if (checkpoint_ && (newCheckpoint || mineCheckedAt_ != checkpoint_->treeSize)) checkMine();
        },
        [this, generation](const Failure& failure) {
            if (generation != generation_) return;
            if (failure.code == QStringLiteral("log_disabled") || failure.status == 404) {
                disabled_ = true;
                liveTimer_.stop();
            } else if (failure.status == 0 && failure.message != QStringLiteral("Can't reach the server. Check your connection.")) {
                error_ = QStringLiteral("The log didn't pass the checks on this computer. Treat it as untrusted.");
            } else {
                error_ = QStringLiteral("Couldn't reach the log. Try again.");
            }
            emit changed();
        });
}

void TransparencyLog::acceptNewKey() {
    if (!checkpoint_) return;
    QSettings settings;
    settings.setValue(logSetting(*api_, "publicKey"), QString::fromStdString(checkpoint_->publicKey));
    settings.remove(logSetting(*api_, "lastTreeSize"));
    settings.remove(logSetting(*api_, "lastRootHash"));
    refresh();
}

QString TransparencyLog::proofKey(std::int64_t index) const {
    if (!checkpoint_) return {};
    return QStringLiteral("%1@%2:%3").arg(index).arg(checkpoint_->treeSize).arg(QString::fromStdString(checkpoint_->rootHash));
}

void TransparencyLog::verifyRows() {
    if (!checkpoint_ || verifyingRows_) return;
    // Each listed row is checked here, not taken on the server's word; results are kept per checkpoint.
    std::vector<live::LogEntry> todo;
    for (const auto& e : rows()) {
        if (!proofs_.count(proofKey(e.index))) todo.push_back(e);
    }
    if (todo.empty()) return;
    verifyingRows_ = true;
    auto api = api_;
    const auto cp = *checkpoint_;
    runInBackground(
        pool_, this,
        [api, todo, cp] {
            std::vector<std::pair<std::int64_t, bool>> results;
            for (const auto& e : todo) {
                bool ok = false;
                try {
                    ok = inclusionOk(*api, e.commitment, e.index, cp);
                } catch (...) {
                    ok = false;
                }
                results.emplace_back(e.index, ok);
            }
            return results;
        },
        [this, cp](std::vector<std::pair<std::int64_t, bool>> results) {
            verifyingRows_ = false;
            for (const auto& [index, ok] : results) {
                proofs_[QStringLiteral("%1@%2:%3").arg(index).arg(cp.treeSize).arg(QString::fromStdString(cp.rootHash))] = ok;
            }
            emit changed();
            verifyRows();  // rows that arrived meanwhile
        },
        [this](const Failure&) { verifyingRows_ = false; });
}

void TransparencyLog::checkMine() {
    if (!checkpoint_ || !chats_ || checkingMine_) return;
    checkingMine_ = true;
    emit changed();
    auto api = api_;
    auto identity = identity_;
    auto metas = chats_->metas();
    const auto cp = *checkpoint_;
    runInBackground(
        pool_, this,
        [api, identity, metas, cp] {
            // Your own recent messages, from a few chats: commitment computed here, entry number from the server, proof checked here.
            data::ConversationRepository repository(api);
            std::vector<data::Message> own;
            int chats = 0;
            for (const auto& meta : metas) {
                if (chats++ >= kOwnChats) break;
                try {
                    for (auto& m : repository.fetchMessages(*meta, identity->userId).messages) {
                        if (m.fromMe && !m.id.empty() && m.ciphertextB64 && m.nonceB64) own.push_back(std::move(m));
                    }
                } catch (...) {
                }
            }
            std::sort(own.begin(), own.end(), [](const auto& a, const auto& b) { return a.createdAtMs > b.createdAtMs; });
            if (own.size() > static_cast<std::size_t>(kMaxOwnChecks)) own.resize(kMaxOwnChecks);
            std::vector<Mine> results;
            for (const auto& m : own) {
                try {
                    const std::string local = crypto::commitment::computeHex(m.id, crypto::base64Decode(*m.ciphertextB64), crypto::base64Decode(*m.nonceB64));
                    const auto v = api->getMessageVerification(m.id);
                    Mine mine{m.id, local, v.logIndex, {}};
                    if (!crypto::commitment::matches(local, v.commitment)) mine.status = QStringLiteral("mismatch");
                    else if (!v.logIndex || *v.logIndex >= cp.treeSize) mine.status = QStringLiteral("pending");
                    else mine.status = inclusionOk(*api, local, *v.logIndex, cp) ? QStringLiteral("match") : QStringLiteral("mismatch");
                    results.push_back(std::move(mine));
                } catch (...) {
                    // From before the log existed, or a transient error: not counted either way.
                }
            }
            return results;
        },
        [this, cp](std::vector<Mine> results) {
            checkingMine_ = false;
            mine_ = std::move(results);
            mineCheckedAt_ = cp.treeSize;
            emit changed();
            if (onlyMine_) loadMineRows();
        },
        [this](const Failure&) {
            checkingMine_ = false;
            emit changed();
        });
}

void TransparencyLog::loadMineRows() {
    std::vector<std::int64_t> indexes;
    for (const auto& m : mine_) {
        if (m.logIndex && indexes.size() < 25) indexes.push_back(*m.logIndex);
    }
    auto api = api_;
    runInBackground(
        pool_, this,
        [api, indexes] {
            std::vector<live::LogEntry> rows;
            for (auto index : indexes) {
                try {
                    for (auto& e : live::logEntries(*api, std::nullopt, 1, "#" + std::to_string(index))) rows.push_back(std::move(e));
                } catch (...) {
                }
            }
            std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.index > b.index; });
            return rows;
        },
        [this](std::vector<live::LogEntry> rows) {
            mineRows_ = std::move(rows);
            emit changed();
            verifyRows();
        },
        [](const Failure&) {});
}

QVariantList TransparencyLog::entries() const {
    std::map<std::int64_t, const Mine*> mineByIndex;
    for (const auto& m : mine_) {
        if (m.logIndex) mineByIndex[*m.logIndex] = &m;
    }
    QVariantList out;
    for (const auto& e : rows()) {
        const auto own = mineByIndex.find(e.index);
        const auto proof = proofs_.find(proofKey(e.index));
        QString status = QStringLiteral("checking");
        if (own != mineByIndex.end() && own->second->status == QStringLiteral("mismatch")) status = QStringLiteral("mismatch");
        else if (proof != proofs_.end()) status = proof->second ? QStringLiteral("verified") : QStringLiteral("failed");
        const QString logged = QString::fromStdString(e.loggedAt);
        out.append(QVariantMap{
            {"index", static_cast<qlonglong>(e.index)},
            {"time", logged.size() >= 16 ? logged.mid(11, 5) : QStringLiteral("—")},
            {"commitment", QString::fromStdString(e.commitment)},
            {"commitmentShort", shortHex(e.commitment)},
            {"sizeKb", e.sizeKb},
            {"mine", own != mineByIndex.end()},
            {"status", status},
        });
    }
    return out;
}

QVariantList TransparencyLog::pendingMine() const {
    QVariantList out;
    for (const auto& m : mine_) {
        if (m.status == QStringLiteral("pending")) out.append(QVariantMap{{"messageId", QString::fromStdString(m.messageId)}, {"commitmentShort", shortHex(m.commitment)}});
    }
    return out;
}

int TransparencyLog::mineMatched() const {
    return static_cast<int>(std::count_if(mine_.begin(), mine_.end(), [](const Mine& m) { return m.status == QStringLiteral("match"); }));
}

int TransparencyLog::mineMismatches() const {
    return static_cast<int>(std::count_if(mine_.begin(), mine_.end(), [](const Mine& m) { return m.status == QStringLiteral("mismatch"); }));
}

int TransparencyLog::failedRows() const {
    int failed = 0;
    for (const auto& e : rows()) {
        const auto proof = proofs_.find(proofKey(e.index));
        if (proof != proofs_.end() && !proof->second) ++failed;
    }
    return failed;
}

void TransparencyLog::select(qint64 index) {
    const auto& list = rows();
    const auto it = std::find_if(list.begin(), list.end(), [index](const auto& e) { return e.index == index; });
    if (it == list.end()) return;
    const QString logged = QString::fromStdString(it->loggedAt);
    QString mineStatus;
    for (const auto& m : mine_) {
        if (m.logIndex && *m.logIndex == index) mineStatus = m.status;
    }
    selected_ = QVariantMap{
        {"index", static_cast<qlonglong>(it->index)},
        {"commitment", QString::fromStdString(it->commitment)},
        {"loggedAt", logged.size() >= 16 ? logged.left(10) + u' ' + logged.mid(11, 5) + QStringLiteral(" UTC") : QStringLiteral("—")},
        {"checkpointSize", static_cast<qlonglong>(it->checkpointSize)},
        {"sizeKb", it->sizeKb},
        {"mine", mineStatus},
        {"checking", true},
        {"ok", false},
        {"path", QStringList()},
    };
    emit selectedChanged();
    recheckSelected();
}

void TransparencyLog::recheckSelected() {
    if (selected_.isEmpty() || !checkpoint_) return;
    selected_["checking"] = true;
    emit selectedChanged();
    auto api = api_;
    const auto cp = *checkpoint_;
    const std::int64_t index = selected_.value("index").toLongLong();
    const std::string commitment = selected_.value("commitment").toString().toStdString();
    struct Result {
        bool ok = false;
        std::vector<std::string> path;
    };
    runInBackground(
        pool_, this,
        [api, cp, index, commitment] {
            Result r;
            r.ok = inclusionOk(*api, commitment, index, cp, &r.path);
            return r;
        },
        [this, index](Result r) {
            if (selected_.value("index").toLongLong() != index) return;
            QStringList path;
            for (const auto& h : r.path) path.append(QString::fromStdString(h));
            selected_["checking"] = false;
            selected_["ok"] = r.ok;
            selected_["path"] = path;
            emit selectedChanged();
        },
        [this, index](const Failure&) {
            if (selected_.value("index").toLongLong() != index) return;
            selected_["checking"] = false;
            selected_["ok"] = false;
            emit selectedChanged();
        });
}

void TransparencyLog::copySelectedProof() {
    if (selected_.isEmpty()) return;
    QJsonObject proof{{"index", selected_.value("index").toLongLong()},
                      {"treeSize", checkpoint_ ? static_cast<qint64>(checkpoint_->treeSize) : 0},
                      {"path", QJsonArray::fromStringList(selected_.value("path").toStringList())}};
    QGuiApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(proof).toJson(QJsonDocument::Compact)));
}

void TransparencyLog::exportTo(const QUrl& file) {
    QJsonObject cp;
    if (checkpoint_) {
        cp = QJsonObject{{"treeSize", static_cast<qint64>(checkpoint_->treeSize)},
                         {"rootHash", QString::fromStdString(checkpoint_->rootHash)},
                         {"signedAt", QString::fromStdString(checkpoint_->signedAt)},
                         {"signature", QString::fromStdString(checkpoint_->signature)},
                         {"publicKey", QString::fromStdString(checkpoint_->publicKey)}};
    }
    QJsonArray entries;
    for (const auto& e : rows()) {
        entries.append(QJsonObject{{"index", static_cast<qint64>(e.index)},
                                   {"commitment", QString::fromStdString(e.commitment)},
                                   {"sizeKb", e.sizeKb},
                                   {"checkpointSize", static_cast<qint64>(e.checkpointSize)},
                                   {"loggedAt", QString::fromStdString(e.loggedAt)}});
    }
    QSaveFile out(file.isLocalFile() ? file.toLocalFile() : file.toString());
    if (!out.open(QIODevice::WriteOnly)) return;
    out.write(QJsonDocument(QJsonObject{{"checkpoint", cp}, {"entries", entries}}).toJson());
    out.commit();
}

// --- Relay servers ---

namespace {

QString hostOf(const std::string& url) {
    QString s = QString::fromStdString(url);
    s.remove(QRegularExpression(QStringLiteral("^turns?:")));
    const auto q = s.indexOf(u'?');
    return q >= 0 ? s.left(q) : s;
}

QString transportOf(const std::string& url) {
    if (url.rfind("turns:", 0) == 0) return QStringLiteral("TLS over TCP");
    const auto pos = url.find("transport=");
    if (pos == std::string::npos) return QStringLiteral("UDP");
    return QString::fromStdString(url.substr(pos + 10)).toUpper();
}

/// The desktop's engine relays over UDP; TCP and TLS relays are for browsers behind strict firewalls.
bool usableHere(const std::string& url) {
    return url.rfind("turn:", 0) == 0 && url.find("transport=tcp") == std::string::npos;
}

}  // namespace

RelayCheck::RelayCheck(std::shared_ptr<api::ApiClient> api, QObject* parent) : QObject(parent), api_(std::move(api)) {
    pool_.setMaxThreadCount(2);
}

RelayCheck::~RelayCheck() {
    pool_.clear();
    pool_.waitForDone(10000);
}

QString RelayCheck::apiHost() const {
    return QUrl(apiBase()).host();
}

QVariantList RelayCheck::relays() const {
    QVariantList out;
    for (const auto& r : relays_) {
        out.append(QVariantMap{{"url", QString::fromStdString(r.url)},
                               {"host", hostOf(r.url)},
                               {"transport", transportOf(r.url)},
                               {"status", r.status},
                               {"tone", r.tone}});
    }
    return out;
}

void RelayCheck::load() {
    auto api = api_;
    runInBackground(
        pool_, this, [api] { return live::relays(*api); },
        [this](std::vector<live::IceServer> servers) {
            relayError_.clear();
            relays_.clear();
            server_.reset();
            if (!servers.empty()) {
                server_ = servers.front();
                for (const auto& url : server_->urls) {
                    relays_.push_back({url, usableHere(url) ? QStringLiteral("Not checked") : QStringLiteral("Used by browsers"),
                                       QStringLiteral("muted")});
                }
            }
            emit changed();
        },
        [this](const Failure& failure) {
            relayError_ = failure.code == QStringLiteral("calls_disabled") ? QStringLiteral("No call relay is set up on this server, so calls are off.")
                                                                             : QStringLiteral("Couldn't load the call relays.");
            emit changed();
        });
}

void RelayCheck::checkAll() {
    apiStatus_ = QStringLiteral("Checking…");
    apiTone_ = QStringLiteral("muted");
    emit changed();
    auto api = api_;
    runInBackground(
        pool_, this,
        [api] {
            QElapsedTimer timer;
            timer.start();
            api->me();
            return static_cast<int>(timer.elapsed());
        },
        [this](int ms) {
            apiStatus_ = QStringLiteral("Reachable · %1 ms").arg(ms);
            apiTone_ = QStringLiteral("ok");
            emit changed();
        },
        [this](const Failure&) {
            apiStatus_ = QStringLiteral("Unreachable from here");
            apiTone_ = QStringLiteral("warn");
            emit changed();
        });

    if (!server_) return;
    for (auto& relay : relays_) {
        if (!usableHere(relay.url)) continue;
        relay.status = QStringLiteral("Checking…");
        relay.tone = QStringLiteral("muted");
        // Times how long it takes to get a relay address. Sends no media and no identity.
        const rtc::IceServer server{relay.url, server_->username, server_->credential};
        const std::string url = relay.url;
        runInBackground(
            pool_, this,
            [server] {
                QElapsedTimer timer;
                timer.start();
                rtc::Config config;
                config.servers = {server};
                rtc::CallMedia probe(config, {});
                probe.createOffer();  // throws when no relay address came back
                probe.close();
                return static_cast<int>(timer.elapsed());
            },
            [this, url](int ms) {
                for (auto& r : relays_) {
                    if (r.url == url) r = {url, QStringLiteral("Reachable · %1 ms").arg(ms), QStringLiteral("ok")};
                }
                emit changed();
            },
            [this, url](const Failure&) {
                for (auto& r : relays_) {
                    if (r.url == url) r = {url, QStringLiteral("Unreachable from here"), QStringLiteral("warn")};
                }
                emit changed();
            });
    }
    emit changed();
}

}  // namespace redvault::app
