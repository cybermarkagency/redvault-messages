// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtMultimedia
import Redvault

// One picture on the group call screen: a person's camera (or their photo when
// it's off), or a screen being shared. It hands its VideoOutput to the call
// controller under its key, which is where that person's pictures are shown.
Item {
    id: tile
    required property GroupCallController groupCalls
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

    // The key this tile's VideoOutput is registered under (a tile can be given another person's key when tiles move).
    property string registeredKey: ""

    function sync() {
        if (registeredKey === key)
            return
        if (registeredKey.length > 0)
            groupCalls.clearTileSink(registeredKey, video.videoSink)
        registeredKey = key
        if (key.length > 0)
            groupCalls.setTileSink(key, video.videoSink)
    }
    onKeyChanged: sync()
    Component.onCompleted: sync()
    Component.onDestruction: {
        if (registeredKey.length > 0)
            groupCalls.clearTileSink(registeredKey, video.videoSink)
    }

    Rectangle {
        anchors.fill: parent
        radius: 14
        color: tile.screen ? Theme.surface : Theme.surface3
        clip: true
        border.width: tile.speaking ? 2 : 1
        border.color: tile.speaking ? Theme.accent : Theme.borderStrong

        VideoOutput {
            id: video
            anchors.fill: parent
            anchors.margins: 1
            visible: tile.videoOn
            fillMode: tile.screen ? VideoOutput.PreserveAspectFit : VideoOutput.PreserveAspectCrop
        }

        // Camera off (or not arrived yet): who it is.
        Avatar {
            anchors.centerIn: parent
            visible: !tile.videoOn
            size: Math.max(48, Math.min(120, Math.min(parent.width, parent.height) * 0.45))
            initial: tile.name.length > 0 ? tile.name.charAt(0).toUpperCase() : "?"
            source: tile.avatarUrl
            tintKey: tile.userId
        }

        // Name and microphone.
        Rectangle {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 10
            implicitWidth: nameRow.implicitWidth + 20
            implicitHeight: 28
            radius: 14
            color: Qt.alpha(Theme.bg, 0.78)
            RowLayout {
                id: nameRow
                anchors.centerIn: parent
                spacing: 6
                Icon {
                    visible: tile.screen
                    name: "monitor-up"
                    size: 14
                    color: Theme.accent
                }
                Icon {
                    visible: !tile.screen && tile.micMuted
                    name: "mic-off"
                    size: 14
                    color: Theme.danger
                }
                Text {
                    Layout.maximumWidth: Math.max(60, tile.width - 80)
                    text: tile.screen ? (tile.isSelf ? "Your screen" : tile.handle + "'s screen") : tile.handle
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.text
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }
            }
        }
    }
}
