// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Stand-in for the app singleton: only what the channel page reads.
pragma Singleton
import QtQml

QtObject {
    property ChatSession chats: ChatSession {}
    property QtObject groupCalls: null
    property QtObject groupInvites: null
    property QtObject communities: null
    property QtObject lock: QtObject {
        function expectExternalReturn() {}
    }
    property string copied: ""
    function copySecret(text) { copied = text }
}
