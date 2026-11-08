// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Stand-in for the app singleton: only what MiniChat.qml reads.
pragma Singleton
import QtQml

QtObject {
    property DeviceKeys deviceKeys: DeviceKeys {}
    property QtObject live: QtObject {
        function notifyTyping(chatId) {}
    }
}
