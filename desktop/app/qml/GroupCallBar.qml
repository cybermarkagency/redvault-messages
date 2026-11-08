// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A group call tucked away ("Back to the app"): a bar across the top that says
// it's still on, with mute, return and leave.
Rectangle {
    id: bar
    required property GroupCallController groupCalls
    property double now: Date.now()

    visible: groupCalls.minimized && groupCalls.present && groupCalls.phase !== "incoming" && groupCalls.phase !== "ended"
    implicitHeight: visible ? 48 : 0
    color: Theme.accentSoft

    Timer {
        running: bar.visible
        interval: 1000
        repeat: true
        onTriggered: bar.now = Date.now()
    }

    function elapsed() {
        if (!groupCalls.connectedAt)
            return groupCalls.phase === "reconnecting" ? "Reconnecting…" : "Connecting…"
        const s = Math.max(0, Math.floor((now - groupCalls.connectedAt) / 1000))
        const mm = Math.floor(s / 60), ss = s % 60
        return mm + ":" + (ss < 10 ? "0" : "") + ss
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.accentBorder
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 20
        anchors.rightMargin: 12
        spacing: 12

        Rectangle {
            implicitWidth: 8
            implicitHeight: 8
            radius: 4
            color: Theme.accent
            SequentialAnimation on opacity {
                running: bar.visible
                loops: Animation.Infinite
                NumberAnimation { from: 1; to: 0.3; duration: 800 }
                NumberAnimation { from: 0.3; to: 1; duration: 800 }
            }
        }
        Text {
            Layout.fillWidth: true
            text: (bar.groupCalls.kind === "video" ? "Group video call in " : "Group call in ") + bar.groupCalls.groupLabel + " · " + bar.elapsed()
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.accent
            font.weight: Font.Medium
        }
        IconButton {
            iconName: bar.groupCalls.muted ? "mic-off" : "mic"
            toggled: bar.groupCalls.muted
            tip: bar.groupCalls.muted ? "Unmute" : "Mute"
            implicitHeight: 36
            implicitWidth: 36
            onClicked: bar.groupCalls.toggleMute()
        }
        RvButton {
            implicitHeight: 34
            text: "Return to call"
            onClicked: bar.groupCalls.setMinimized(false)
        }
        IconButton {
            danger: true
            round: true
            iconName: "phone-off"
            tip: "Leave the call"
            implicitHeight: 36
            implicitWidth: 36
            iconSize: 18
            onClicked: bar.groupCalls.leave()
        }
    }
}
