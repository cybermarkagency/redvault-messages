// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "file_preview.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringDecoder>

namespace redvault::app::file_preview {

QString kindFor(const QString& filename) {
    const QString ext = QFileInfo(filename).suffix().toLower();
    static const QStringList images = {"jpg", "jpeg", "png", "gif", "webp"};
    static const QStringList audio = {"mp3", "wav", "flac"};
    static const QStringList video = {"mp4", "mov"};
    if (images.contains(ext)) return QStringLiteral("image");
    if (ext == QLatin1String("txt")) return QStringLiteral("text");
    if (ext == QLatin1String("csv")) return QStringLiteral("csv");
    if (ext == QLatin1String("json")) return QStringLiteral("json");
    if (audio.contains(ext)) return QStringLiteral("audio");
    if (video.contains(ext)) return QStringLiteral("video");
    if (ext == QLatin1String("pdf")) return QStringLiteral("pdf");
    return {};
}

std::int64_t limitFor(const QString& kind) {
    constexpr std::int64_t MiB = 1024 * 1024;
    if (kind == QLatin1String("image")) return 100 * MiB;
    if (kind == QLatin1String("text") || kind == QLatin1String("csv") || kind == QLatin1String("json")) return kMaxTextBytes;
    if (kind == QLatin1String("audio")) return 1024 * MiB;
    if (kind == QLatin1String("video")) return 4096 * MiB;
    if (kind == QLatin1String("pdf")) return 500 * MiB;
    return 0;
}

bool isPlainText(const QByteArray& bytes) {
    if (bytes.contains('\0')) return false;
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder.decode(bytes);  // converting is what runs the decoder
    Q_UNUSED(text);
    return !decoder.hasError();
}

QList<QStringList> parseCsv(const QString& text, int maxRows, int maxColumns, bool* truncated) {
    QList<QStringList> rows;
    QStringList row;
    QString cell;
    bool quoted = false;
    bool cut = false;
    const auto endCell = [&] {
        if (row.size() < maxColumns) row.append(cell);
        else cut = true;
        cell.clear();
    };
    const auto endRow = [&] {
        endCell();
        if (rows.size() < maxRows) rows.append(row);
        else cut = true;
        row.clear();
    };
    const qsizetype n = text.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == u'"') {
                if (i + 1 < n && text.at(i + 1) == u'"') {
                    cell.append(u'"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cell.append(c);
            }
            continue;
        }
        if (c == u'"' && cell.isEmpty()) {
            quoted = true;
        } else if (c == u',') {
            endCell();
        } else if (c == u'\r' || c == u'\n') {
            if (c == u'\r' && i + 1 < n && text.at(i + 1) == u'\n') ++i;
            endRow();
            if (rows.size() >= maxRows && i + 1 < n) {
                cut = true;
                break;
            }
        } else {
            cell.append(c);
        }
    }
    // The last row, unless the file ended with a line break.
    if (!cell.isEmpty() || !row.isEmpty() || quoted) endRow();
    if (truncated) *truncated = cut;
    return rows;
}

TextPreview readText(const std::filesystem::path& file, const QString& kind) {
    TextPreview out;
    QFile f(QString::fromStdWString(file.wstring()));
    if (!f.open(QIODevice::ReadOnly)) {
        out.error = QStringLiteral("Couldn't read the decrypted file.");
        return out;
    }
    if (f.size() > kMaxTextBytes) {
        out.error = QStringLiteral("This file is too big to show here. Download it instead.");
        return out;
    }
    QByteArray bytes = f.readAll();
    if (!isPlainText(bytes)) {
        out.error = QStringLiteral("This file isn't plain text, so it can't be shown here. Download it instead.");
        return out;
    }
    if (bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);
    out.text = QString::fromUtf8(bytes);
    if (kind == QLatin1String("json")) {
        QJsonParseError error{};
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
        if (error.error == QJsonParseError::NoError) out.text = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    } else if (kind == QLatin1String("csv")) {
        bool cut = false;
        for (const auto& row : parseCsv(out.text, kMaxCsvRows, kMaxCsvColumns, &cut)) out.rows.append(QVariant(row));
        out.truncated = cut;
    }
    out.ok = true;
    return out;
}

bool looksLikePdf(const std::filesystem::path& file) {
    QFile f(QString::fromStdWString(file.wstring()));
    if (!f.open(QIODevice::ReadOnly)) return false;
    return f.read(1024).contains("%PDF-");
}

}  // namespace redvault::app::file_preview
