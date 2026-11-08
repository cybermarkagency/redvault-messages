// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QByteArray>
#include <QSettings>
#include <QString>

#include <string>

namespace redvault::app {

/**
 * Local-only preferences, like Android's LocalStores.kt. Nothing secret:
 * read receipts are a preference, and "I've verified this contact" is a
 * judgment about a public key, so it's keyed by the key itself (if a
 * contact's key changes, old trust isn't silently inherited) and never sent
 * to the server.
 */
namespace local_store {

/// Off by default, like the web and Android clients.
inline bool readReceipts() {
    return QSettings().value(QStringLiteral("readReceipts"), false).toBool();
}

inline void setReadReceipts(bool enabled) {
    QSettings().setValue(QStringLiteral("readReceipts"), enabled);
}

inline QString verifiedKey(const std::string& selfUserId, const std::string& contactPublicKeyB64) {
    // Hex rather than base64: QSettings treats '/' in a key as a group separator.
    const QByteArray key = QByteArray::fromBase64(QByteArray::fromStdString(contactPublicKeyB64)).toHex();
    return QStringLiteral("verifiedContacts/") + QString::fromStdString(selfUserId).toLower() + u'_' + QString::fromLatin1(key);
}

inline bool isVerified(const std::string& selfUserId, const std::string& contactPublicKeyB64) {
    return QSettings().value(verifiedKey(selfUserId, contactPublicKeyB64), false).toBool();
}

inline void setVerified(const std::string& selfUserId, const std::string& contactPublicKeyB64, bool verified) {
    QSettings settings;
    if (verified) {
        settings.setValue(verifiedKey(selfUserId, contactPublicKeyB64), true);
    } else {
        settings.remove(verifiedKey(selfUserId, contactPublicKeyB64));
    }
}

}  // namespace local_store

}  // namespace redvault::app
