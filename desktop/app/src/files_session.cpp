// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "files_session.hpp"

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QLockFile>
#include <QStandardPaths>

#include <algorithm>
#include <set>
#include <system_error>

#include "async.hpp"
#include "chat_session.hpp"
#include "file_safety.hpp"
#include "prefs.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/util/text.hpp"
#include "tor_controller.hpp"

namespace redvault::app {

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr int kRefreshMs = 60 * 1000;
constexpr int kPcBackupMs = 15 * 60 * 1000;
constexpr std::int64_t kMaxPreviewBytes = 25LL * 1024 * 1024;
constexpr int kMaxPcBackupFiles = 20000;

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

QString whenText(std::int64_t ms) {
    if (ms <= 0) return QStringLiteral("—");
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(ms);
    const QDate today = QDate::currentDate();
    const qint64 ago = QDateTime::currentMSecsSinceEpoch() - ms;
    if (ago < 60 * 1000) return QStringLiteral("Now");
    if (when.date() == today) return QStringLiteral("Today %1").arg(when.toString(QStringLiteral("HH:mm")));
    if (when.date() == today.addDays(-1)) return QStringLiteral("Yesterday");
    if (when.date().year() == today.year()) return QLocale().toString(when.date(), QStringLiteral("MMM d"));
    return QLocale().toString(when.date(), QStringLiteral("MMM d, yyyy"));
}

/// The type tile: a short label and its tint (the design's per-type colours).
std::pair<QString, QString> typeOf(const QString& name, bool folder) {
    if (folder) return {QStringLiteral("DIR"), QStringLiteral("#86D9AE")};
    const QString ext = QFileInfo(name).suffix().toLower();
    const QString label = ext.isEmpty() ? QStringLiteral("FILE") : ext.left(4).toUpper();
    static const QStringList sheets = {"xlsx", "xls", "csv", "ods", "numbers"};
    static const QStringList images = {"jpg", "jpeg", "png", "gif", "webp", "heic", "bmp", "svg", "tif", "tiff"};
    static const QStringList docs = {"doc", "docx", "txt", "md", "rtf", "odt", "pages", "ppt", "pptx", "key"};
    static const QStringList videos = {"mp4", "mov", "webm", "mkv", "avi", "m4v"};
    if (ext == "pdf") return {label, QStringLiteral("#F2C38B")};
    if (sheets.contains(ext)) return {label, QStringLiteral("#B9D98C")};
    if (images.contains(ext)) return {label, QStringLiteral("#9CC9E8")};
    if (docs.contains(ext)) return {label, QStringLiteral("#9FB4FF")};
    if (videos.contains(ext)) return {label, QStringLiteral("#E8A7A0")};
    return {label, QStringLiteral("#C9CFCA")};
}

bool isImage(const data::DriveEntry& e) {
    return QString::fromStdString(e.mimeType).startsWith(QStringLiteral("image/"));
}

fs::path localPath(const QUrl& url) {
    return fs::path(url.toLocalFile().toStdWString());
}

QString lockPathFor(const fs::path& cacheDir) {
    return QString::fromStdWString(cacheDir.wstring()) + QStringLiteral(".lock");
}

/// Decrypted previews and opened files left by a session that never closed
/// (a crash, a forced quit). A running session holds its folder's lock, so
/// another open copy of the app keeps its own.
void sweepAbandonedCaches(const fs::path& root) {
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path dir = it->path();
        if (!it->is_directory(ec) || dir.filename().string().rfind("files-", 0) != 0) continue;
        // No lock at all: an older version of the app, which may still be
        // running. Only once it has sat untouched for a day.
        if (!QFileInfo::exists(lockPathFor(dir)) &&
            QFileInfo(QString::fromStdWString(dir.wstring())).lastModified().secsTo(QDateTime::currentDateTime()) < 24 * 60 * 60) {
            continue;
        }
        QLockFile lock(lockPathFor(dir));
        lock.setStaleLockTime(0);  // taken over only when its app is no longer running
        if (!lock.tryLock(0)) continue;
        std::error_code ignored;
        fs::remove_all(dir, ignored);
        lock.unlock();
    }
}

}  // namespace

FilesSession::FilesSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, ChatSession* chats,
                           QString webAppBase, TorController* tor, QObject* parent)
    : QObject(parent), api_(std::move(api)), identity_(std::move(identity)), chats_(chats), webAppBase_(std::move(webAppBase)) {
    pool_.setMaxThreadCount(3);
    uploadPool_.setMaxThreadCount(1);
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    cacheDir_ = fs::path(cacheRoot.toStdWString()) / ("files-" + util::randomUuid());
    QDir().mkpath(cacheRoot);
    cacheLock_ = std::make_unique<QLockFile>(lockPathFor(cacheDir_));
    cacheLock_->setStaleLockTime(0);
    cacheLock_->tryLock(0);
    sweepAbandonedCaches(fs::path(cacheRoot.toStdWString()));
    repository_ = std::make_shared<data::DriveRepository>(api_, identity_, cacheDir_, data::DriveRepository::Sleep{}, prefs::contactKeyPinStore());
    // Over Tor a big file can take hours: uploads go in parts that survive a dropped circuit or a
    // restart (docs/tor-routing-plan.md). Unfinished ones from last time come back paused.
    if (tor != nullptr && tor->enabled()) {
        tor_ = tor;
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        repository_->enableResumable(fs::path(root.toStdWString()) / "transfers" / identity_->userId,
                                     data::DriveRepository::deriveJournalKey(identity_->privateKey));
        for (const auto& pending : repository_->pendingUploads()) {
            Upload u;
            u.localId = QStringLiteral("upload:") + qs(pending.objectId);
            u.resumeObjectId = pending.objectId;
            u.parent = pending.parent;
            u.name = qs(pending.name);
            u.size = pending.size;
            u.stage = QStringLiteral("paused");
            u.error = QStringLiteral("Paused · resumes when Tor connects");
            uploads_.push_back(std::move(u));
        }
        connect(tor_, &TorController::becameConnected, this, &FilesSession::resumePaused);
        pausedTimer_.setInterval(20 * 1000);
        connect(&pausedTimer_, &QTimer::timeout, this, &FilesSession::resumePaused);
        pausedTimer_.start();
        if (tor_->connected()) QTimer::singleShot(0, this, &FilesSession::resumePaused);
    }
    vault_ = std::make_shared<data::VaultRepository>(api_, identity_, prefs::vaultRevStore(), prefs::contactKeyPinStore());
    keyRepository_ = std::make_shared<data::FileKeyRepository>(vault_);
    clipboardTimer_.setSingleShot(true);
    clipboardTimer_.setInterval(30 * 1000);
    connect(&clipboardTimer_, &QTimer::timeout, this, [this] {
        if (QGuiApplication::clipboard()->text() == clipboardText_) QGuiApplication::clipboard()->clear();
        clipboardText_.clear();
    });

    refreshTimer_.setInterval(kRefreshMs);
    connect(&refreshTimer_, &QTimer::timeout, this, &FilesSession::refresh);
    refreshTimer_.start();

    pcBackupPath_ = prefs::text(identity_->userId, "pcBackupPath");
    pcBackupTimer_.setInterval(kPcBackupMs);
    connect(&pcBackupTimer_, &QTimer::timeout, this, &FilesSession::runPcBackup);
    if (!pcBackupPath_.isEmpty()) {
        pcBackupStatus_ = QStringLiteral("Auto-synced");
        pcBackupTimer_.start();
    }
    refresh();
    refreshKeys();
}

FilesSession::~FilesSession() {
    if (!clipboardText_.isEmpty() && QGuiApplication::clipboard()->text() == clipboardText_) QGuiApplication::clipboard()->clear();
    uploads_.clear();
    uploadPool_.clear();
    pool_.clear();
    uploadPool_.waitForDone();
    pool_.waitForDone();
    std::error_code ignored;
    fs::remove_all(cacheDir_, ignored);  // decrypted previews and files opened this session
    cacheLock_.reset();
}

// --- Loading ---

void FilesSession::refresh() {
    auto repository = repository_;
    const int generation = ++refreshGeneration_;
    runInBackground(
        pool_, this, [repository] { return repository->refresh(); },
        [this, generation](data::DriveSnapshot snapshot) {
            if (generation == refreshGeneration_) apply(std::move(snapshot));
        },
        [this](const Failure& failure) {
            if (failure.status == 401) return emit sessionExpired();
            if (!loaded_) setMessages(failure.message, {});
        });
}

void FilesSession::apply(data::DriveSnapshot snapshot) {
    snapshot_ = std::move(snapshot);
    const bool first = !loaded_;
    loaded_ = true;
    // The folder we were in may have been deleted elsewhere.
    if (!folderId_.empty() && !find(qs(folderId_))) folderId_.clear();
    emit changed();
    emit selectedChanged();
    emit snapshotApplied();
    if (first && !pcBackupPath_.isEmpty()) runPcBackup();
}

const data::DriveEntry* FilesSession::find(const QString& id) const {
    const std::string key = id.toStdString();
    for (const auto& e : snapshot_.entries) {
        if (e.id == key) return &e;
    }
    for (const auto& e : snapshot_.incoming) {
        if (e.id == key) return &e;
    }
    return nullptr;
}

data::DriveEntry* FilesSession::findMutable(const QString& id) {
    const std::string key = id.toStdString();
    for (auto& e : snapshot_.entries) {
        if (e.id == key) return &e;
    }
    return nullptr;
}

std::map<std::string, const data::DriveEntry*> FilesSession::currentVersions() const {
    std::map<std::string, const data::DriveEntry*> out;
    for (const auto& e : snapshot_.entries) {
        if (e.kind != "file") continue;
        auto& slot = out[e.fileId];
        if (!slot || e.version > slot->version) slot = &e;
    }
    return out;
}

int FilesSession::versionsOf(const data::DriveEntry& entry) const {
    return static_cast<int>(std::count_if(snapshot_.entries.begin(), snapshot_.entries.end(),
                                          [&](const data::DriveEntry& e) { return e.kind == "file" && e.fileId == entry.fileId; }));
}

QString FilesSession::handleFor(const std::string& userId) const {
    if (chats_) {
        for (const auto& c : chats_->contacts()) {
            const auto m = c.toMap();
            if (m.value("userId").toString() == qs(userId)) return m.value("handle").toString();
        }
    }
    return QStringLiteral("Someone");
}

// --- Views ---

void FilesSession::setSection(const QString& section) {
    if (section_ == section && folderId_.empty()) return;
    section_ = section;
    folderId_.clear();
    if (!notice_.isEmpty()) setMessages(error_, {});  // it was about the page being left
    emit changed();
}

void FilesSession::setSearch(const QString& search) {
    if (search_ == search) return;
    search_ = search;
    emit changed();
}

void FilesSession::openFolder(const QString& id) {
    section_ = QStringLiteral("all");
    folderId_ = id.toStdString();
    search_.clear();
    if (!notice_.isEmpty()) setMessages(error_, {});
    emit changed();
}

QVariantList FilesSession::breadcrumb() const {
    QVariantList out;
    std::string cursor = folderId_;
    int guard = 0;
    while (!cursor.empty() && guard++ < 64) {
        const auto* folder = find(qs(cursor));
        if (!folder) break;
        out.prepend(QVariantMap{{"id", qs(folder->id)}, {"name", qs(folder->name)}});
        cursor = folder->parent;
    }
    return out;
}

QVariantList FilesSession::folders() const {
    QVariantList out;
    if (section_ != "all" || !search_.trimmed().isEmpty()) return out;
    const auto current = currentVersions();
    for (const auto& e : snapshot_.entries) {
        if (e.kind != "folder" || e.trashed || e.parent != folderId_) continue;
        int items = 0;
        bool shared = false;
        for (const auto& child : snapshot_.entries) {
            if (child.parent != e.id || child.trashed) continue;
            if (child.kind == "folder" || current.at(child.fileId) == &child) ++items;
            if (!child.sharedWith.empty()) shared = true;
        }
        const bool pcBackup = e.meta.value("pcBackup", false);
        QString detail = QStringLiteral("%1 item%2").arg(QLocale().toString(items), items == 1 ? QString() : QStringLiteral("s"));
        if (pcBackup) detail = pcBackupPath_.isEmpty() ? QStringLiteral("Not syncing") : pcBackupStatus_;
        else if (shared) detail = QStringLiteral("Shared · ") + detail;
        out.append(QVariantMap{{"id", qs(e.id)}, {"name", qs(e.name)}, {"detail", detail}, {"pcBackup", pcBackup}});
    }
    std::sort(out.begin(), out.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("name").toString().compare(b.toMap().value("name").toString(), Qt::CaseInsensitive) < 0;
    });
    return out;
}

QVariantMap FilesSession::rowFor(const data::DriveEntry& e) const {
    const auto [label, tint] = typeOf(qs(e.name), e.kind == "folder");
    QString sharedText = QStringLiteral("Only you");
    if (e.incoming) {
        sharedText = QStringLiteral("From ") + handleFor(e.ownerId);
    } else if (!e.sharedWith.empty()) {
        sharedText = handleFor(e.sharedWith.front());
        if (e.sharedWith.size() > 1) sharedText += QStringLiteral(" +%1").arg(e.sharedWith.size() - 1);
    }
    return {
        {"id", qs(e.id)},
        {"kind", qs(e.kind)},
        {"name", qs(e.name)},
        {"typeLabel", label},
        {"tint", tint},
        {"shared", sharedText},
        {"modified", whenText(e.modifiedMs)},
        {"size", e.kind == "folder" ? QStringLiteral("—") : formatBytes(e.size)},
        {"status", QStringLiteral("synced")},
        {"progress", 1.0},
        {"incoming", e.incoming},
        {"trashed", e.trashed},
        {"lock", qs(data::filelock::lockKind(e.meta))},
        {"lockShort", qs(data::filelock::lockFingerprint(e.meta).substr(0, 4))},
        {"unlocked", e.locked() && unlocked_.count(e.id) > 0},
    };
}

QVariantList FilesSession::rows() const {
    QVariantList out;
    const QString q = search_.trimmed().toLower();
    const auto current = currentVersions();
    std::vector<const data::DriveEntry*> picked;

    if (section_ == "shared") {
        for (const auto& e : snapshot_.incoming) picked.push_back(&e);
    } else if (section_ == "trash") {
        for (const auto& e : snapshot_.entries) {
            if (e.trashed && (e.kind == "folder" || current.at(e.fileId) == &e)) picked.push_back(&e);
        }
    } else if (section_ == "links") {
        std::set<std::string> linked;
        for (const auto& l : snapshot_.links) linked.insert(l.backupId);
        for (const auto& e : snapshot_.entries) {
            if (linked.count(e.id) && !e.trashed) picked.push_back(&e);
        }
    } else {
        for (const auto& [fileId, e] : current) {
            if (e->trashed) continue;
            if (!q.isEmpty()) {
                picked.push_back(e);  // search covers every folder
            } else if (section_ == "recent") {
                picked.push_back(e);
            } else if (section_ == "photos") {
                if (isImage(*e)) picked.push_back(e);
            } else if (section_ == "locked") {
                if (e->locked()) picked.push_back(e);
            } else if (e->parent == folderId_) {
                picked.push_back(e);
            }
        }
        if (!q.isEmpty()) {
            for (const auto& e : snapshot_.entries) {
                if (e.kind == "folder" && !e.trashed) picked.push_back(&e);
            }
            for (const auto& e : snapshot_.incoming) picked.push_back(&e);
        }
    }
    if (!q.isEmpty()) {
        picked.erase(std::remove_if(picked.begin(), picked.end(), [&](const data::DriveEntry* e) { return !qs(e->name).toLower().contains(q); }),
                     picked.end());
    }
    std::sort(picked.begin(), picked.end(), [&](const data::DriveEntry* a, const data::DriveEntry* b) {
        if (section_ == "recent" || section_ == "trash") return a->modifiedMs > b->modifiedMs;
        if (a->kind != b->kind) return a->kind == "folder";
        return qs(a->name).compare(qs(b->name), Qt::CaseInsensitive) < 0;
    });
    if (section_ == "recent" && picked.size() > 100) picked.resize(100);

    // Uploads in progress (here, in their folder) come first.
    if (section_ == "all" && q.isEmpty()) {
        for (const auto& u : uploads_) {
            if (u.parent != folderId_) continue;
            const auto [label, tint] = typeOf(u.name, false);
            const QString status = u.stage == "failed" ? QStringLiteral("failed") : u.stage;
            out.append(QVariantMap{{"id", u.localId},
                                   {"kind", QStringLiteral("upload")},
                                   {"name", u.name},
                                   {"typeLabel", label},
                                   {"tint", tint},
                                   {"shared", QStringLiteral("Only you")},
                                   {"modified", QStringLiteral("Now")},
                                   {"size", formatBytes(u.size)},
                                   {"status", status},
                                   {"progress", u.progress},
                                   {"error", u.error},
                                   {"incoming", false},
                                   {"trashed", false},
                                   {"lock", u.lock ? QString(u.lock->kind == data::FileLock::Kind::Key ? "key" : "password") : QString()},
                                   {"lockShort", u.lock ? qs(u.lock->fingerprint.substr(0, 4)) : QString()},
                                   {"unlocked", false}});
        }
    }
    for (const auto* e : picked) out.append(rowFor(*e));
    return out;
}

QVariantMap FilesSession::selected() const {
    for (const auto& u : uploads_) {
        if (u.localId != selectedId_) continue;
        const auto [label, tint] = typeOf(u.name, false);
        const int percent = qRound(u.progress * 100);
        const QString status = u.stage == "failed"      ? u.error
                               : u.stage == "paused"     ? u.error
                               : u.stage == "encrypting" ? QStringLiteral("Encrypting · %1%").arg(percent)
                               : u.stage == "uploading"  ? QStringLiteral("Uploading · %1%").arg(percent)
                                                         : QStringLiteral("Waiting to upload");
        return {{"id", u.localId}, {"kind", QStringLiteral("upload")}, {"name", u.name}, {"typeLabel", label}, {"tint", tint},
                {"sizeText", formatBytes(u.size)}, {"modifiedText", QStringLiteral("Now")}, {"versions", 1}, {"statusText", status},
                {"uploading", u.stage != "failed"}, {"failed", u.stage == "failed"}, {"sharedWith", QVariantList{}}, {"links", QVariantList{}}};
    }
    const auto* e = find(selectedId_);
    if (!e) return {};
    const auto [label, tint] = typeOf(qs(e->name), e->kind == "folder");
    QVariantList sharedWith;
    for (const auto& userId : e->sharedWith) {
        const QString handle = handleFor(userId);
        sharedWith.append(QVariantMap{{"userId", qs(userId)}, {"handle", handle}, {"initial", handle.left(2).toUpper()}, {"role", QStringLiteral("Can view")}});
    }
    QVariantList links;
    for (const auto& l : snapshot_.links) {
        if (l.backupId != e->id) continue;
        QString expires = QStringLiteral("Never expires");
        if (l.expiresAt) {
            const auto ms = util::parseIsoMillis(*l.expiresAt);
            expires = ms ? QStringLiteral("Expires %1").arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(*ms).date(), QStringLiteral("MMM d")))
                         : QStringLiteral("Expires");
        }
        links.append(QVariantMap{{"id", qs(l.id)},
                                 {"expires", expires},
                                 {"hasPassword", l.hasPassword},
                                 {"allowDownload", l.allowDownload},
                                 {"canCopy", !repository_->linkUrl(*e, l.id, webAppBase_.toStdString()).empty()}});
    }
    const auto preview = previews_.find(e->id);
    const bool locked = e->locked();
    const bool unlocked = locked && unlocked_.count(e->id) > 0;
    const std::string lockKeyId = data::filelock::lockKeyId(e->meta);
    const auto* lockKey = lockKeyId.empty() ? nullptr : findKey(lockKeyId);
    QString sharedText = QStringLiteral("only you");
    if (e->incoming) sharedText = QStringLiteral("from ") + handleFor(e->ownerId);
    else if (!e->sharedWith.empty()) sharedText = QStringLiteral("shared with ") + handleFor(e->sharedWith.front());
    return {
        {"locked", locked},
        {"unlocked", unlocked},
        {"lockKind", qs(data::filelock::lockKind(e->meta))},
        {"lockKeyId", qs(lockKeyId)},
        {"lockKeyName", lockKey ? qs(lockKey->name) : QStringLiteral("A key that isn't on this account")},
        {"lockKeyAvailable", lockKey != nullptr},
        {"lockKeyHasPassword", lockKey && lockKey->passwordProtected},
        {"lockFingerprint", qs(data::filelock::lockFingerprint(e->meta))},
        {"sharedText", sharedText},
        {"id", qs(e->id)},
        {"kind", qs(e->kind)},
        {"name", qs(e->name)},
        {"typeLabel", label},
        {"tint", tint},
        {"sizeText", e->kind == "folder" ? QString() : formatBytes(e->size)},
        {"modifiedText", whenText(e->modifiedMs)},
        {"versions", e->kind == "folder" || e->incoming ? 1 : versionsOf(*e)},
        {"statusText", QString()},
        {"uploading", false},
        {"failed", false},
        {"incoming", e->incoming},
        {"owner", e->incoming ? handleFor(e->ownerId) : QString()},
        {"trashed", e->trashed},
        {"isImage", isImage(*e)},
        {"previewUrl", preview == previews_.end() ? QString() : preview->second},
        {"sharedWith", sharedWith},
        {"links", links},
        {"canOpen", e->kind == "file" && file_safety::isSafeToOpen(qs(e->name))},
    };
}

QVariantMap FilesSession::usage() const {
    const auto& u = snapshot_.usage;
    const QString plan = u.tier == "premium" ? QStringLiteral("Premium") : u.tier == "pro" ? QStringLiteral("Pro") : QStringLiteral("Free");
    return {{"usedText", formatBytes(u.used)},
            {"limitText", formatBytes(u.limit)},
            {"remainingText", formatBytes(u.remaining)},
            {"fraction", u.limit > 0 ? std::clamp(static_cast<double>(u.used) / static_cast<double>(u.limit), 0.0, 1.0) : 0.0},
            {"full", u.limit > 0 && u.used >= u.limit},
            {"nearlyFull", u.limit > 0 && static_cast<double>(u.used) >= 0.9 * static_cast<double>(u.limit)},
            {"plan", plan},
            {"tier", qs(u.tier)}};
}

void FilesSession::select(const QString& id) {
    selectedId_ = id;
    emit selectedChanged();
    if (const auto* e = find(id); e && isImage(*e) && e->size <= kMaxPreviewBytes && !previews_.count(e->id) && !usable(*e).needsUnlock()) {
        loadPreview(*e);
    }
}

void FilesSession::loadPreview(const data::DriveEntry& entry) {
    auto repository = repository_;
    runInBackground(
        pool_, this, [repository, entry = usable(entry)] { return repository->downloadToCache(entry); },
        [this, id = entry.id](fs::path path) {
            previews_[id] = QUrl::fromLocalFile(QString::fromStdWString(path.wstring())).toString();
            if (selectedId_ == qs(id)) emit selectedChanged();
        },
        [](const Failure&) {});
}

// --- Uploads ---

void FilesSession::upload(const QList<QUrl>& files) {
    enqueueUploads(files, nullptr);
}

void FilesSession::enqueueUploads(const QList<QUrl>& files, std::shared_ptr<LockSpec> lock) {
    for (const auto& url : files) {
        const fs::path path = localPath(url);
        std::error_code error;
        if (!fs::is_regular_file(path, error)) continue;
        Upload u;
        u.localId = QStringLiteral("upload:") + qs(util::randomUuid());
        u.path = path;
        u.parent = section_ == "all" ? folderId_ : std::string();
        u.name = QFileInfo(url.toLocalFile()).fileName();
        u.size = static_cast<std::int64_t>(fs::file_size(path, error));
        u.stage = QStringLiteral("queued");
        u.lock = lock;
        uploads_.push_back(std::move(u));
    }
    emit changed();
    startNextUpload();
}

void FilesSession::uploadNewVersion(const QString& id, const QUrl& file) {
    const auto* e = find(id);
    if (!e || e->incoming || e->kind != "file") return;
    const fs::path path = localPath(file);
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return;
    Upload u;
    u.localId = QStringLiteral("upload:") + qs(util::randomUuid());
    u.path = path;
    u.parent = e->parent;
    u.replacing = *e;
    u.name = qs(e->name);
    u.size = static_cast<std::int64_t>(fs::file_size(path, error));
    u.stage = QStringLiteral("queued");
    uploads_.push_back(std::move(u));
    emit changed();
    startNextUpload();
}

void FilesSession::startNextUpload() {
    if (uploading_) return;
    auto next = std::find_if(uploads_.begin(), uploads_.end(), [](const Upload& u) { return u.stage == "queued"; });
    if (next == uploads_.end()) {
        if (pcBackupStatus_.startsWith(QStringLiteral("Syncing"))) {
            pcBackupStatus_ = QStringLiteral("Auto-synced");
            emit pcBackupChanged();
            emit changed();
        }
        return;
    }
    uploading_ = true;
    // A resumed upload is already encrypted: it only sends what the server is missing.
    next->stage = next->resumeObjectId.empty() ? QStringLiteral("encrypting") : QStringLiteral("uploading");
    emit changed();

    const QString localId = next->localId;
    const Upload job = *next;
    auto repository = repository_;
    // PC backup folders already made, by their path inside the chosen folder.
    std::vector<data::DriveEntry> knownFolders;
    if (!job.subPath.empty()) {
        for (const auto& e : snapshot_.entries) {
            if (e.kind == "folder" && !e.trashed && e.meta.contains("sourceDir")) knownFolders.push_back(e);
        }
    }
    QPointer<FilesSession> self(this);
    // Progress arrives on the upload thread: forward at most every whole percent.
    auto lastPercent = std::make_shared<std::atomic<int>>(-1);
    auto progress = [self, localId, lastPercent](data::DriveStage stage, std::uint64_t done, std::uint64_t total) {
        const int percent = total > 0 ? static_cast<int>(done * 100 / total) : 0;
        const int key = static_cast<int>(stage) * 1000 + percent;
        if (lastPercent->exchange(key) == key) return;
        QMetaObject::invokeMethod(
            qApp,
            [self, localId, stage, percent] {
                if (!self) return;
                for (auto& u : self->uploads_) {
                    if (u.localId != localId) continue;
                    u.stage = stage == data::DriveStage::Encrypting ? QStringLiteral("encrypting") : QStringLiteral("uploading");
                    u.progress = percent / 100.0;
                }
                emit self->changed();
                if (self->selectedId_ == localId) emit self->selectedChanged();
            },
            Qt::QueuedConnection);
    };
    runInBackground(
        uploadPool_, this,
        [repository, job, progress, knownFolders]() mutable {
            if (!job.resumeObjectId.empty()) {
                // Continues from the first part the server is missing.
                auto entry = repository->resumeUpload(job.resumeObjectId, progress, job.control);
                if (entry.locked()) entry.meta.erase("key");
                return std::make_pair(std::move(entry), std::vector<data::DriveEntry>{});
            }
            // Recreate the file's subfolders (Invoices, then Invoices/2024) under the PC backup folder.
            std::vector<data::DriveEntry> created;
            std::string parent = job.parent;
            std::string prefix;
            std::size_t start = 0;
            while (start < job.subPath.size()) {
                const std::size_t slash = job.subPath.find('/', start);
                const std::string segment = job.subPath.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                start = slash == std::string::npos ? job.subPath.size() : slash + 1;
                if (segment.empty()) continue;
                prefix += (prefix.empty() ? "" : "/") + segment;
                const auto found = std::find_if(knownFolders.begin(), knownFolders.end(), [&](const data::DriveEntry& f) {
                    return f.parent == parent && f.meta.value("sourceDir", std::string()) == prefix;
                });
                if (found != knownFolders.end()) {
                    parent = found->id;
                    continue;
                }
                auto folder = repository->createFolder(segment, parent);
                folder.meta["sourceDir"] = prefix;
                folder = repository->save(folder);
                knownFolders.push_back(folder);
                created.push_back(folder);
                parent = folder.id;
            }
            std::optional<data::FileLock> lock;
            if (job.lock) lock = job.lock->lock();
            auto entry = repository->upload(job.path, parent, job.replacing, progress, job.extraMeta, lock ? &*lock : nullptr, job.control);
            // Uploaded locked: it stays locked until it's unlocked on purpose.
            if (entry.locked()) entry.meta.erase("key");
            return std::make_pair(std::move(entry), std::move(created));
        },
        [this, localId](std::pair<data::DriveEntry, std::vector<data::DriveEntry>> result) {
            auto& [entry, folders] = result;
            uploading_ = false;
            uploads_.erase(std::remove_if(uploads_.begin(), uploads_.end(), [&](const Upload& u) { return u.localId == localId; }), uploads_.end());
            for (auto& folder : folders) snapshot_.entries.push_back(std::move(folder));
            snapshot_.entries.push_back(entry);
            if (selectedId_ == localId) selectedId_ = qs(entry.id);
            emit changed();
            emit selectedChanged();
            refresh();  // storage use
            startNextUpload();
        },
        [this, localId](const Failure& failure) {
            uploading_ = false;
            if (failure.status == 401) return emit sessionExpired();
            if (failure.code == QStringLiteral("cancelled")) {
                uploads_.erase(std::remove_if(uploads_.begin(), uploads_.end(), [&](const Upload& u) { return u.localId == localId; }),
                               uploads_.end());
                emit changed();
                startNextUpload();
                return;
            }
            if (failure.code == QStringLiteral("interrupted") && tor_ != nullptr) {
                // Over Tor: paused, not failed. It resumes from where it stopped once Tor is back.
                // (Without Tor nothing is resumable, so it stays "failed" as before.)
                std::set<std::string> known;
                for (const auto& u : uploads_) known.insert(u.resumeObjectId);
                std::string resumeId;
                for (const auto& pending : repository_->pendingUploads()) {
                    if (!known.count(pending.objectId)) resumeId = pending.objectId;
                }
                for (auto& u : uploads_) {
                    if (u.localId != localId) continue;
                    u.stage = QStringLiteral("paused");
                    u.error = QStringLiteral("Paused · resumes when Tor connects");
                    if (u.resumeObjectId.empty()) u.resumeObjectId = resumeId;
                }
                emit changed();
                emit selectedChanged();
                startNextUpload();
                return;
            }
            for (auto& u : uploads_) {
                if (u.localId == localId) {
                    u.stage = QStringLiteral("failed");
                    u.error = failure.message;
                }
            }
            if (failure.message.startsWith(QStringLiteral("Not enough storage"))) {
                // Nothing else in the queue fits either until space is freed: stop, say so once.
                for (auto& u : uploads_) {
                    if (u.stage == "queued") {
                        u.stage = QStringLiteral("failed");
                        u.error = QStringLiteral("Not uploaded: your storage is full.");
                    }
                }
                emit storageFull(failure.message);
            }
            setMessages(failure.message, {});
            emit changed();
            emit selectedChanged();
            startNextUpload();
        });
}

void FilesSession::cancelUpload(const QString& localId) {
    const auto it = std::find_if(uploads_.begin(), uploads_.end(), [&](const Upload& u) { return u.localId == localId; });
    if (it == uploads_.end()) return;
    if (it->stage == "encrypting" || it->stage == "uploading") {
        // The worker sees it at the next chunk, drops the upload and reports "cancelled".
        it->control->cancelled = true;
        return;
    }
    if (!it->resumeObjectId.empty()) {
        auto repository = repository_;
        const std::string objectId = it->resumeObjectId;
        runInBackground(pool_, this, [repository, objectId] { repository->abandonUpload(objectId); }, [] {}, [](const Failure&) {});
    }
    uploads_.erase(it);
    emit changed();
    emit selectedChanged();
}

void FilesSession::resumePaused() {
    if (tor_ != nullptr && !tor_->connected()) return;
    bool any = false;
    for (auto& u : uploads_) {
        if (u.stage != "paused") continue;
        u.stage = QStringLiteral("queued");
        u.error.clear();
        any = true;
    }
    if (!any) return;
    emit changed();
    startNextUpload();
}

// --- Changes ---

void FilesSession::saveEntry(const data::DriveEntry& entry, const QString& notice) {
    auto repository = repository_;
    runInBackground(
        pool_, this, [repository, entry] { return repository->save(entry); },
        [this, notice](data::DriveEntry saved) {
            if (auto* e = findMutable(qs(saved.id))) *e = saved;
            emit changed();
            emit selectedChanged();
            if (!notice.isEmpty()) setMessages({}, notice);
        },
        [this](const Failure& failure) { fail(failure.message); });
}

void FilesSession::newFolder(const QString& name) {
    if (section_ != "all") {
        section_ = QStringLiteral("all");
        folderId_.clear();
    }
    auto repository = repository_;
    const std::string folderName = name.trimmed().isEmpty() ? std::string("New folder") : name.trimmed().toStdString();
    const std::string parent = folderId_;
    runInBackground(
        pool_, this, [repository, folderName, parent] { return repository->createFolder(folderName, parent); },
        [this](data::DriveEntry folder) {
            snapshot_.entries.push_back(folder);
            emit changed();
        },
        [this](const Failure& failure) { fail(failure.message); });
}

void FilesSession::rename(const QString& id, const QString& name) {
    const auto* e = find(id);
    if (!e || e->incoming || name.trimmed().isEmpty()) return;
    // Every version keeps the same name.
    for (const auto& other : snapshot_.entries) {
        if (other.id == e->id || (e->kind == "file" && other.kind == "file" && other.fileId == e->fileId)) {
            auto copy = other;
            copy.name = name.trimmed().toStdString();
            saveEntry(copy, {});
        }
    }
}

void FilesSession::move(const QString& id, const QString& folderId) {
    const auto* e = find(id);
    if (!e || e->incoming) return;
    // Not into itself or one of its own subfolders.
    std::string cursor = folderId.toStdString();
    for (int guard = 0; !cursor.empty() && guard < 64; ++guard) {
        if (cursor == e->id) return;
        const auto* parent = find(qs(cursor));
        cursor = parent ? parent->parent : std::string();
    }
    for (const auto& other : snapshot_.entries) {
        if (other.id == e->id || (e->kind == "file" && other.kind == "file" && other.fileId == e->fileId)) {
            auto copy = other;
            copy.parent = folderId.toStdString();
            saveEntry(copy, QStringLiteral("Moved."));
        }
    }
}

void FilesSession::trash(const QString& id) {
    const auto* e = find(id);
    if (!e || e->incoming) return;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& other : snapshot_.entries) {
        if (other.id == e->id || (e->kind == "file" && other.kind == "file" && other.fileId == e->fileId)) {
            auto copy = other;
            copy.trashed = true;
            copy.trashedMs = now;
            saveEntry(copy, {});
        }
    }
    setMessages({}, QStringLiteral("Moved to Trash. It still counts toward your storage until you empty the Trash."));
}

void FilesSession::restore(const QString& id) {
    const auto* e = find(id);
    if (!e) return;
    for (const auto& other : snapshot_.entries) {
        if (other.id == e->id || (e->kind == "file" && other.kind == "file" && other.fileId == e->fileId)) {
            auto copy = other;
            copy.trashed = false;
            copy.trashedMs = 0;
            // Back to the top level if its folder is gone.
            if (!copy.parent.empty() && !find(qs(copy.parent))) copy.parent.clear();
            saveEntry(copy, QStringLiteral("Restored."));
        }
    }
}

void FilesSession::deleteForever(const QString& id) {
    const auto* e = find(id);
    if (!e || e->incoming) return;
    // A folder takes everything in it; a file takes all its versions.
    std::vector<data::DriveEntry> doomed;
    std::vector<std::string> folders = {e->id};
    if (e->kind == "file") {
        for (const auto& other : snapshot_.entries) {
            if (other.kind == "file" && other.fileId == e->fileId) doomed.push_back(other);
        }
    } else {
        doomed.push_back(*e);
        for (std::size_t i = 0; i < folders.size(); ++i) {
            for (const auto& child : snapshot_.entries) {
                if (child.parent != folders[i]) continue;
                doomed.push_back(child);
                if (child.kind == "folder") folders.push_back(child.id);
            }
        }
    }
    auto repository = repository_;
    runInBackground(
        pool_, this,
        [repository, doomed] {
            for (const auto& d : doomed) repository->remove(d);
        },
        [this, doomed] {
            std::set<std::string> ids;
            for (const auto& d : doomed) ids.insert(d.id);
            snapshot_.entries.erase(std::remove_if(snapshot_.entries.begin(), snapshot_.entries.end(),
                                                   [&](const data::DriveEntry& x) { return ids.count(x.id) > 0; }),
                                    snapshot_.entries.end());
            if (ids.count(selectedId_.toStdString())) selectedId_.clear();
            emit changed();
            emit selectedChanged();
            setMessages({}, QStringLiteral("Deleted for good."));
            refresh();
        },
        [this](const Failure& failure) {
            fail(failure.message);
            refresh();
        });
}

void FilesSession::emptyTrash() {
    for (const auto& e : snapshot_.entries) {
        if (e.trashed && (e.kind == "folder" || currentVersions().at(e.fileId) == &e)) deleteForever(qs(e.id));
    }
}

void FilesSession::download(const QString& id, const QUrl& destination) {
    const auto* e = find(id);
    if (!e || e->kind != "file") return;
    auto repository = repository_;
    const auto entry = usable(*e);
    const fs::path path = localPath(destination);
    setMessages({}, QStringLiteral("Downloading and decrypting %1…").arg(qs(entry.name)));
    runInBackground(
        pool_, this, [repository, entry, path] { repository->download(entry, path); },
        [this, path] {
            file_safety::markFromInternet(path);
            setMessages({}, QStringLiteral("Saved to %1").arg(QString::fromStdWString(path.wstring())));
        },
        [this](const Failure& failure) {
            // Over Tor what arrived is kept (encrypted): downloading again continues from there.
            if (failure.code == QStringLiteral("interrupted") && tor_ != nullptr) {
                return fail(QStringLiteral("Download paused: Tor isn't connected. Download it again once it is to continue from where it stopped."));
            }
            fail(failure.message);
        });
}

void FilesSession::openFile(const QString& id) {
    const auto* e = find(id);
    if (!e || e->kind != "file") return;
    if (!file_safety::isSafeToOpen(qs(e->name))) return setMessages(QStringLiteral("Only pictures, audio, video, text and PDF files open from here. Save this one instead."), {});
    auto repository = repository_;
    const auto entry = usable(*e);
    setMessages({}, QStringLiteral("Decrypting %1…").arg(qs(entry.name)));
    runInBackground(
        pool_, this, [repository, entry] { return repository->downloadToCache(entry); },
        [this, key = entry.id](fs::path path) {
            file_safety::markFromInternet(path);
            openedCopies_[key].push_back(path);
            setMessages({}, {});
            QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(path.wstring())));
        },
        [this](const Failure& failure) { fail(failure.message); });
}

void FilesSession::shareWith(const QString& id, const QString& userId) {
    const auto* e = find(id);
    if (!e || e->incoming || e->kind != "file") return;
    auto repository = repository_;
    const auto entry = usable(*e);
    const std::string recipient = userId.toStdString();
    runInBackground(
        pool_, this, [repository, entry, recipient] { repository->share(entry, recipient); },
        [this, id, recipient] {
            if (auto* x = findMutable(id); x && std::find(x->sharedWith.begin(), x->sharedWith.end(), recipient) == x->sharedWith.end()) {
                x->sharedWith.push_back(recipient);
            }
            emit changed();
            emit selectedChanged();
            setMessages({}, QStringLiteral("Shared with %1. Only they can open it.").arg(handleFor(recipient)));
        },
        [this](const Failure& failure) { fail(failure.message); });
}

void FilesSession::unshare(const QString& id, const QString& userId) {
    const auto* e = find(id);
    if (!e || e->incoming) return;
    auto repository = repository_;
    const auto entry = *e;
    const std::string recipient = userId.toStdString();
    runInBackground(
        pool_, this, [repository, entry, recipient] { repository->unshare(entry, recipient); },
        [this, id, recipient] {
            if (auto* x = findMutable(id)) x->sharedWith.erase(std::remove(x->sharedWith.begin(), x->sharedWith.end(), recipient), x->sharedWith.end());
            emit changed();
            emit selectedChanged();
        },
        [this](const Failure& failure) { fail(failure.message); });
}

void FilesSession::createLink(const QString& id, const QString& password) {
    const auto* e = find(id);
    if (!e || e->incoming || e->kind != "file") return;
    if (linkPassword_ && password.size() < 6) return setMessages(QStringLiteral("Choose a link password of at least 6 characters."), {});
    auto repository = repository_;
    auto entry = usable(*e);
    data::DriveRepository::LinkOptions options;
    options.password = linkPassword_ ? password.toStdString() : std::string();
    options.expiresInDays = linkExpires_ ? std::optional<int>(7) : std::nullopt;
    options.allowDownload = linkAllowDownload_;
    const std::string base = webAppBase_.toStdString();
    struct Result {
        std::string url;
        data::DriveEntry entry;
    };
    runInBackground(
        pool_, this,
        [repository, entry, options, base]() mutable {
            Result r;
            r.url = repository->createLink(entry, options, base);
            r.entry = entry;
            return r;
        },
        [this](Result r) {
            if (auto* x = findMutable(qs(r.entry.id))) *x = r.entry;
            QGuiApplication::clipboard()->setText(qs(r.url));
            emit linkCreated(qs(r.url));
            setMessages({}, QStringLiteral("Link created and copied. Send the password separately."));
            refresh();
        },
        [this](const Failure& failure) { fail(failure.message); });
}

void FilesSession::copyLink(const QString& id, const QString& linkId) {
    const auto* e = find(id);
    if (!e) return;
    const std::string url = repository_->linkUrl(*e, linkId.toStdString(), webAppBase_.toStdString());
    if (url.empty()) return setMessages(QStringLiteral("This link was made on another device and can't be copied here."), {});
    QGuiApplication::clipboard()->setText(qs(url));
    setMessages({}, QStringLiteral("Link copied."));
}

void FilesSession::deleteLink(const QString& id, const QString& linkId) {
    const auto* e = find(id);
    if (!e) return;
    auto repository = repository_;
    auto entry = *e;
    const std::string link = linkId.toStdString();
    runInBackground(
        pool_, this,
        [repository, entry, link]() mutable {
            repository->deleteLink(entry, link);
            return entry;
        },
        [this](data::DriveEntry updated) {
            if (auto* x = findMutable(qs(updated.id))) *x = updated;
            setMessages({}, QStringLiteral("Link removed. It no longer works."));
            refresh();
        },
        [this](const Failure& failure) { fail(failure.message); });
}

QVariantList FilesSession::moveTargets(const QString& id) const {
    QVariantList out{QVariantMap{{"id", QString()}, {"name", QStringLiteral("All files")}}};
    for (const auto& e : snapshot_.entries) {
        if (e.kind != "folder" || e.trashed || qs(e.id) == id) continue;
        out.append(QVariantMap{{"id", qs(e.id)}, {"name", qs(e.name)}});
    }
    return out;
}

void FilesSession::setLinkExpires(bool on) {
    linkExpires_ = on;
    emit linkSettingsChanged();
}

void FilesSession::setLinkPassword(bool on) {
    linkPassword_ = on;
    emit linkSettingsChanged();
}

void FilesSession::setLinkAllowDownload(bool on) {
    linkAllowDownload_ = on;
    emit linkSettingsChanged();
}

// --- PC backup ---

bool FilesSession::isInside(const data::DriveEntry& entry, const std::string& folderId) const {
    std::string cursor = entry.parent;
    for (int guard = 0; !cursor.empty() && guard < 64; ++guard) {
        if (cursor == folderId) return true;
        const auto* folder = find(qs(cursor));
        if (!folder) return false;
        cursor = folder->parent;
    }
    return false;
}

std::string FilesSession::pcBackupFolderId() const {
    for (const auto& e : snapshot_.entries) {
        if (e.kind == "folder" && !e.trashed && e.meta.value("pcBackup", false)) return e.id;
    }
    return {};
}

void FilesSession::setPcBackupFolder(const QUrl& folder) {
    pcBackupPath_ = folder.toLocalFile();
    prefs::setText(identity_->userId, "pcBackupPath", pcBackupPath_);
    pcBackupStatus_ = QStringLiteral("Auto-synced");
    pcBackupTimer_.start();
    emit pcBackupChanged();
    emit changed();
    runPcBackup();
}

void FilesSession::stopPcBackup() {
    pcBackupPath_.clear();
    prefs::setText(identity_->userId, "pcBackupPath", {});
    pcBackupTimer_.stop();
    pcBackupStatus_.clear();
    emit pcBackupChanged();
    emit changed();
}

void FilesSession::syncPcBackupNow() {
    runPcBackup();
}

void FilesSession::runPcBackup() {
    if (pcBackupPath_.isEmpty() || pcBackupRunning_ || !loaded_) return;
    const QDir root(pcBackupPath_);
    if (!root.exists()) {
        pcBackupStatus_ = QStringLiteral("Folder not found");
        emit pcBackupChanged();
        return;
    }
    const std::string folderId = pcBackupFolderId();
    if (folderId.empty()) {
        // First run: the "PC backup" folder in Files.
        pcBackupRunning_ = true;
        auto repository = repository_;
        const std::string name = "PC backup";
        runInBackground(
            pool_, this,
            [repository, name] {
                auto folder = repository->createFolder(name, "");
                folder.meta["pcBackup"] = true;
                return repository->save(folder);
            },
            [this](data::DriveEntry folder) {
                pcBackupRunning_ = false;
                snapshot_.entries.push_back(folder);
                emit changed();
                runPcBackup();
            },
            [this](const Failure& failure) {
                pcBackupRunning_ = false;
                fail(failure.message);
            });
        return;
    }

    // What's already backed up, by path relative to the chosen folder.
    std::map<QString, const data::DriveEntry*> backedUp;
    for (const auto& [fileId, e] : currentVersions()) {
        if (e->meta.contains("source") && isInside(*e, folderId)) backedUp[qs(e->meta.value("source", std::string()))] = e;
    }
    int queued = 0;
    int seen = 0;
    QDirIterator it(pcBackupPath_, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext() && seen < kMaxPcBackupFiles) {
        const QString path = it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isHidden() || info.fileName().startsWith(u'.') || info.fileName().startsWith(QStringLiteral("~$"))) continue;
        ++seen;
        const QString relative = root.relativeFilePath(path);
        const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
        const auto existing = backedUp.find(relative);
        const bool unchanged = existing != backedUp.end() && existing->second->meta.value("sourceSize", std::int64_t{-1}) == info.size() &&
                               existing->second->meta.value("sourceMtime", std::int64_t{-1}) == mtime;
        const bool inQueue = std::any_of(uploads_.begin(), uploads_.end(), [&](const Upload& u) {
            return u.extraMeta.value("source", std::string()) == relative.toStdString() && u.stage != "failed";
        });
        if (unchanged || inQueue) continue;
        Upload u;
        u.localId = QStringLiteral("upload:") + qs(util::randomUuid());
        u.path = fs::path(path.toStdWString());
        u.parent = folderId;
        if (existing != backedUp.end()) {
            // A new version goes where the file already is.
            u.replacing = *existing->second;
            u.parent = existing->second->parent;
        } else {
            const QString dir = QFileInfo(relative).path();
            if (dir != QStringLiteral(".")) u.subPath = dir.toStdString();
        }
        u.name = relative;
        u.size = info.size();
        u.stage = QStringLiteral("queued");
        u.extraMeta = Json{{"source", relative.toStdString()}, {"sourceSize", info.size()}, {"sourceMtime", mtime}};
        uploads_.push_back(std::move(u));
        ++queued;
    }
    pcBackupStatus_ = queued > 0 ? QStringLiteral("Syncing · %1 to upload").arg(queued) : QStringLiteral("Auto-synced");
    emit pcBackupChanged();
    emit changed();
    startNextUpload();
}

// --- Messages ---

void FilesSession::fail(const QString& message) {
    setMessages(message, {});
}

void FilesSession::setMessages(const QString& error, const QString& notice) {
    error_ = error;
    notice_ = notice;
    emit messagesChanged();
}

void FilesSession::clearMessages() {
    setMessages({}, {});
    // Failed uploads go once the message is dismissed.
    uploads_.erase(std::remove_if(uploads_.begin(), uploads_.end(), [](const Upload& u) { return u.stage == "failed"; }), uploads_.end());
    emit changed();
}

}  // namespace redvault::app
