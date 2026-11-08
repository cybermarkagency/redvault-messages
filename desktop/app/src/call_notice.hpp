// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QString>

#include <optional>

namespace redvault::app {

struct CallNotice {
    QString title;
    QString text;
};

/// What the operating system's notification centre gets told about a ringing call, which it keeps in its history.
inline CallNotice callNotice(const QString& handle, const QString& kind, bool showName) {
    const QString title = kind == QStringLiteral("video") ? QStringLiteral("Video call") : QStringLiteral("Voice call");
    return {title, showName ? QStringLiteral("%1 is calling").arg(handle) : QStringLiteral("Someone is calling")};
}

/// Device-key messages in chats that aren't open: how many, and nothing else. Nothing at all when notifications are off.
inline std::optional<CallNotice> deviceKeyNotice(int count, bool notificationsOn) {
    if (!notificationsOn || count <= 0) return std::nullopt;
    return CallNotice{QStringLiteral("Device keys (preview)"), count == 1 ? QStringLiteral("1 new message") : QStringLiteral("%1 new messages").arg(count)};
}

}  // namespace redvault::app
