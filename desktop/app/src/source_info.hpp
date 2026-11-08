// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

// Settings > About, "Open-Source Licenses and Source Code": the source link and contact come from the build
// (REDVAULT_SOURCE_CODE_URL and REDVAULT_SOURCE_CONTACT in CMakeLists.txt, checked there too), and the licence
// texts are built in as :/legal/LICENSE.txt and :/legal/THIRD_PARTY_NOTICES.txt. No network request.
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace redvault::app::source_info {

/// The link to show for this version's source, or empty when none is configured or it isn't a plain
/// https:// URL (checked again here so a bad value can never become a link). "{version}" becomes the version.
inline QString sourceCodeLink(const QString& configured, const QString& version) {
    QString text = configured.trimmed();
    if (text.isEmpty()) return {};
    text.replace(QStringLiteral("{version}"), version);
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty() ||
        !url.userInfo().isEmpty())
        return {};
    return url.toString(QUrl::FullyEncoded);
}

/// Where to report a source-code problem: an email address or https:// URL from the build, else empty
/// (the app then points to the contact address in the Terms of Service).
inline QString sourceContact(const QString& configured) {
    const QString text = configured.trimmed();
    if (text.isEmpty()) return {};
    static const QRegularExpression email(QStringLiteral("^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]+$"));
    if (email.match(text).hasMatch()) return text;
    const QUrl url(text, QUrl::StrictMode);
    if (url.isValid() && url.scheme() == QStringLiteral("https") && !url.host().isEmpty() && url.userInfo().isEmpty())
        return url.toString(QUrl::FullyEncoded);
    return {};
}

/// The built-in resource for "license" or "notices"; empty for anything else.
inline QString resourcePath(const QString& name) {
    if (name == QStringLiteral("license")) return QStringLiteral(":/legal/LICENSE.txt");
    if (name == QStringLiteral("notices")) return QStringLiteral(":/legal/THIRD_PARTY_NOTICES.txt");
    return {};
}

/// Splits a long text into pieces of at most this many lines, so a list view only lays out what's on screen
/// (the third-party notices are about 100,000 lines). Joining them with "\n" gives the text back.
inline QStringList pages(const QString& text, int linesPerPage = 200) {
    QStringList out;
    if (text.isEmpty() || linesPerPage < 1) return out;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); i += linesPerPage)
        out.append(lines.mid(i, linesPerPage).join(QLatin1Char('\n')));
    return out;
}

/// The licence ("license") or third-party notices ("notices") as plain-text pages.
inline QStringList documentPages(const QString& name) {
    const QString path = resourcePath(name);
    if (path.isEmpty()) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QString text = QString::fromUtf8(file.readAll());
    text.remove(QLatin1Char('\r'));
    return pages(text);
}

}  // namespace redvault::app::source_info
