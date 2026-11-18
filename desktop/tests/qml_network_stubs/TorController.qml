// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick

// Stands in for the app's TorController (C++) in network_routing_page_test.cpp: the same properties, and a
// setBridges that records what the screen asked for and answers with `nextError` ("" = saved).
QtObject {
    property bool enabled: true
    property string state: "connected"
    property int progress: 100
    property string detail: ""
    property string onionHost: "4cydjhdc3lmhg7l6ykjont7dms2alunglmnlznlmjjwkyc67m5h325yd.onion"
    property int queued: 0
    property int pausedUploads: 0
    property bool backupRoute: false
    property bool backupRouteAvailable: false
    property bool onionOnly: false
    property string bridgeMode: "off"
    property string bridgeLines: ""
    property bool bridgesAvailable: true

    property string nextError: ""
    property var calls: []

    signal bridgesChanged()

    function setBridges(mode, lines) {
        calls = calls.concat([[mode, lines]])
        if (nextError.length > 0) return nextError
        bridgeMode = mode
        bridgeLines = mode === "custom" ? lines.trim() : ""
        state = "connecting"  // the real one restarts Tor
        bridgesChanged()
        return ""
    }
}
