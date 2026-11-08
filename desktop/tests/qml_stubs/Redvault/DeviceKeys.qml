// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Stand-in for DeviceKeysSession.
import QtQml

QtObject {
    property bool chatActive: false
    property string pinnedChat: ""

    function chatPinned(chatId) {
        return pinnedChat.length > 0 && chatId === pinnedChat
    }
}
