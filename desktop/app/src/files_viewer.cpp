// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The Files page's in-app viewer: pictures, text, CSV, JSON, audio, video and
// PDF are decrypted into this session's private cache and shown inside the
// app (FilePreviewDialog.qml). No other program sees the plaintext. Text-like
// files are read into memory and their decrypted copy deleted at once; the
// others are deleted when the viewer closes, and the whole cache at sign-out.

#include <QLocale>
#include <QMetaObject>
#include <QPointer>

#include <system_error>

#include "async.hpp"
#include "file_preview.hpp"
#include "files_session.hpp"

namespace redvault::app {

namespace fs = std::filesystem;

namespace {

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

QString localUrl(const fs::path& path) {
    return QUrl::fromLocalFile(QString::fromStdWString(path.wstring())).toString();
}

/// What a background load hands back to the UI thread.
struct ViewerLoad {
    fs::path path;
    file_preview::TextPreview text;
    QString error;
};

}  // namespace

bool FilesSession::pdfSupported() {
#ifdef REDVAULT_HAS_PDF
    return true;
#else
    return false;
#endif
}

QString FilesSession::previewKind(const QString& id) const {
    const auto* e = find(id);
    if (!e || e->kind != "file") return {};
    const QString kind = file_preview::kindFor(qs(e->name));
    if (kind == QLatin1String("pdf") && !pdfSupported()) return {};
    return kind;
}

void FilesSession::setViewer(QVariantMap viewer) {
    viewer_ = std::move(viewer);
    emit viewerChanged();
}

void FilesSession::openPreview(const QString& id) {
    closePreview();
    const auto* e = find(id);
    if (!e || e->kind != "file") return;
    const QString kind = previewKind(id);
    if (kind.isEmpty()) return;
    const QString name = qs(e->name);
    QVariantMap base{{"id", id}, {"name", name}, {"kind", kind}, {"progress", 0.0},
                     {"sizeText", QLocale().formattedDataSize(e->size, 1, QLocale::DataSizeTraditionalFormat)}};
    const auto error = [&](const QString& message) {
        base["state"] = QStringLiteral("error");
        base["message"] = message;
        setViewer(base);
    };
    const auto entry = usable(*e);
    if (entry.needsUnlock()) return error(QStringLiteral("This file is locked. Unlock it to preview it."));
    if (e->size > file_preview::limitFor(kind)) {
        return error(QStringLiteral("This file is too big to preview here (up to %1). Download it instead.")
                         .arg(QLocale().formattedDataSize(file_preview::limitFor(kind), 0, QLocale::DataSizeTraditionalFormat)));
    }
    // A picture already decrypted for the details panel shows at once.
    if (kind == QLatin1String("image")) {
        if (const auto known = previews_.find(e->id); known != previews_.end()) {
            base["state"] = QStringLiteral("ready");
            base["url"] = known->second;
            return setViewer(base);
        }
    }
    base["state"] = QStringLiteral("loading");
    setViewer(base);

    const int generation = ++viewerGeneration_;
    QPointer<FilesSession> self(this);
    auto repository = repository_;
    runInBackground(
        pool_, this,
        [repository, entry, kind, self, generation] {
            ViewerLoad load;
            load.path = repository->downloadToCache(entry, [self, generation](data::DriveStage, std::uint64_t done, std::uint64_t total) {
                if (total == 0) return;
                const double progress = static_cast<double>(done) / static_cast<double>(total);
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, generation, progress] {
                        if (!self || generation != self->viewerGeneration_ || self->viewer_.value("state") != QLatin1String("loading")) return;
                        self->viewer_["progress"] = progress;
                        emit self->viewerChanged();
                    },
                    Qt::QueuedConnection);
            });
            if (kind == QLatin1String("text") || kind == QLatin1String("csv") || kind == QLatin1String("json")) {
                load.text = file_preview::readText(load.path, kind);
                if (!load.text.ok) load.error = load.text.error;
                // Only the text in memory is kept: the decrypted copy goes now.
                std::error_code ignored;
                fs::remove(load.path, ignored);
                load.path.clear();
            } else if (kind == QLatin1String("pdf") && !file_preview::looksLikePdf(load.path)) {
                load.error = QStringLiteral("This doesn't look like a PDF, so it can't be shown here. Download it instead.");
            }
            return load;
        },
        [this, generation, kind, id = e->id](ViewerLoad load) {
            const auto drop = [this, &id](const fs::path& path) {
                if (path.empty()) return;
                std::error_code ec;
                fs::remove(path, ec);
                if (ec) openedCopies_[id].push_back(path);  // still in use: lockNow or sign-out removes it
            };
            if (generation != viewerGeneration_) return drop(load.path);  // closed or replaced meanwhile
            QVariantMap v = viewer_;
            if (!load.error.isEmpty()) {
                drop(load.path);
                v["state"] = QStringLiteral("error");
                v["message"] = load.error;
                return setViewer(v);
            }
            v["state"] = QStringLiteral("ready");
            v["progress"] = 1.0;
            if (!load.path.empty()) {
                v["url"] = localUrl(load.path);
                // A picture also becomes the details panel's preview (kept until sign-out or Lock now).
                if (kind == QLatin1String("image") && !previews_.count(id)) {
                    previews_[id] = v["url"].toString();
                    if (selectedId_ == qs(id)) emit selectedChanged();
                } else {
                    viewerCopy_ = load.path;
                }
            }
            v["text"] = load.text.text;
            v["rows"] = load.text.rows;
            v["truncated"] = load.text.truncated;
            setViewer(v);
        },
        [this, generation](const Failure& failure) {
            if (generation != viewerGeneration_) return;
            QVariantMap v = viewer_;
            v["state"] = QStringLiteral("error");
            v["message"] = failure.code == QStringLiteral("interrupted") && tor_ != nullptr
                               ? QStringLiteral("Tor isn't connected. Try again once it is.")
                               : failure.message;
            setViewer(v);
        });
}

void FilesSession::closePreview() {
    ++viewerGeneration_;
    if (!viewerCopy_.empty()) {
        std::error_code ec;
        fs::remove(viewerCopy_, ec);
        // A player can still hold it for a moment on Windows: lockNow or sign-out removes it then.
        if (ec) openedCopies_[viewer_.value("id").toString().toStdString()].push_back(viewerCopy_);
        viewerCopy_.clear();
    }
    if (!viewer_.isEmpty()) setViewer({});
}

}  // namespace redvault::app
