// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Stand-in for ChannelUploads: nothing being uploaded.
import QtQml

QtObject {
    property var items: []
    property int count: 0
    property int readyCount: 0
    property bool busy: false
    property bool full: false
    function addFiles(files, kind) {}
    function addRecording(file, kind, durationMs, mime) {}
    function remove(key) {}
    function clear() {}
}
