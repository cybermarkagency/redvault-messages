// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QByteArray>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <string>

#include "redvault/data/key_trust.hpp"
#include "redvault/data/vault_repository.hpp"

namespace redvault::app::prefs {

// Per-account preferences that never leave this computer, like the web
// app's lib/localPrefs.ts: favorites, muted contacts, the ringtone, the
// audio/video devices calls use, and the first key seen for each contact
// (to notice when it changes). Nothing secret.

inline QString key(const std::string& userId, const char* name) {
    return QStringLiteral("prefs/") + QString::fromStdString(userId) + u'/' + QLatin1String(name);
}

inline QStringList list(const std::string& userId, const char* name) {
    return QSettings().value(key(userId, name)).toStringList();
}

inline void setList(const std::string& userId, const char* name, const QStringList& values) {
    QSettings().setValue(key(userId, name), values);
}

inline bool toggleIn(const std::string& userId, const char* name, const QString& id) {
    QStringList values = list(userId, name);
    const bool present = values.removeAll(id) > 0;
    if (!present) values.append(id);
    setList(userId, name, values);
    return !present;
}

inline bool flag(const std::string& userId, const char* name, bool fallback) {
    return QSettings().value(key(userId, name), fallback).toBool();
}

inline void setFlag(const std::string& userId, const char* name, bool value) {
    QSettings().setValue(key(userId, name), value);
}

inline QString text(const std::string& userId, const char* name) {
    return QSettings().value(key(userId, name)).toString();
}

inline void setText(const std::string& userId, const char* name, const QString& value) {
    QSettings().setValue(key(userId, name), value);
}

/**
 * The first message-signing key seen for someone (trust on first use, like the
 * web's lib/messageSigning.ts): stored unless one is pinned already; returns the
 * pinned one (base64). A later, different key from the server is ignored, so
 * signatures made with it fail instead of a swapped key being trusted.
 */
inline std::string pinSigningKey(const std::string& selfUserId, const std::string& userId, const std::string& keyB64) {
    QSettings settings;
    // Lowercase, always: another spelling of an id must never earn a second pin.
    const QString name = key(QString::fromStdString(selfUserId).toLower().toStdString(), "signingKeys") + u'/' + QString::fromStdString(userId).toLower();
    const QString known = settings.value(name).toString();
    if (!known.isEmpty()) return known.toStdString();
    settings.setValue(name, QString::fromStdString(keyB64));
    return keyB64;
}

/// Conversation key pins (data/key_trust.hpp), as JSON. Local settings, per account: clearing them goes back to trust on first use.
inline std::shared_ptr<data::KeyPins> keyPinStore() {
    const auto name = [](const std::string& selfUserId, const std::string& conversationId) {
        return QStringLiteral("keyPins/") + QString::fromStdString(selfUserId) + u'/' + QString::fromStdString(conversationId);
    };
    return std::make_shared<data::StoredKeyPins>(
        [name](const std::string& selfUserId, const std::string& conversationId) {
            return QSettings().value(name(selfUserId, conversationId)).toString().toStdString();
        },
        [name](const std::string& selfUserId, const std::string& conversationId, const std::string& json) {
            QSettings().setValue(name(selfUserId, conversationId), QString::fromStdString(json));
        });
}

/// The signing key pinned for someone, if any, without pinning anything.
inline std::optional<std::string> pinnedSigningKey(const std::string& selfUserId, const std::string& userId) {
    const QString name = key(QString::fromStdString(selfUserId).toLower().toStdString(), "signingKeys") + u'/' + QString::fromStdString(userId).toLower();
    const QString known = QSettings().value(name).toString();
    if (known.isEmpty()) return std::nullopt;
    return known.toStdString();
}

/// "Delete for me": message ids hidden on this computer only (the newest 5000), like the web's lib/hiddenMessages.ts.
inline QStringList hiddenMessages(const std::string& userId) { return list(userId, "hiddenMessages"); }
inline void hideMessage(const std::string& userId, const QString& messageId) {
    QStringList ids = hiddenMessages(userId);
    if (ids.contains(messageId)) return;
    ids.append(messageId);
    while (ids.size() > 5000) ids.removeFirst();
    setList(userId, "hiddenMessages", ids);
}

// Favorites and muted contacts are lists of userIds.
inline QStringList favorites(const std::string& userId) { return list(userId, "favorites"); }
inline QStringList muted(const std::string& userId) { return list(userId, "muted"); }
inline bool ringtone(const std::string& userId) { return flag(userId, "ringtone", true); }
/// Off by default, like the web app: typing is only sent (and shown) while this is on.
inline bool typingIndicators(const std::string& userId) { return flag(userId, "typingIndicators", false); }
inline bool chatDetailsOpen(const std::string& userId) { return flag(userId, "chatDetailsOpen", true); }

/**
 * Trust on first use: remembers the first key seen for a contact, and says
 * whether the key now differs. A changed key is never accepted silently; it
 * stays flagged until the new safety number is verified (acceptContactKey).
 */
inline QString pinnedKey(const std::string& selfUserId, const std::string& contactUserId) {
    return QStringLiteral("pinnedKeys/") + QString::fromStdString(selfUserId).toLower() + u'_' + QString::fromStdString(contactUserId).toLower();
}

inline bool hasKeyChanged(const std::string& selfUserId, const std::string& contactUserId, const std::string& publicKeyB64) {
    QSettings settings;
    const QString k = pinnedKey(selfUserId, contactUserId);
    // Hex, since QSettings treats '/' in values fine but keeps keys readable either way.
    const QString current = QString::fromLatin1(QByteArray::fromBase64(QByteArray::fromStdString(publicKeyB64)).toHex());
    const QString known = settings.value(k).toString();
    if (known.isEmpty()) {
        settings.setValue(k, current);
        return false;
    }
    return known != current;
}

/// The same pins, for the core: checked before anything is sealed to someone (data/key_trust.hpp).
class SettingsContactKeyPins final : public data::ContactKeyPins {
public:
    bool keyChanged(const std::string& selfUserId, const std::string& contactUserId, const std::string& publicKeyB64) override {
        return !publicKeyB64.empty() && hasKeyChanged(selfUserId, contactUserId, publicKeyB64);
    }
};

inline std::shared_ptr<data::ContactKeyPins> contactKeyPinStore() {
    return std::make_shared<SettingsContactKeyPins>();
}

/// Vault item revisions: `name` is the item's id, or "shared:" and its id for a shared copy.
class SettingsVaultRevs final : public data::VaultRevs {
public:
    std::int64_t seen(const std::string& userId, const std::string& name) override { return QSettings().value(slot(userId, name), 0).toLongLong(); }
    void record(const std::string& userId, const std::string& name, std::int64_t rev) override {
        QSettings().setValue(slot(userId, name), static_cast<qlonglong>(rev));
    }

private:
    static QString slot(const std::string& userId, const std::string& name) {
        return QStringLiteral("vaultRevs/") + QString::fromStdString(userId).toLower() + u'/' + QString::fromStdString(name).toLower();
    }
};

inline std::shared_ptr<data::VaultRevs> vaultRevStore() {
    return std::make_shared<SettingsVaultRevs>();
}

inline void acceptContactKey(const std::string& selfUserId, const std::string& contactUserId, const std::string& publicKeyB64) {
    QSettings().setValue(pinnedKey(selfUserId, contactUserId),
                         QString::fromLatin1(QByteArray::fromBase64(QByteArray::fromStdString(publicKeyB64)).toHex()));
}

}  // namespace redvault::app::prefs
