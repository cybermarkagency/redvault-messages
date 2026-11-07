#include "shares_session.hpp"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QLocale>
#include <QStandardPaths>

#include <map>
#include <set>

#include "async.hpp"
#include "file_safety.hpp"
#include "files_session.hpp"
#include "prefs.hpp"
#include "redvault/util/text.hpp"

namespace redvault::app {

namespace fs = std::filesystem;

namespace {

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

constexpr const char* kNotifiedKey = "shareIncomingNotified";
constexpr const char* kNotifiedReadyKey = "shareIncomingNotifiedReady";
constexpr qint64 kMb = 1024 * 1024;

QString bytesText(std::int64_t bytes) {
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString dateText(std::int64_t ms) {
    if (ms <= 0) return QStringLiteral("—");
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms), QLocale::ShortFormat);
}

std::optional<std::int64_t> positive(const QVariant& value, qint64 scale = 1) {
    bool ok = false;
    const double n = value.toDouble(&ok);
    if (!ok || n <= 0) return std::nullopt;
    return static_cast<std::int64_t>(n * static_cast<double>(scale) + 0.5);
}

api::Json optJson(const std::optional<std::int64_t>& v) {
    return v ? api::Json(*v) : api::Json(nullptr);
}

/// Folders, newest version of each file, nothing trashed: a fingerprint of what folder links show.
std::string signatureOf(const std::vector<data::DriveEntry>& entries) {
    std::string out;
    for (const auto& e : entries) {
        out += e.id + '|' + e.name + '|' + e.parent + '|' + (e.trashed ? "t" : "") + '|' + std::to_string(e.version) + '|' +
               std::to_string(e.modifiedMs) + '|' + (e.meta.contains("key") ? "k" : "") + '\n';
    }
    return out;
}

}  // namespace

SharesSession::SharesSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, FilesSession* files,
                             QString webAppBase, QObject* parent)
    : QObject(parent), identity_(std::move(identity)), files_(files) {
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    repository_ = std::make_shared<data::ShareRepository>(std::move(api), identity_, fs::path(cacheRoot.toStdWString()) / "shares",
                                                          webAppBase.toStdString());
    pool_.setMaxThreadCount(2);
    checkTimer_.setInterval(60 * 1000);
    connect(&checkTimer_, &QTimer::timeout, this, &SharesSession::refresh);
    checkTimer_.start();
    reconcileTimer_.setSingleShot(true);
    reconcileTimer_.setInterval(1500);
    connect(&reconcileTimer_, &QTimer::timeout, this, [this] {
        if (reconciling_ || !files_ || !files_->loaded()) return;
        std::vector<data::OwnerShare> live;
        for (const auto& l : links_) {
            if (l.status == "active" && l.box) live.push_back(l);
        }
        if (live.empty()) return;
        reconciling_ = true;
        auto repository = repository_;
        auto entries = files_->snapshot().entries;
        runInBackground(
            pool_, this, [repository, live, entries] { repository->reconcileAll(live, entries); },
            [this] { reconciling_ = false; },
            [this](const Failure& failure) {
                reconciling_ = false;
                if (failure.status == 401) emit sessionExpired();
            });
    });
    if (files_) connect(files_, &FilesSession::snapshotApplied, this, &SharesSession::reconcileSoon);
    refresh();
}

SharesSession::~SharesSession() {
    checkTimer_.stop();
    reconcileTimer_.stop();
    pool_.waitForDone(5000);
}

void SharesSession::reconcileSoon() {
    if (!files_) return;
    // Only when Files or the links changed since the last pass: each pass reads every live link's items.
    std::string signature = signatureOf(files_->snapshot().entries);
    for (const auto& l : links_) signature += l.link.id + ':' + std::to_string(l.link.keyEpoch) + ':' + l.status + ';';
    if (signature == lastSignature_) return;
    lastSignature_ = std::move(signature);
    reconcileTimer_.start();
}

void SharesSession::refresh() {
    auto repository = repository_;
    const int generation = ++generation_;
    runInBackground(
        pool_, this, [repository] { return repository->refresh(); },
        [this, generation](data::ShareRepository::Listing listing) {
            if (generation == generation_) apply(std::move(listing));
        },
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
        });
}

void SharesSession::apply(data::ShareRepository::Listing listing) {
    links_ = std::move(listing.links);
    incoming_ = std::move(listing.incoming);
    loaded_ = true;

    // What this computer already announced; the first check only remembers.
    const std::string& userId = identity_->userId;
    QStringList notified = prefs::list(userId, kNotifiedKey);
    const bool ready = prefs::flag(userId, kNotifiedReadyKey, false);
    std::map<std::string, int> fresh;  // link id ("" once deleted) -> files
    for (const auto& i : incoming_) {
        const QString id = qs(i.dto.id);
        if (notified.contains(id)) continue;
        notified.append(id);
        fresh[i.dto.shareId.value_or("")] += 1;
    }
    while (notified.size() > 2000) notified.removeFirst();
    prefs::setList(userId, kNotifiedKey, notified);
    if (!ready) prefs::setFlag(userId, kNotifiedReadyKey, true);
    if (ready && !incomingVisible_) {
        for (const auto& [shareId, count] : fresh) {
            QString label;
            for (const auto& l : links_) {
                if (l.link.id == shareId && l.box) label = qs(l.box->label);
            }
            const QString files = count == 1 ? QStringLiteral("1 encrypted file was") : QStringLiteral("%1 encrypted files were").arg(count);
            emit incomingArrived(label.isEmpty() ? files + QStringLiteral(" added through one of your sharing links.")
                                                 : files + QStringLiteral(" added through your “%1” sharing link.").arg(label));
        }
    }
    emit changed();
    reconcileSoon();
}

QVariantList SharesSession::links() const {
    std::map<std::string, const data::DriveEntry*> byId;
    if (files_) {
        for (const auto& e : files_->snapshot().entries) byId[e.id] = &e;
    }
    std::vector<const data::OwnerShare*> sorted;
    for (const auto& l : links_) sorted.push_back(&l);
    const auto rank = [](const std::string& s) { return s == "active" ? 0 : s == "expired" ? 1 : 2; };
    std::stable_sort(sorted.begin(), sorted.end(), [&](const auto* a, const auto* b) {
        if (rank(a->status) != rank(b->status)) return rank(a->status) < rank(b->status);
        return (a->box ? a->box->created : 0) > (b->box ? b->box->created : 0);
    });
    QVariantList out;
    for (const auto* s : sorted) {
        const auto& l = s->link;
        const auto item = byId.find(l.backupId);
        const QString itemName = item != byId.end() ? qs(item->second->name) : QString();
        const bool limitReached = l.maxDownloads && l.downloadCount >= *l.maxDownloads;
        const QString statusText = s->status == "revoked"   ? QStringLiteral("Revoked")
                                   : s->status == "expired" ? QStringLiteral("Expired")
                                   : limitReached           ? QStringLiteral("Download limit reached")
                                                            : QStringLiteral("Active");
        const auto expiresMs = l.expiresAt ? util::parseIsoMillis(*l.expiresAt) : std::nullopt;
        out.append(QVariantMap{
            {"id", qs(l.id)},
            {"itemId", qs(l.backupId)},
            {"itemName", itemName},
            {"itemMissing", item == byId.end()},
            {"kind", qs(l.kind)},
            {"permission", qs(l.permission)},
            {"status", qs(s->status)},
            {"statusText", statusText},
            {"active", s->status == "active"},
            {"label", s->box ? qs(s->box->label) : itemName},
            {"url", qs(s->url)},
            {"canManage", s->box.has_value()},
            {"hasPassword", l.password.has_value()},
            {"created", s->box ? dateText(s->box->created) : QStringLiteral("—")},
            {"expires", expiresMs ? dateText(*expiresMs) : QStringLiteral("Never")},
            {"expiresAtMs", expiresMs ? static_cast<double>(*expiresMs) : 0.0},
            {"downloads", l.maxDownloads ? QStringLiteral("%1 of %2").arg(l.downloadCount).arg(*l.maxDownloads) : QString::number(l.downloadCount)},
            {"maxDownloads", l.maxDownloads ? static_cast<double>(*l.maxDownloads) : 0.0},
            {"opens", static_cast<double>(l.openCount)},
            {"uploads", QStringLiteral("%1 (%2)").arg(l.upload.count).arg(bytesText(l.upload.bytes))},
            {"uploadsInProgress", static_cast<double>(l.upload.inProgress)},
            {"toReview", l.incoming},
            {"maxFileSizeMb", l.upload.maxFileSize ? static_cast<double>(*l.upload.maxFileSize) / kMb : 0.0},
            {"allowanceMb", l.upload.allowance ? static_cast<double>(*l.upload.allowance) / kMb : 0.0},
            {"maxFiles", l.upload.maxFiles ? static_cast<double>(*l.upload.maxFiles) : 0.0},
        });
    }
    return out;
}

QVariantList SharesSession::linksFor(const QString& itemId) const {
    QVariantList out;
    for (const auto& v : links()) {
        if (v.toMap().value("itemId").toString() == itemId) out.append(v);
    }
    return out;
}

int SharesSession::activeLinks() const {
    int n = 0;
    for (const auto& l : links_) n += l.status == "active" ? 1 : 0;
    return n;
}

QVariantList SharesSession::incoming() const {
    QVariantList out;
    for (const auto& i : incoming_) {
        QString via = QStringLiteral("a link you deleted");
        for (const auto& l : links_) {
            if (i.dto.shareId && l.link.id == *i.dto.shareId) via = l.box ? qs(l.box->label) : QStringLiteral("a share link");
        }
        const auto size = i.meta ? i.meta->at("size").get<std::int64_t>() : i.dto.encryptedSize;
        const auto sent = i.meta ? i.meta->at("sent").get<std::int64_t>() : 0;
        out.append(QVariantMap{
            {"id", qs(i.dto.id)},
            {"name", i.meta ? qs(i.name) : QStringLiteral("Can't be opened on this computer")},
            {"folder", qs(i.folder)},
            {"size", bytesText(size)},
            {"sent", dateText(sent)},
            {"via", via},
            {"openable", i.meta.has_value()},
            {"risky", i.meta.has_value() && isRisky(qs(i.name))},
        });
    }
    return out;
}

QVariantList SharesSession::folderChoices() const {
    QVariantList out{QVariantMap{{"id", QString()}, {"name", QStringLiteral("All files (top level)")}}};
    if (!files_) return out;
    const auto& entries = files_->snapshot().entries;
    std::map<std::string, const data::DriveEntry*> byId;
    for (const auto& e : entries) byId[e.id] = &e;
    std::vector<std::pair<QString, QString>> folders;
    for (const auto& e : entries) {
        if (e.kind != "folder" || e.trashed) continue;
        QStringList parts{qs(e.name)};
        auto it = byId.find(e.parent);
        for (int depth = 0; it != byId.end() && depth < 64; ++depth) {
            parts.prepend(qs(it->second->name));
            it = byId.find(it->second->parent);
        }
        folders.emplace_back(parts.join(QStringLiteral(" / ")), qs(e.id));
    }
    std::sort(folders.begin(), folders.end());
    for (const auto& [path, id] : folders) out.append(QVariantMap{{"id", id}, {"name", path}});
    return out;
}

const data::OwnerShare* SharesSession::findLink(const QString& id) const {
    for (const auto& l : links_) {
        if (qs(l.link.id) == id) return &l;
    }
    return nullptr;
}

const data::IncomingFile* SharesSession::findIncoming(const QString& id) const {
    for (const auto& i : incoming_) {
        if (qs(i.dto.id) == id) return &i;
    }
    return nullptr;
}

void SharesSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void SharesSession::setMessages(const QString& error, const QString& notice) {
    error_ = error;
    notice_ = notice;
    emit messagesChanged();
}

void SharesSession::clearMessages() {
    setMessages({}, {});
}

void SharesSession::setIncomingVisible(bool visible) {
    incomingVisible_ = visible;
}

template <typename Work, typename Done>
void SharesSession::run(Work work, Done done, const QString& doneNotice) {
    setBusy(true);
    setMessages({}, {});
    runInBackground(
        pool_, this, std::move(work),
        [this, done = std::move(done), doneNotice](auto result) mutable {
            setBusy(false);
            done(std::move(result));
            if (!doneNotice.isEmpty()) setMessages({}, doneNotice);
            refresh();
        },
        [this](const Failure& failure) {
            // The message first: dialogs close when busy ends without one.
            if (failure.status != 401) setMessages(failure.message, {});
            setBusy(false);
            if (failure.status == 401) emit sessionExpired();
        });
}

void SharesSession::createLink(const QString& itemId, const QVariantMap& options) {
    if (!files_) return;
    const auto& entries = files_->snapshot().entries;
    const data::DriveEntry* root = nullptr;
    for (const auto& e : entries) {
        if (qs(e.id) == itemId) root = &e;
    }
    if (!root) return setMessages(QStringLiteral("That item isn't in your files anymore."), {});
    data::ShareOptions o;
    o.permission = options.value("permission").toString() == QStringLiteral("contributor") ? "contributor" : "viewer";
    o.label = options.value("label").toString().trimmed().toStdString();
    o.password = options.value("password").toString().normalized(QString::NormalizationForm_C).toStdString();
    if (!o.password.empty() && options.value("password").toString().size() < 8) {
        return setMessages(QStringLiteral("Use a password of at least 8 characters."), {});
    }
    o.expiresAtMs = positive(options.value("expiresAtMs"));
    if (o.expiresAtMs && *o.expiresAtMs <= QDateTime::currentMSecsSinceEpoch()) {
        return setMessages(QStringLiteral("Pick an expiry in the future."), {});
    }
    o.maxDownloads = positive(options.value("maxDownloads"));
    o.maxFileSize = positive(options.value("maxFileSizeMb"), kMb);
    o.allowance = positive(options.value("allowanceMb"), kMb);
    o.maxFiles = positive(options.value("maxFiles"));
    auto repository = repository_;
    auto rootCopy = *root;
    auto all = entries;
    run([repository, rootCopy, all, o] { return repository->create(rootCopy, all, o); },
        [this](data::OwnerShare made) { emit linkReady(qs(made.url), qs(made.link.permission), made.link.password.has_value()); });
}

void SharesSession::copyLink(const QString& linkId) {
    const auto* l = findLink(linkId);
    if (!l || l->url.empty()) return setMessages(QStringLiteral("This link can't be copied on this computer."), {});
    QGuiApplication::clipboard()->setText(qs(l->url));
    setMessages({}, QStringLiteral("Link copied."));
}

void SharesSession::saveSettings(const QString& linkId, const QVariantMap& options) {
    const auto* l = findLink(linkId);
    if (!l) return;
    api::Json body = api::Json::object();
    const QString permission = options.value("permission").toString();
    if (permission == QStringLiteral("viewer") || permission == QStringLiteral("contributor")) body["permission"] = permission.toStdString();
    if (options.contains("expiresAtMs")) {
        const double ms = options.value("expiresAtMs").toDouble();
        if (ms == 0) {
            body["expiresAt"] = nullptr;
        } else if (ms > 0) {
            if (ms <= QDateTime::currentMSecsSinceEpoch()) return setMessages(QStringLiteral("Pick an expiry in the future."), {});
            body["expiresAt"] = util::formatIsoMillis(static_cast<std::int64_t>(ms));
        }
    }
    if (options.contains("maxDownloads")) body["maxDownloads"] = optJson(positive(options.value("maxDownloads")));
    if (permission == QStringLiteral("contributor")) {
        body["upload"] = api::Json{{"maxFileSize", optJson(positive(options.value("maxFileSizeMb"), kMb))},
                                   {"allowance", optJson(positive(options.value("allowanceMb"), kMb))},
                                   {"maxFiles", optJson(positive(options.value("maxFiles")))}};
    }
    std::optional<std::string> label;
    if (options.contains("label")) label = options.value("label").toString().trimmed().toStdString();
    auto repository = repository_;
    auto share = *l;
    run([repository, share, body, label] { return repository->updateSettings(share, body, label); }, [](data::OwnerShare) {},
        QStringLiteral("Saved."));
}

void SharesSession::turnOffUploads(const QString& linkId) {
    saveSettings(linkId, QVariantMap{{"permission", QStringLiteral("viewer")}});
}

void SharesSession::setPassword(const QString& linkId, const QString& password) {
    const auto* l = findLink(linkId);
    if (!l) return;
    if (!password.isEmpty() && password.size() < 8) return setMessages(QStringLiteral("Use a password of at least 8 characters."), {});
    auto repository = repository_;
    auto share = *l;
    const std::string normalized = password.normalized(QString::NormalizationForm_C).toStdString();
    run([repository, share, normalized] { return repository->setPassword(share, normalized); }, [](data::OwnerShare) {},
        password.isEmpty() ? QStringLiteral("Password removed.") : QStringLiteral("Password changed."));
}

void SharesSession::regenerate(const QString& linkId, const QString& password) {
    const auto* l = findLink(linkId);
    if (!l || !files_) return;
    if (!password.isEmpty() && password.size() < 8) return setMessages(QStringLiteral("Use a password of at least 8 characters."), {});
    auto repository = repository_;
    auto share = *l;
    auto entries = files_->snapshot().entries;
    const std::string normalized = password.normalized(QString::NormalizationForm_C).toStdString();
    run([repository, share, normalized, entries] { return repository->regenerate(share, normalized, entries); },
        [this](data::OwnerShare made) { emit linkReady(qs(made.url), qs(made.link.permission), made.link.password.has_value()); });
}

void SharesSession::revoke(const QString& linkId) {
    const auto* l = findLink(linkId);
    if (!l) return;
    auto repository = repository_;
    auto share = *l;
    run([repository, share] { return repository->revoke(share); }, [](data::OwnerShare) {},
        QStringLiteral("Link revoked. It stopped working for everyone."));
}

void SharesSession::remove(const QString& linkId) {
    const auto* l = findLink(linkId);
    if (!l) return;
    auto repository = repository_;
    auto share = *l;
    run(
        [repository, share] {
            repository->remove(share);
            return true;
        },
        [](bool) {}, QStringLiteral("Link deleted."));
}

void SharesSession::downloadIncoming(const QString& id, const QUrl& destination) {
    const auto* i = findIncoming(id);
    if (!i || !destination.isLocalFile()) return;
    auto repository = repository_;
    auto file = *i;
    const fs::path target(destination.toLocalFile().toStdWString());
    run(
        [repository, file, target] {
            repository->downloadIncoming(file, target);
            file_safety::markFromInternet(target);
            return true;
        },
        [](bool) {}, QStringLiteral("Saved. It was uploaded by someone anonymous: only open it if you expected it."));
}

void SharesSession::acceptIncoming(const QString& id, const QString& folderId) {
    const auto* i = findIncoming(id);
    if (!i) return;
    auto repository = repository_;
    auto file = *i;
    const std::string parent = folderId.toStdString();
    const QString name = qs(i->name);
    run([repository, file, parent] { return repository->acceptIncoming(file, parent); },
        [this](data::DriveEntry) {
            if (files_) files_->refresh();
        },
        QStringLiteral("“%1” was added to your files.").arg(name));
}

void SharesSession::rejectIncoming(const QString& id) {
    const auto* i = findIncoming(id);
    if (!i) return;
    auto repository = repository_;
    auto file = *i;
    run(
        [repository, file] {
            repository->rejectIncoming(file);
            return true;
        },
        [this](bool) {
            if (files_) files_->refresh();
        },
        QStringLiteral("Rejected. It's deleted, and its space is free again."));
}

// The same allow-list as opening from Files: anything that isn't a picture, audio, video, text or PDF gets the warning.
bool SharesSession::isRisky(const QString& name) const {
    return !file_safety::isSafeToOpen(name);
}

}  // namespace redvault::app
