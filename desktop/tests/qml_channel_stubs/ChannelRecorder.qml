// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Stand-in for ChannelRecorder: never records.
import QtQml

QtObject {
    property string kind: "voice"
    property var videoOutput: null
    property bool recording: false
    property int seconds: 0
    property int maxSeconds: 60
    property string error: ""
    signal finished(url file, int durationMs, string mime)
    function start() {}
    function stop() {}
    function cancel() {}
}
