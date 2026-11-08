// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The app-wide connection status (docs/tor-routing-plan.md): Tor connecting /
// connected / unavailable, messages queued, uploads paused, and while a call
// runs, that it goes over WebRTC through a TURN relay. Hidden when Tor routing
// is off and no call is on.
Rectangle {
    id: chip
    property TorController tor: null
    /// True while a one-to-one or group call is connected.
    property bool callActive: false
    signal openSettings()

    readonly property bool torOn: chip.tor !== null && chip.tor.enabled
    readonly property string torState: chip.torOn ? chip.tor.state : "off"
    readonly property color dot: chip.torState === "connected" ? Theme.accent
                                 : chip.torState === "connecting" ? Theme.warn
                                 : chip.torState === "unavailable" ? Theme.danger : Theme.text3

    visible: chip.torOn || chip.callActive
    implicitHeight: 28
    implicitWidth: row.implicitWidth + 20
    radius: 14
    color: chip.torState === "unavailable" ? Theme.dangerSoft : Theme.surface2
    border.color: Theme.border
    border.width: 1

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 6
        Rectangle {
            visible: chip.torOn
            width: 8
            height: 8
            radius: 4
            color: chip.dot
        }
        Text {
            visible: chip.torOn
            text: chip.torState === "connected" ? "Tor connected"
                  : chip.torState === "connecting" ? "Tor connecting " + chip.tor.progress + "%"
                  : "Tor unavailable"
            textFormat: Text.PlainText
            color: chip.torState === "unavailable" ? Theme.danger : Theme.text
            font.pixelSize: 12
            font.weight: Font.DemiBold
        }
        Text {
            visible: chip.torOn && chip.tor.queued > 0
            text: "· " + (chip.torOn ? chip.tor.queued : 0) + " queued"
            textFormat: Text.PlainText
            color: Theme.text2
            font.pixelSize: 12
        }
        Text {
            visible: chip.torOn && chip.tor.pausedUploads > 0
            text: "· upload paused"
            color: Theme.text2
            font.pixelSize: 12
        }
        Icon {
            visible: chip.callActive
            name: "phone"
            size: 13
            color: Theme.text2
        }
        Text {
            visible: chip.callActive
            text: "WebRTC call · TURN relay"
            color: Theme.text2
            font.pixelSize: 12
        }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: chip.openSettings() }
    ToolTip.visible: hover.hovered && chip.torOn && chip.tor.detail.length > 0
    ToolTip.text: chip.torOn ? chip.tor.detail : ""
}
