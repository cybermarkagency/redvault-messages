// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtMultimedia
import Redvault

// An exclusive post: its public teaser and price for everyone; its text and
// photo or video (decrypted here) for its author and for buyers once the
// creator's app has sealed them the key. As the web app's PostCard.tsx.
Card {
    id: card
    required property EarningsSession earnings
    required property var post
    property string mediaUrl: ""
    property bool mediaVideo: false
    property bool mediaFailed: false
    property bool mediaLoading: false
    readonly property bool payingHere: earnings !== null && earnings.payContext === "exclusive_post" && earnings.payTarget === post.id
    signal changed()

    Layout.fillWidth: true
    spacing: 10
    Accessible.name: "Exclusive post"

    function loadMedia() {
        if (!post.opened || !post.hasMedia || mediaUrl.length > 0 || mediaLoading) return
        mediaLoading = true
        earnings.loadPostMedia(post.id)
    }
    Component.onCompleted: loadMedia()
    onPostChanged: {
        mediaUrl = ""
        mediaFailed = false
        mediaLoading = false
        loadMedia()
    }

    Connections {
        target: card.earnings
        function onPostMediaReady(postId, url, video) {
            if (postId !== card.post.id) return
            card.mediaLoading = false
            card.mediaVideo = video
            card.mediaUrl = url
        }
        function onPostMediaFailed(postId) {
            if (postId !== card.post.id) return
            card.mediaLoading = false
            card.mediaFailed = true
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Text {
            text: card.post.price
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.fontTitle
            font.weight: Font.Bold
        }
        Chip {
            visible: card.post.mine
            text: card.post.statusLabel
            checked: card.post.status === "published"
            enabled: false
        }
        Chip {
            visible: card.post.owned
            text: "Bought"
            checked: true
            enabled: false
        }
        Item { Layout.fillWidth: true }
        Text {
            text: card.post.when
            textFormat: Text.PlainText
            color: Theme.text3
            font.pixelSize: Theme.fontSmall
        }
    }

    Text {
        Layout.fillWidth: true
        visible: card.post.teaser.length > 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: "Public teaser: " + card.post.teaser
        color: Theme.text2
        font.pixelSize: Theme.fontSmall + 1
    }

    Text {
        Layout.fillWidth: true
        visible: card.post.opened && card.post.text.length > 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: card.post.text
        color: Theme.text
        font.pixelSize: Theme.fontSize
    }

    // The photo.
    Image {
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? Math.min(420, implicitHeight > 0 ? width * implicitHeight / Math.max(1, implicitWidth) : 240) : 0
        visible: card.mediaUrl.length > 0 && !card.mediaVideo
        source: visible ? card.mediaUrl : ""
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        Accessible.role: Accessible.Graphic
        Accessible.name: "Photo"
    }

    // The video: click to play or pause.
    Rectangle {
        id: videoBox
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? 300 : 0
        visible: card.mediaUrl.length > 0 && card.mediaVideo
        color: "black"
        radius: Theme.radiusControl
        MediaPlayer {
            id: player
            source: videoBox.visible ? card.mediaUrl : ""
            videoOutput: videoOut
            audioOutput: AudioOutput {}
        }
        VideoOutput {
            id: videoOut
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
        }
        Icon {
            anchors.centerIn: parent
            visible: player.playbackState !== MediaPlayer.PlayingState
            name: "play"
            size: 40
            color: "white"
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play()
        }
        Accessible.role: Accessible.Button
        Accessible.name: player.playbackState === MediaPlayer.PlayingState ? "Pause video" : "Play video"
    }

    Text {
        visible: card.mediaLoading
        text: "Decrypting..."
        color: Theme.text3
        font.pixelSize: Theme.fontSmall
    }
    Text {
        Layout.fillWidth: true
        visible: card.mediaFailed || card.post.failed
        wrapMode: Text.Wrap
        text: card.post.failed ? "This post couldn't be opened." : "The photo or video couldn't be opened."
        textFormat: Text.PlainText
        color: Theme.danger
        font.pixelSize: Theme.fontSmall
    }

    // Locked: buy it, or wait for the key.
    ColumnLayout {
        Layout.fillWidth: true
        visible: card.post.locked && !card.post.mine
        spacing: 8
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: card.post.awaitingKey
                ? "Bought. Your key arrives from the creator's app shortly (or you're refunded after 72 hours)."
                : "Encrypted. Buy it to read it."
            textFormat: Text.PlainText
            color: Theme.text2
            font.pixelSize: Theme.fontSmall + 1
        }
        RowLayout {
            visible: !card.post.awaitingKey
            spacing: 8
            RvButton {
                visible: card.earnings !== null && card.earnings.canBuy
                text: card.payingHere ? "Waiting for payment..." : "Buy for " + card.post.price
                enabled: card.earnings !== null && !card.earnings.paying
                onClicked: card.earnings.buyPost(card.post.id)
            }
            RvButton {
                visible: card.payingHere
                secondary: true
                text: "Stop waiting"
                onClicked: card.earnings.cancelPayment()
            }
            Text {
                visible: card.earnings !== null && !card.earnings.canBuy
                text: "Buying isn't available in this app right now."
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
        }
        Text {
            Layout.fillWidth: true
            visible: card.payingHere && card.earnings.payStatus.length > 0
            wrapMode: Text.Wrap
            text: card.earnings ? card.earnings.payStatus : ""
            textFormat: Text.PlainText
            color: Theme.text2
            font.pixelSize: Theme.fontSmall
        }
        Text {
            Layout.fillWidth: true
            visible: card.earnings !== null && card.earnings.payErrorContext === "exclusive_post" && card.earnings.payTarget === card.post.id
                     && card.earnings.payError.length > 0
            wrapMode: Text.Wrap
            text: card.earnings ? card.earnings.payError : ""
            textFormat: Text.PlainText
            color: Theme.danger
            font.pixelSize: Theme.fontSmall
        }
    }

    RvButton {
        visible: card.post.mine && card.post.status === "published"
        primary: false
        text: "Stop selling"
        onClicked: stopDialog.open()
    }
    ConfirmDialog {
        id: stopDialog
        title: "Stop selling this post?"
        message: "People who bought it keep it. Nobody new can buy it."
        confirmText: "Stop selling"
        onConfirmed: {
            card.earnings.unpublishPost(card.post.id)
            card.changed()
        }
    }
}
