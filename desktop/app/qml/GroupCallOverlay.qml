// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A group call, over the rest of the window: the ring (bottom right, doesn't
// block the app), the full-window call with everyone's pictures, safety words
// and controls, and the closing banner. Minimized, GroupCallBar shows instead.
Item {
    id: root
    required property GroupCallController groupCalls
    required property ChatSession chats
    signal openChat(string chatId)

    readonly property string phase: groupCalls.phase
    readonly property bool video: groupCalls.kind === "video"
    readonly property bool fullScreen: groupCalls.present && phase.length > 0 && phase !== "incoming" && !groupCalls.minimized
    property double now: Date.now()

    function duration() {
        if (!groupCalls.connectedAt)
            return ""
        const s = Math.max(0, Math.floor((now - groupCalls.connectedAt) / 1000))
        const hh = Math.floor(s / 3600), mm = Math.floor((s % 3600) / 60), ss = s % 60
        const two = (n) => (n < 10 ? "0" : "") + n
        return (hh > 0 ? hh + ":" + two(mm) : two(mm)) + ":" + two(ss)
    }
    function status() {
        if (phase === "ended") return groupCalls.endedMessage
        if (phase === "connecting") return "Connecting securely…"
        if (phase === "reconnecting") return "Reconnecting…"
        if (phase === "active" && !groupCalls.keyReady) return "Securing the call…"
        if (phase === "active") return duration()
        return ""
    }

    Timer {
        running: root.groupCalls.connectedAt > 0 && root.phase !== "ended"
        interval: 1000
        repeat: true
        onTriggered: root.now = Date.now()
    }

    // The pictures on the call screen. Rows are updated in place (not rebuilt), so a tile's video keeps playing.
    ListModel {
        id: tileModel
    }
    function syncTiles() {
        const tiles = groupCalls.tiles
        for (let i = tileModel.count - 1; i >= 0; --i) {
            const key = tileModel.get(i).key
            if (!tiles.some((t) => t.key === key))
                tileModel.remove(i)
        }
        for (let i = 0; i < tiles.length; ++i) {
            let at = -1
            for (let j = 0; j < tileModel.count; ++j) {
                if (tileModel.get(j).key === tiles[i].key) {
                    at = j
                    break
                }
            }
            if (at < 0) {
                tileModel.insert(i, tiles[i])
            } else {
                if (at !== i)
                    tileModel.move(at, i, 1)
                tileModel.set(i, tiles[i])
            }
        }
    }
    Connections {
        target: root.groupCalls
        function onViewChanged() { root.syncTiles() }
    }
    Component.onCompleted: syncTiles()

    // --- The ring ---
    Rectangle {
        visible: root.phase === "incoming"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 24
        width: 380
        height: ringColumn.implicitHeight + 40
        radius: Theme.radiusDialog
        color: Theme.surface
        border.color: Theme.borderStrong
        Accessible.role: Accessible.AlertMessage
        Accessible.name: "Incoming group call in " + root.groupCalls.groupLabel

        ColumnLayout {
            id: ringColumn
            anchors.fill: parent
            anchors.margins: 20
            spacing: 14

            RowLayout {
                spacing: 14
                Avatar {
                    size: 56
                    group: true
                    initial: root.groupCalls.groupLabel.length > 0 ? root.groupCalls.groupLabel.charAt(0).toUpperCase() : "G"
                    tintKey: root.groupCalls.conversationId
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: root.video ? "Group video call" : "Group voice call"
                        textFormat: Text.PlainText
                        color: Theme.text3
                        font.pixelSize: 12
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.groupCalls.groupLabel
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text
                        font.family: Theme.displayFamily
                        font.pixelSize: 22
                        font.weight: Font.Bold
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.groupCalls.hostHandle.length > 0
                        text: root.groupCalls.hostHandle + " started it"
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text2
                        font.pixelSize: 12
                    }
                }
            }
            RowLayout {
                spacing: 6
                Icon {
                    name: "shield-check"
                    size: 14
                    color: Theme.accent
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Sealed to each member's identity key · end-to-end encrypted"
                    color: Theme.accent
                    font.pixelSize: 12
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                RvButton {
                    Layout.fillWidth: true
                    danger: true
                    primary: true
                    iconName: "phone-off"
                    text: "Decline"
                    onClicked: root.groupCalls.decline()
                }
                RvButton {
                    Layout.fillWidth: true
                    iconName: root.video ? "video" : "phone"
                    text: "Join"
                    onClicked: root.groupCalls.accept()
                }
            }
            RvButton {
                Layout.alignment: Qt.AlignHCenter
                visible: root.groupCalls.conversationId.length > 0
                primary: false
                implicitHeight: 32
                text: "Open the chat"
                onClicked: {
                    const chat = root.groupCalls.conversationId
                    root.openChat(chat)
                }
            }
        }
    }

    // --- The call ---
    Rectangle {
        anchors.fill: parent
        visible: root.fullScreen
        color: Theme.callBg

        // Catch clicks so nothing underneath reacts.
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 12

            // Top bar: encryption, the group, the time.
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Rectangle {
                    implicitWidth: lockRow.implicitWidth + 24
                    implicitHeight: 34
                    radius: 17
                    color: Qt.alpha(Theme.accentSoft, 0.9)
                    border.color: Theme.accentBorder
                    RowLayout {
                        id: lockRow
                        anchors.centerIn: parent
                        spacing: 6
                        Icon {
                            name: "lock"
                            size: 14
                            color: Theme.accent
                        }
                        Text {
                            text: "End-to-end encrypted · group call"
                            color: Theme.accent
                            font.pixelSize: 12
                            font.weight: Font.Medium
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: root.groupCalls.groupLabel + (root.status().length > 0 ? " · " + root.status() : "")
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    Layout.maximumWidth: root.width * 0.5
                    color: root.phase === "ended" ? Theme.danger : Theme.text
                    font.family: root.phase === "active" ? Theme.monoFamily : Theme.fontFamily
                    font.pixelSize: 14
                }
            }

            // Everyone.
            GridView {
                id: grid
                Layout.fillWidth: true
                Layout.fillHeight: true
                interactive: false
                clip: true
                readonly property int cols: count <= 1 ? 1 : count <= 4 ? 2 : count <= 9 ? 3 : 4
                readonly property int rows: Math.max(1, Math.ceil(count / cols))
                cellWidth: width / cols
                cellHeight: height / rows
                model: tileModel
                delegate: Item {
                    id: cell
                    required property string key
                    required property string userId
                    required property string handle
                    required property string name
                    required property string avatarUrl
                    required property bool isSelf
                    required property bool screen
                    required property bool videoOn
                    required property bool micMuted
                    required property bool speaking
                    width: grid.cellWidth
                    height: grid.cellHeight
                    GroupCallTile {
                        anchors.fill: parent
                        anchors.margins: 5
                        groupCalls: root.groupCalls
                        key: cell.key
                        userId: cell.userId
                        handle: cell.handle
                        name: cell.name
                        avatarUrl: cell.avatarUrl
                        isSelf: cell.isSelf
                        screen: cell.screen
                        videoOn: cell.videoOn
                        micMuted: cell.micMuted
                        speaking: cell.speaking
                    }
                }
            }

            // Four words from the call and everyone's identity key: the same on every screen.
            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                visible: root.groupCalls.sas.length > 0 && root.phase !== "ended"
                implicitWidth: sasColumn.implicitWidth + 36
                implicitHeight: sasColumn.implicitHeight + 20
                radius: Theme.radiusCard
                color: Qt.alpha(Theme.surface, 0.95)
                border.color: Theme.accentBorder
                ColumnLayout {
                    id: sasColumn
                    anchors.centerIn: parent
                    spacing: 6
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: "Safety words: everyone in the call should see the same four."
                        color: Theme.text3
                        font.pixelSize: 12
                    }
                    Row {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 8
                        Repeater {
                            model: root.groupCalls.sas
                            delegate: Rectangle {
                                required property string modelData
                                width: word.implicitWidth + 20
                                height: 28
                                radius: 8
                                color: Theme.accentSoft
                                Text {
                                    id: word
                                    anchors.centerIn: parent
                                    text: parent.modelData
                                    textFormat: Text.PlainText
                                    color: Theme.accent
                                    font.family: Theme.monoFamily
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                }
                            }
                        }
                    }
                }
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                visible: root.phase !== "ended" && root.groupCalls.notice.length > 0
                text: root.groupCalls.notice
                textFormat: Text.PlainText
                color: Theme.warn
                font.pixelSize: 13
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                visible: root.phase !== "ended"
                spacing: 10
                DevicePicker {
                    iconName: "mic"
                    defaultLabel: "Default microphone"
                    devices: root.groupCalls.microphones
                    currentId: root.groupCalls.microphoneId
                    onPicked: (id) => root.groupCalls.setMicrophone(id)
                }
                DevicePicker {
                    iconName: "speaker"
                    defaultLabel: "Default speaker"
                    devices: root.groupCalls.speakers
                    currentId: root.groupCalls.speakerId
                    onPicked: (id) => root.groupCalls.setSpeaker(id)
                }
                DevicePicker {
                    visible: root.video
                    iconName: "video"
                    defaultLabel: "Default camera"
                    devices: root.groupCalls.cameras
                    currentId: root.groupCalls.cameraId
                    onPicked: (id) => root.groupCalls.setCamera(id)
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                visible: root.phase !== "ended"
                spacing: 14
                IconButton {
                    round: true; filled: true
                    implicitWidth: 56; implicitHeight: 56; iconSize: 22
                    iconName: root.groupCalls.muted ? "mic-off" : "mic"
                    toggled: root.groupCalls.muted
                    tip: root.groupCalls.muted ? "Unmute" : "Mute"
                    onClicked: root.groupCalls.toggleMute()
                }
                IconButton {
                    visible: root.video
                    round: true; filled: true
                    implicitWidth: 56; implicitHeight: 56; iconSize: 22
                    iconName: root.groupCalls.cameraOff ? "video-off" : "video"
                    toggled: root.groupCalls.cameraOff
                    tip: root.groupCalls.cameraOff ? "Turn camera on" : "Turn camera off"
                    onClicked: root.groupCalls.toggleCamera()
                }
                IconButton {
                    round: true; filled: true
                    implicitWidth: 56; implicitHeight: 56; iconSize: 22
                    iconName: "monitor-up"
                    toggled: root.groupCalls.sharingScreen
                    enabled: root.phase === "active" && root.groupCalls.keyReady
                    tip: root.groupCalls.sharingScreen ? "Stop sharing" : "Share your screen"
                    onClicked: {
                        const problem = root.groupCalls.toggleScreenShare()
                        if (problem.length > 0) shareNotice.text = problem
                    }
                }
                IconButton {
                    round: true; filled: true
                    implicitWidth: 56; implicitHeight: 56; iconSize: 22
                    iconName: "chevron-down"
                    tip: "Back to the app (the call continues)"
                    onClicked: {
                        if (root.groupCalls.conversationId.length > 0)
                            root.openChat(root.groupCalls.conversationId)
                        root.groupCalls.setMinimized(true)
                    }
                }
                IconButton {
                    danger: true
                    round: true
                    implicitWidth: 64; implicitHeight: 56; iconSize: 24
                    iconName: "phone-off"
                    tip: "Leave the call"
                    onClicked: root.groupCalls.leave()
                }
            }
            Text {
                id: shareNotice
                Layout.alignment: Qt.AlignHCenter
                visible: text.length > 0
                color: Theme.warn
                font.pixelSize: 13
                Timer {
                    running: shareNotice.text.length > 0
                    interval: 5000
                    onTriggered: shareNotice.text = ""
                }
            }

            RvButton {
                Layout.alignment: Qt.AlignHCenter
                visible: root.phase === "ended"
                primary: false
                text: "Close"
                onClicked: root.groupCalls.dismiss()
            }
        }
    }
}
