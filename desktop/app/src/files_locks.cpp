// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Files: extra locks on files, and the person's own encryption keys
// (FilesSession's half for the Encryption keys page and the lock dialogs).
#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QMimeData>

#include <algorithm>

#include "async.hpp"
#include "files_session.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/drive_crypto.hpp"
#include "redvault/crypto/vault_crypto.hpp"

namespace redvault::app {

namespace fs = std::filesystem;
namespace vc = redvault::crypto::vault;

namespace {

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

QString formatBytes(std::int64_t bytes) {
    static const char* units[] = {"bytes", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    if (unit == 0 || value >= 100) return QStringLiteral("%1 %2").arg(qRound(value)).arg(QLatin1String(units[unit]));
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QLatin1String(units[unit]));
}

/// "Created Sep 3", "Imported today".
QString createdText(const data::FileKey& key) {
    const QString verb = key.imported ? QStringLiteral("Imported") : QStringLiteral("Created");
    if (key.createdMs <= 0) return verb;
    const QDate day = QDateTime::fromMSecsSinceEpoch(key.createdMs).date();
    const QDate today = QDate::currentDate();
    if (day == today) return verb + QStringLiteral(" today");
    if (day == today.addDays(-1)) return verb + QStringLiteral(" yesterday");
    if (day.year() == today.year()) return verb + u' ' + QLocale().toString(day, QStringLiteral("MMM d"));
    return verb + u' ' + QLocale().toString(day, QStringLiteral("MMM d, yyyy"));
}

/// "866d36ef c47cd4ba ...": 64 hex characters in groups of 8, easier to copy out by hand.
QString grouped(const std::string& hex) {
    QString out;
    for (std::size_t i = 0; i < hex.size(); i += 8) {
        if (!out.isEmpty()) out += u' ';
        out += qs(hex.substr(i, 8));
    }
    return out;
}

bool isHexKeyText(const QString& compact) {
    if (compact.isEmpty()) return false;
    return std::all_of(compact.begin(), compact.end(), [](QChar c) { return c.isDigit() || (c.toLower() >= u'a' && c.toLower() <= u'f'); });
}

}  // namespace

// --- Lookups ---

data::DriveEntry FilesSession::usable(const data::DriveEntry& entry) const {
    data::DriveEntry out = entry;
    if (!entry.locked()) return out;
    if (const auto it = unlocked_.find(entry.id); it != unlocked_.end()) out.meta["key"] = it->second;
    return out;
}

const data::FileKey* FilesSession::findKey(const std::string& id) const {
    for (const auto& key : keys_) {
        if (key.id == id) return &key;
    }
    return nullptr;
}

int FilesSession::lockedCount() const {
    int count = 0;
    for (const auto& [fileId, e] : currentVersions()) {
        if (!e->trashed && e->locked()) ++count;
    }
    return count;
}

int FilesSession::filesLockedWith(const QString& keyId) const {
    const std::string id = keyId.toStdString();
    int count = 0;
    for (const auto& [fileId, e] : currentVersions()) {
        if (data::filelock::lockKeyId(e->meta) == id) ++count;
    }
    return count;
}

bool FilesSession::keyNeedsPassword(const QString& keyId) const {
    const auto* key = findKey(keyId.toStdString());
    return key && key->passwordProtected && !openedKeys_.count(key->id);
}

QVariantList FilesSession::keys() const {
    QVariantList out;
    for (const auto& key : keys_) {
        out.append(QVariantMap{{"id", qs(key.id)},
                               {"name", qs(key.name)},
                               {"fingerprint", qs(key.fingerprint)},
                               {"short", qs(key.fingerprint.substr(0, 4))},
                               {"passwordProtected", key.passwordProtected},
                               {"protectedBy", key.passwordProtected ? QStringLiteral("Account + password") : QStringLiteral("Account only")},
                               {"createdText", createdText(key)},
                               {"files", filesLockedWith(qs(key.id))},
                               {"openNow", openedKeys_.count(key.id) > 0}});
    }
    return out;
}

QVariantMap FilesSession::selectedKey() const {
    const auto* key = findKey(selectedKeyId_.toStdString());
    if (!key) return {};
    const int files = filesLockedWith(selectedKeyId_);
    return {{"id", qs(key->id)},
            {"name", qs(key->name)},
            {"fingerprint", qs(key->fingerprint)},
            {"passwordProtected", key->passwordProtected},
            {"createdText", createdText(*key)},
            {"files", files},
            {"filesText", QStringLiteral("%1 locked file%2").arg(files).arg(files == 1 ? QString() : QStringLiteral("s"))}};
}

// --- Passwords ---

QString FilesSession::suggestPassword(bool words) const {
    return qs(data::filelock::suggestPassword(words));
}

QString FilesSession::passwordProblem(const QString& password) const {
    const auto problem = data::filelock::passwordProblem(password.toStdString());
    return problem ? qs(*problem) : QString();
}

QVariantMap FilesSession::strengthOf(const QString& password) const {
    if (password.isEmpty()) return {};
    const auto estimate = vc::estimateStrength(password.toStdString());
    const double fraction = estimate.strength == vc::Strength::Strong ? 1.0 : estimate.strength == vc::Strength::Fair ? 0.6 : 0.28;
    const QString tone = estimate.strength == vc::Strength::Strong ? QStringLiteral("good")
                         : estimate.strength == vc::Strength::Fair ? QStringLiteral("warn")
                                                                   : QStringLiteral("bad");
    return {{"label", qs(estimate.label)}, {"fraction", fraction}, {"tone", tone}, {"length", static_cast<int>(password.size())}};
}

// --- Busy state and results ---

void FilesSession::setLockBusy(bool busy) {
    if (lockBusy_ == busy) return;
    lockBusy_ = busy;
    emit lockBusyChanged();
}

void FilesSession::finishLock(const QString& action, const QString& error) {
    setLockBusy(false);
    emit lockDone(action, error);
}

void FilesSession::withLockSpec(const QString& action, const QString& mode, const QString& keyId, const QString& keyPassword,
                                const QString& filePassword, std::function<void(std::shared_ptr<LockSpec>)> then) {
    if (mode == "none") return then(nullptr);
    auto spec = std::make_shared<LockSpec>();
    if (mode == "password") {
        if (const QString problem = passwordProblem(filePassword); !problem.isEmpty()) return finishLock(action, problem);
        spec->kind = data::FileLock::Kind::Password;
        spec->password = filePassword.toStdString();
        return then(spec);
    }
    const auto* key = findKey(keyId.toStdString());
    if (!key) return finishLock(action, QStringLiteral("Choose a key."));
    spec->kind = data::FileLock::Kind::Key;
    spec->keyId = key->id;
    spec->fingerprint = key->fingerprint;
    if (const auto opened = openedKeys_.find(key->id); opened != openedKeys_.end()) {
        spec->key = opened->second;
        return then(spec);
    }
    // Open the key (Argon2id when it has a password), once per session.
    setLockBusy(true);
    auto keys = keyRepository_;
    const data::FileKey keyCopy = *key;
    const std::string password = keyPassword.toStdString();
    runInBackground(
        pool_, this, [keys, keyCopy, password] { return std::make_shared<crypto::SecretBytes>(keys->open(keyCopy, password)); },
        [this, spec, then](std::shared_ptr<crypto::SecretBytes> secret) {
            openedKeys_[spec->keyId] = secret;
            spec->key = secret;
            emit keysChanged();
            then(spec);
        },
        [this, action](const Failure& failure) { finishLock(action, failure.message); });
}

// --- Locking files ---

QVariantList FilesSession::describeFiles(const QList<QUrl>& files) const {
    QVariantList out;
    for (const auto& url : files) {
        const QFileInfo info(url.toLocalFile());
        if (!info.isFile()) continue;
        const QString ext = info.suffix().toLower();
        out.append(QVariantMap{{"name", info.fileName()},
                               {"size", formatBytes(info.size())},
                               {"typeLabel", ext.isEmpty() ? QStringLiteral("FILE") : ext.left(4).toUpper()}});
    }
    return out;
}

void FilesSession::uploadLocked(const QList<QUrl>& files, const QString& mode, const QString& keyId, const QString& keyPassword,
                                const QString& filePassword) {
    withLockSpec(QStringLiteral("upload"), mode, keyId, keyPassword, filePassword, [this, files](std::shared_ptr<LockSpec> spec) {
        // The lock goes on each upload before it's queued: the first one starts right away.
        enqueueUploads(files, spec);
        finishLock(QStringLiteral("upload"), {});
    });
}

void FilesSession::unlockFile(const QString& id, const QString& mode, const QString& keyId, const QString& keyPassword,
                              const QString& filePassword) {
    const auto* e = find(id);
    if (!e || !e->locked()) return finishLock(QStringLiteral("unlock"), {});
    const data::DriveEntry entry = *e;
    auto repository = repository_;
    auto run = [this, repository, entry, id](data::FileLock lock, std::shared_ptr<LockSpec> keep) {
        setLockBusy(true);
        runInBackground(
            pool_, this,
            [repository, entry, lock, keep] {
                (void)keep;  // keeps the borrowed key alive for the call
                return repository->unlock(entry, lock).meta.at("key").get<std::string>();
            },
            [this, id, entry](std::string fileKey) {
                unlocked_[entry.id] = std::move(fileKey);
                emit changed();
                emit selectedChanged();
                emit keysChanged();
                if (entry.mimeType.rfind("image/", 0) == 0 && !previews_.count(entry.id)) loadPreview(entry);
                finishLock(QStringLiteral("unlock"), {});
            },
            [this](const Failure& failure) { finishLock(QStringLiteral("unlock"), failure.message); });
    };
    if (mode == "password") {
        // Any password is worth trying here: it was chosen when the file was locked.
        if (filePassword.isEmpty()) return finishLock(QStringLiteral("unlock"), QStringLiteral("Enter the file's password."));
        return run(data::FileLock::withPassword(filePassword.toStdString()), nullptr);
    }
    withLockSpec(QStringLiteral("unlock"), QStringLiteral("key"), keyId, keyPassword, {},
                 [run](std::shared_ptr<LockSpec> spec) { run(spec->lock(), spec); });
}

void FilesSession::lockNow(const QString& id) {
    const std::string key = id.toStdString();
    unlocked_.erase(key);
    if (const auto preview = previews_.find(key); preview != previews_.end()) {
        std::error_code ignored;
        fs::remove(fs::path(QUrl(preview->second).toLocalFile().toStdWString()), ignored);
        previews_.erase(preview);
    }
    // Copies opened in other apps go too, so a locked file leaves no plaintext behind
    // (one still open elsewhere can't be removed on Windows; the cache is wiped at sign-out).
    if (const auto copies = openedCopies_.find(key); copies != openedCopies_.end()) {
        for (const auto& path : copies->second) {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
        openedCopies_.erase(copies);
    }
    emit changed();
    emit selectedChanged();
    setMessages({}, QStringLiteral("Locked again."));
}

void FilesSession::changeLock(const QString& id, const QString& mode, const QString& keyId, const QString& keyPassword,
                              const QString& filePassword) {
    const auto* e = find(id);
    if (!e || e->incoming || e->kind != "file") return finishLock(QStringLiteral("changeLock"), {});
    const data::DriveEntry entry = usable(*e);
    if (entry.needsUnlock()) return finishLock(QStringLiteral("changeLock"), QStringLiteral("Unlock this file first."));
    auto repository = repository_;
    withLockSpec(QStringLiteral("changeLock"), mode, keyId, keyPassword, filePassword, [this, repository, entry](std::shared_ptr<LockSpec> spec) {
        setLockBusy(true);
        runInBackground(
            pool_, this,
            [repository, entry, spec] {
                if (!spec) return repository->setLock(entry, nullptr);
                const auto lock = spec->lock();
                return repository->setLock(entry, &lock);
            },
            [this, locking = spec != nullptr](data::DriveEntry saved) {
                // Still usable this session: it was unlocked to change it.
                if (locking) unlocked_[saved.id] = saved.meta.at("key").get<std::string>();
                else unlocked_.erase(saved.id);
                saved.meta.erase("key");
                if (auto* x = findMutable(qs(saved.id))) {
                    saved.sharedWith = x->sharedWith;
                    *x = saved;
                }
                emit changed();
                emit selectedChanged();
                emit keysChanged();
                setMessages({}, locking ? QStringLiteral("Lock saved. Nothing was re-encrypted or re-uploaded.") : QStringLiteral("Lock removed."));
                finishLock(QStringLiteral("changeLock"), {});
            },
            [this](const Failure& failure) { finishLock(QStringLiteral("changeLock"), failure.message); });
    });
}

// --- Encryption keys ---

void FilesSession::refreshKeys() {
    auto keys = keyRepository_;
    runInBackground(
        pool_, this, [keys] { return keys->list(); },
        [this](std::vector<data::FileKey> list) {
            keys_ = std::move(list);
            keysLoaded_ = true;
            // Forget opened keys that were deleted (here or on another device).
            for (auto it = openedKeys_.begin(); it != openedKeys_.end();) {
                it = findKey(it->first) ? std::next(it) : openedKeys_.erase(it);
            }
            if (!findKey(selectedKeyId_.toStdString())) selectedKeyId_ = keys_.empty() ? QString() : qs(keys_.front().id);
            emit keysChanged();
            emit selectedChanged();
        },
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
        });
}

void FilesSession::selectKey(const QString& id) {
    selectedKeyId_ = id;
    exportedKey_.clear();
    emit keysChanged();
}

void FilesSession::createKey(const QString& name, const QString& password) {
    if (!password.isEmpty()) {
        if (const QString problem = passwordProblem(password); !problem.isEmpty()) return finishLock(QStringLiteral("createKey"), problem);
    }
    setLockBusy(true);
    auto keys = keyRepository_;
    const std::string n = name.trimmed().toStdString();
    const std::string p = password.toStdString();
    runInBackground(
        pool_, this, [keys, n, p] { return keys->create(n, p); },
        [this](data::FileKey key) {
            keys_.push_back(key);
            selectedKeyId_ = qs(key.id);
            exportedKey_.clear();
            emit keysChanged();
            setMessages({}, QStringLiteral("Key created. Export a copy for backup and keep it offline."));
            finishLock(QStringLiteral("createKey"), {});
        },
        [this](const Failure& failure) { finishLock(QStringLiteral("createKey"), failure.message); });
}

QVariantMap FilesSession::checkImport(const QString& text) const {
    QString compact;
    for (const QChar c : text) {
        if (!c.isSpace()) compact += c;
    }
    if (compact.isEmpty()) return {{"state", QStringLiteral("empty")}};
    QString hex = compact;
    hex.remove(u'-').remove(u':');
    const bool looksHex = isHexKeyText(hex);
    const auto key = crypto::drive::parseFileLockKey(text.toStdString());
    if (!key) {
        QString message;
        if (looksHex) {
            message = QStringLiteral("That's %1 bits. A key must be exactly 256 bits (64 hex characters).").arg(hex.size() * 4);
        } else {
            std::size_t bytes = 0;
            try {
                QString b64 = compact;
                b64.replace(u'-', u'+').replace(u'_', u'/');
                while (b64.size() % 4 != 0) b64 += u'=';
                bytes = crypto::base64Decode(b64.toStdString()).size();
            } catch (const std::exception&) {
                bytes = 0;
            }
            message = bytes > 0 ? QStringLiteral("That's %1 bits. A key must be exactly 256 bits (64 hex characters).").arg(bytes * 8)
                                : QStringLiteral("That isn't a key. Paste 64 hex characters, or the base64 or base64url form of a 256-bit key.");
        }
        return {{"state", QStringLiteral("invalid")}, {"message", message}};
    }
    const std::string fingerprint = crypto::drive::fileLockKeyFingerprint(key->view());
    for (const auto& existing : keys_) {
        if (existing.fingerprint == fingerprint) {
            return {{"state", QStringLiteral("duplicate")}, {"name", qs(existing.name)}, {"fingerprint", qs(fingerprint)}};
        }
    }
    const bool base64Url = compact.contains(u'-') || compact.contains(u'_');
    return {{"state", QStringLiteral("valid")},
            {"format", looksHex ? QStringLiteral("hex") : base64Url ? QStringLiteral("base64url") : QStringLiteral("base64")},
            {"fingerprint", qs(fingerprint)}};
}

void FilesSession::importKey(const QString& name, const QString& text, const QString& password) {
    if (!password.isEmpty()) {
        if (const QString problem = passwordProblem(password); !problem.isEmpty()) return finishLock(QStringLiteral("importKey"), problem);
    }
    setLockBusy(true);
    auto keys = keyRepository_;
    const std::string n = name.trimmed().toStdString();
    const std::string t = text.toStdString();
    const std::string p = password.toStdString();
    runInBackground(
        pool_, this, [keys, n, t, p] { return keys->import(n, t, p); },
        [this](data::FileKey key) {
            keys_.push_back(key);
            selectedKeyId_ = qs(key.id);
            exportedKey_.clear();
            emit keysChanged();
            emit selectedChanged();
            setMessages({}, QStringLiteral("Key imported. Files locked with it can be unlocked here now."));
            finishLock(QStringLiteral("importKey"), {});
        },
        [this](const Failure& failure) { finishLock(QStringLiteral("importKey"), failure.message); });
}

void FilesSession::renameKey(const QString& id, const QString& name) {
    const auto* key = findKey(id.toStdString());
    if (!key || name.trimmed().isEmpty()) return;
    auto keys = keyRepository_;
    const data::FileKey copy = *key;
    const std::string n = name.trimmed().toStdString();
    runInBackground(
        pool_, this, [keys, copy, n] { return keys->rename(copy, n); },
        [this](data::FileKey renamed) {
            for (auto& k : keys_) {
                if (k.id == renamed.id) k = renamed;
            }
            emit keysChanged();
            emit selectedChanged();
            finishLock(QStringLiteral("renameKey"), {});
        },
        [this](const Failure& failure) { finishLock(QStringLiteral("renameKey"), failure.message); });
}

void FilesSession::setKeyPassword(const QString& id, const QString& current, const QString& next) {
    const auto* key = findKey(id.toStdString());
    if (!key) return finishLock(QStringLiteral("keyPassword"), QStringLiteral("That key isn't there anymore."));
    if (!next.isEmpty()) {
        if (const QString problem = passwordProblem(next); !problem.isEmpty()) return finishLock(QStringLiteral("keyPassword"), problem);
    }
    setLockBusy(true);
    auto keys = keyRepository_;
    const data::FileKey copy = *key;
    const std::string c = current.toStdString();
    const std::string n = next.toStdString();
    runInBackground(
        pool_, this, [keys, copy, c, n] { return keys->setPassword(copy, c, n); },
        [this, removed = next.isEmpty(), hadOne = key->passwordProtected](data::FileKey updated) {
            for (auto& k : keys_) {
                if (k.id == updated.id) k = updated;
            }
            emit keysChanged();
            emit selectedChanged();
            setMessages({}, removed ? QStringLiteral("Password removed. Your account alone protects this key now.")
                            : hadOne ? QStringLiteral("Password changed. Locked files weren't re-encrypted.")
                                     : QStringLiteral("Password set. Using this key now needs it too."));
            finishLock(QStringLiteral("keyPassword"), {});
        },
        [this](const Failure& failure) { finishLock(QStringLiteral("keyPassword"), failure.message); });
}

void FilesSession::revealKeyExport(const QString& id, const QString& password) {
    const auto* key = findKey(id.toStdString());
    if (!key) return finishLock(QStringLiteral("export"), QStringLiteral("That key isn't there anymore."));
    if (const auto opened = openedKeys_.find(key->id); opened != openedKeys_.end()) {
        selectedKeyId_ = id;
        exportedKey_ = grouped(crypto::drive::fileLockKeyHex(opened->second->view()));
        emit keysChanged();
        return finishLock(QStringLiteral("export"), {});
    }
    setLockBusy(true);
    auto keys = keyRepository_;
    const data::FileKey copy = *key;
    const std::string p = password.toStdString();
    runInBackground(
        pool_, this, [keys, copy, p] { return std::make_shared<crypto::SecretBytes>(keys->open(copy, p)); },
        [this, id](std::shared_ptr<crypto::SecretBytes> secret) {
            openedKeys_[id.toStdString()] = secret;
            selectedKeyId_ = id;
            exportedKey_ = grouped(crypto::drive::fileLockKeyHex(secret->view()));
            emit keysChanged();
            finishLock(QStringLiteral("export"), {});
        },
        [this](const Failure& failure) { finishLock(QStringLiteral("export"), failure.message); });
}

void FilesSession::hideKeyExport() {
    exportedKey_.clear();
    emit keysChanged();
}

void FilesSession::copyKeyExport() {
    if (exportedKey_.isEmpty()) return;
    QString text = exportedKey_;
    text.remove(u' ');
    auto* mime = new QMimeData;
    mime->setText(text);
#ifdef _WIN32
    // Keep the key out of Windows clipboard history and cloud clipboard sync.
    const QByteArray zero(4, '\0');
    mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\""), zero);
    mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanIncludeInClipboardHistory\""), zero);
    mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanUploadToCloudClipboard\""), zero);
#endif
    QGuiApplication::clipboard()->setMimeData(mime);
    clipboardText_ = text;
    clipboardTimer_.start();
    setMessages({}, QStringLiteral("Key copied. It clears from the clipboard in 30 seconds."));
}

void FilesSession::deleteKey(const QString& id) {
    const auto* key = findKey(id.toStdString());
    if (!key) return;
    auto keys = keyRepository_;
    const data::FileKey copy = *key;
    runInBackground(
        pool_, this, [keys, copy] { keys->remove(copy); },
        [this, id] {
            const std::string k = id.toStdString();
            openedKeys_.erase(k);
            keys_.erase(std::remove_if(keys_.begin(), keys_.end(), [&](const data::FileKey& x) { return x.id == k; }), keys_.end());
            selectedKeyId_ = keys_.empty() ? QString() : qs(keys_.front().id);
            exportedKey_.clear();
            emit keysChanged();
            emit selectedChanged();
            setMessages({}, QStringLiteral("Key deleted."));
            finishLock(QStringLiteral("deleteKey"), {});
        },
        [this](const Failure& failure) { finishLock(QStringLiteral("deleteKey"), failure.message); });
}

}  // namespace redvault::app
