// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <filesystem>

namespace redvault::app::file_preview {

/**
 * What the Files viewer shows inside the app, by file name: "image", "text",
 * "csv", "json", "audio", "video", "pdf", or empty (no in-app preview: the
 * file is only saved or opened elsewhere). The bytes are checked again once
 * decrypted (looksLikePdf, readText); pictures, sound and video go to Qt's
 * decoders, which read the content and never run anything in it.
 */
QString kindFor(const QString& filename);

/// Largest file of this kind the viewer decrypts (0: none).
std::int64_t limitFor(const QString& kind);

/// Text, CSV and JSON shown inline are capped here (the web viewer's limit).
constexpr std::int64_t kMaxTextBytes = 512 * 1024;
constexpr int kMaxCsvRows = 1000;
constexpr int kMaxCsvColumns = 50;

/// A decrypted text-like file, ready for the viewer.
struct TextPreview {
    bool ok = false;
    QString error;
    /// "text" and "json" (pretty-printed when it parses), and the raw text of a CSV.
    QString text;
    /// CSV only: rows of cells (QStringList each), at most kMaxCsvRows x kMaxCsvColumns.
    QVariantList rows;
    /// CSV only: rows or columns were left out.
    bool truncated = false;
};

/// Reads `file` as `kind` ("text", "csv" or "json"). Only valid UTF-8 without NUL bytes is shown.
TextPreview readText(const std::filesystem::path& file, const QString& kind);

/// Valid UTF-8 with no NUL bytes (a BOM is fine).
bool isPlainText(const QByteArray& bytes);

/// RFC 4180 CSV: quoted cells, doubled quotes, CRLF or LF rows.
QList<QStringList> parseCsv(const QString& text, int maxRows, int maxColumns, bool* truncated = nullptr);

/// The file starts with a PDF header ("%PDF-" in its first kilobyte, as readers allow).
bool looksLikePdf(const std::filesystem::path& file);

}  // namespace redvault::app::file_preview
