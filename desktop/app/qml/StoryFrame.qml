// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtMultimedia

// One story, drawn from its decrypted details the same way the web app does:
// the background or the photo/video, then text and stickers on top. Sizes are
// fractions of the frame, so it looks the same at any size.
Rectangle {
    id: frame
    /// {kind, text, background, font, overlays: [...]} (StoriesSession's story maps).
    property var story: ({})
    /// The decrypted photo ("image://chatimage/...") or video (a private file URL); empty while loading.
    property string mediaUrl: ""
    property bool muted: false
    property bool paused: false
    /// The composer draws its own (editable) text and stickers.
    property bool editing: false
    readonly property real progress: player.duration > 0 ? player.position / player.duration : 0
    /// A video's length in ms once it has loaded (the composer sends it with the story).
    readonly property int duration: player.duration
    readonly property bool loading: frame.story.kind !== "text" && frame.mediaUrl.length === 0
    signal ready()
    signal ended()
    signal failed()

    function fontFamily(name) {
        return name === "typewriter" ? Theme.monoFamily : (name === "modern" || name === "strong") ? Theme.displayFamily : Theme.fontFamily
    }
    function colorOf(name) {
        return Theme.storyColors[name] || Theme.storyColors.white
    }
    function restart() {
        if (frame.story.kind === "video") player.position = 0
    }

    color: Theme.storyStage
    clip: true

    // A text story's background.
    gradient: frame.story.kind === "text" ? textBackground : null
    Gradient {
        id: textBackground
        GradientStop { position: 0.0; color: (Theme.storyBackgrounds[frame.story.background] || Theme.storyBackgrounds.sunset)[0] }
        GradientStop { position: 1.0; color: (Theme.storyBackgrounds[frame.story.background] || Theme.storyBackgrounds.sunset)[1] }
    }

    // A photo: a soft, dim copy fills the frame behind it (a tiny version, stretched).
    Image {
        anchors.fill: parent
        visible: frame.story.kind === "image" && status === Image.Ready
        source: frame.story.kind === "image" ? frame.mediaUrl : ""
        sourceSize: Qt.size(12, 20)
        fillMode: Image.PreserveAspectCrop
        autoTransform: true
        smooth: true
        opacity: 0.45
        asynchronous: true
    }
    Image {
        id: photo
        anchors.fill: parent
        visible: frame.story.kind === "image"
        source: frame.story.kind === "image" ? frame.mediaUrl : ""
        fillMode: Image.PreserveAspectFit
        autoTransform: true
        asynchronous: true
        smooth: true
        mipmap: true
        onStatusChanged: {
            if (status === Image.Ready) frame.ready()
            else if (status === Image.Error) frame.failed()
        }
    }

    MediaPlayer {
        id: player
        source: frame.story.kind === "video" ? frame.mediaUrl : ""
        videoOutput: video
        audioOutput: AudioOutput { muted: frame.muted }
        onMediaStatusChanged: {
            if (mediaStatus === MediaPlayer.LoadedMedia || mediaStatus === MediaPlayer.BufferedMedia) {
                frame.ready()
                if (!frame.paused) player.play()
            } else if (mediaStatus === MediaPlayer.EndOfMedia) {
                frame.ended()
            } else if (mediaStatus === MediaPlayer.InvalidMedia) {
                frame.failed()
            }
        }
    }
    VideoOutput {
        id: video
        anchors.fill: parent
        visible: frame.story.kind === "video"
        fillMode: VideoOutput.PreserveAspectFit
    }
    onPausedChanged: {
        if (frame.story.kind !== "video" || player.source.toString().length === 0) return
        if (frame.paused) player.pause()
        else player.play()
    }

    // Loading.
    Rectangle {
        anchors.centerIn: parent
        visible: frame.loading
        width: 34
        height: 34
        radius: 17
        color: "transparent"
        border.width: 3
        border.color: Theme.storyTrack
        Rectangle {
            width: 8
            height: 8
            radius: 4
            color: Theme.storyChrome
            x: 13 + 13 * Math.cos(spin.angle)
            y: 13 + 13 * Math.sin(spin.angle)
        }
        NumberAnimation on rotation { from: 0; to: 360; duration: 900; loops: Animation.Infinite; running: frame.loading }
        QtObject { id: spin; property real angle: 0 }
    }

    // A text story's words.
    Text {
        visible: frame.story.kind === "text" && !frame.editing
        anchors.centerIn: parent
        width: parent.width * 0.84
        text: frame.story.text || ""
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        color: Theme.storyColors.white
        font.family: frame.fontFamily(frame.story.font)
        font.weight: frame.story.font === "typewriter" ? Font.Medium : frame.story.font === "classic" ? Font.DemiBold : Font.Bold
        font.capitalization: frame.story.font === "strong" ? Font.AllUppercase : Font.MixedCase
        font.pixelSize: Math.max(12, frame.width * 0.075)
        lineHeight: 1.2
    }

    // Text and stickers.
    Repeater {
        model: frame.editing ? [] : (frame.story.overlays || [])
        delegate: Item {
            id: overlay
            required property var modelData
            x: overlay.modelData.x * frame.width - width / 2
            y: overlay.modelData.y * frame.height - height / 2
            width: label.implicitWidth + (overlay.modelData.type === "text" ? frame.width * 0.05 : 0)
            height: label.implicitHeight + (overlay.modelData.type === "text" ? frame.width * 0.025 : 0)
            rotation: overlay.modelData.rotation
            scale: overlay.modelData.scale
            Rectangle {
                anchors.fill: parent
                visible: overlay.modelData.type === "text" && overlay.modelData.boxed
                radius: height * 0.25
                color: frame.colorOf(overlay.modelData.color)
            }
            Text {
                id: label
                anchors.centerIn: parent
                width: Math.min(implicitWidth, frame.width * 0.86)
                text: overlay.modelData.text
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                color: overlay.modelData.type === "emoji" ? Theme.storyColors.white
                     : overlay.modelData.boxed ? frame.colorOf(Theme.storyLightColors.indexOf(overlay.modelData.color) >= 0 ? "black" : "white")
                     : frame.colorOf(overlay.modelData.color)
                style: overlay.modelData.type === "text" && !overlay.modelData.boxed ? Text.Raised : Text.Normal
                styleColor: Theme.storyScrim
                font.family: frame.fontFamily(overlay.modelData.font)
                font.weight: overlay.modelData.font === "classic" ? Font.DemiBold : Font.Bold
                font.capitalization: overlay.modelData.font === "strong" ? Font.AllUppercase : Font.MixedCase
                font.pixelSize: Math.max(10, frame.width * (overlay.modelData.type === "emoji" ? 0.15 : 0.06))
            }
        }
    }
}
