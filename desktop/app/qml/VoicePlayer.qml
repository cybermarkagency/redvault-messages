// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtMultimedia
import Redvault

// A voice message (or other audio file) in a chat bubble: play/pause, a
// progress bar you can click to seek, and the time. The file is decrypted
// into the session cache on the first play (ChatSession::loadAudio).
RowLayout {
    id: player
    /// {state: ""|"loading"|"ready"|"failed", url}
    property var info: ({})
    property color ink: Theme.text
    property color inkSoft: Theme.text3
    signal loadRequested()

    readonly property string state: info && info.state ? info.state : ""
    readonly property bool playing: media.playbackState === MediaPlayer.PlayingState
    // Pressed Play before the file was ready: start as soon as it is.
    property bool wantsPlay: false

    spacing: 10
    implicitWidth: 260

    function clock(ms) {
        const s = Math.max(0, Math.floor(ms / 1000))
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }

    onStateChanged: {
        if (state === "ready" && wantsPlay) {
            wantsPlay = false
            media.source = info.url
            media.play()
        }
    }

    MediaPlayer {
        id: media
        audioOutput: AudioOutput {}
    }

    IconButton {
        implicitWidth: 36
        implicitHeight: 36
        round: true
        filled: true
        iconName: player.playing ? "pause" : "play"
        iconSize: 16
        tip: player.state === "failed" ? qsTr("Couldn't load this recording. Try again") : player.playing ? qsTr("Pause") : qsTr("Play")
        enabled: player.state !== "loading"
        onClicked: {
            if (player.playing) {
                media.pause()
            } else if (player.state === "ready") {
                if (media.source !== player.info.url) media.source = player.info.url
                media.play()
            } else {
                player.wantsPlay = true
                player.loadRequested()
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 4
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 4
            radius: 2
            color: Qt.alpha(player.ink, 0.2)
            Rectangle {
                width: media.duration > 0 ? parent.width * Math.min(1, media.position / media.duration) : 0
                height: parent.height
                radius: 2
                color: player.ink
            }
            TapHandler {
                enabled: media.duration > 0
                onTapped: (point) => media.position = Math.round(media.duration * point.position.x / parent.width)
            }
            HoverHandler { cursorShape: media.duration > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor }
        }
        Text {
            text: player.state === "loading" ? qsTr("Decrypting…")
                : player.state === "failed" ? qsTr("Couldn't load")
                : media.duration > 0 ? player.clock(player.playing || media.position > 0 ? media.position : media.duration)
                : qsTr("Voice message")
            textFormat: Text.PlainText
            color: player.inkSoft
            font.pixelSize: Theme.fontSmall
            font.family: Theme.monoFamily
        }
    }
}
