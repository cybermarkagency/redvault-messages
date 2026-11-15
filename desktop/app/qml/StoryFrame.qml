// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtMultimedia
import Redvault

// One story, drawn from its decrypted details the same way the web app does:
// the background or the photo/video, then text and stickers on top. Sizes are
// fractions of the frame, so it looks the same at any size.
Rectangle {
    id: frame
    /// {kind, text, background, font, overlays: [...], captions, waveform} (StoriesSession's story maps).
    property var story: ({})
    /// The decrypted photo ("image://chatimage/...") or video or voice (a private file URL); empty while loading.
    property string mediaUrl: ""
    property bool muted: false
    property bool paused: false
    /// The composer draws its own (editable) text and stickers.
    property bool editing: false
    readonly property real progress: player.duration > 0 ? player.position / player.duration : 0
    /// A video's length in ms once it has loaded (the composer sends it with the story).
    readonly property int duration: player.duration
    readonly property bool loading: frame.story.kind !== "text" && frame.mediaUrl.length === 0
    readonly property bool playable: frame.story.kind === "video" || frame.story.kind === "audio"
    /// Subtitles of a video or voice story: the line for this moment of the sound.
    readonly property string caption: {
        const list = frame.story.captions || []
        const at = player.position
        for (let i = 0; i < list.length; i++) if (at >= list[i].startMs && at < list[i].endMs) return list[i].text
        return ""
    }
    /// A voice story's bars: its own, or a flat line while it has none.
    readonly property var bars: (frame.story.waveform && frame.story.waveform.length > 0) ? frame.story.waveform : Array(48).fill(12)
    /// Words and emoji move (pulse, bounce, fade) unless Windows is set to show fewer animations.
    property bool motion: !StudioTools.reducedMotion
    property real motionMs: 0
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
        if (frame.playable) player.position = 0
    }
    // One clock for every moving overlay, running only while something moves.
    FrameAnimation {
        running: frame.motion && frame.visible && !frame.editing && (frame.story.overlays || []).some(o => o.anim && o.anim.length > 0)
        onTriggered: frame.motionMs += frameTime * 1000
    }

    color: Theme.storyStage
    clip: true

    // A text story's background.
    gradient: frame.story.kind === "text" || frame.story.kind === "audio" ? textBackground : null
    Gradient {
        id: textBackground
        readonly property var pair: Theme.storyBackgrounds[frame.story.background] || (frame.story.kind === "audio" ? Theme.storyBackgrounds.night : Theme.storyBackgrounds.sunset)
        GradientStop { position: 0.0; color: textBackground.pair[0] }
        GradientStop { position: 1.0; color: textBackground.pair[1] }
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
        source: frame.playable ? frame.mediaUrl : ""
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
        if (!frame.playable || player.source.toString().length === 0) return
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

    // A voice story: its title, a microphone and the picture of its loudness, lit as it plays.
    Column {
        objectName: "voiceStory"
        visible: frame.story.kind === "audio"
        anchors.centerIn: parent
        width: parent.width * 0.84
        spacing: frame.height * 0.035
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: frame.width * 0.22
            height: width
            radius: width / 2
            color: Theme.storyScrim
            Icon { anchors.centerIn: parent; name: "mic"; size: parent.width * 0.48; color: Theme.storyColors.white }
        }
        Text {
            width: parent.width
            visible: text.length > 0
            text: frame.story.kind === "audio" ? (frame.story.text || "") : ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            color: Theme.storyColors.white
            font.family: frame.fontFamily(frame.story.font)
            font.weight: frame.story.font === "classic" ? Font.DemiBold : Font.Bold
            font.capitalization: frame.story.font === "strong" ? Font.AllUppercase : Font.MixedCase
            font.pixelSize: Math.max(12, frame.width * 0.065)
        }
        Row {
            id: waveRow
            objectName: "voiceWaveform"
            anchors.horizontalCenter: parent.horizontalCenter
            height: frame.height * 0.12
            spacing: Math.max(1, frame.width * 0.006)
            readonly property real barWidth: Math.max(1, (parent.width - spacing * (frame.bars.length - 1)) / frame.bars.length)
            Repeater {
                model: frame.bars
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    anchors.verticalCenter: parent.verticalCenter
                    width: waveRow.barWidth
                    height: Math.max(3, waveRow.height * modelData / 100)
                    radius: width / 2
                    color: (index + 0.5) / frame.bars.length <= frame.progress ? Theme.storyColors.white : Theme.storyTrack
                }
            }
        }
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
            // Animated words and emoji: the same curves the web's CSS and Android draw (core studio/story_motion).
            readonly property var move: overlay.modelData.anim && frame.motion ? StudioTools.motionFrame(overlay.modelData.anim, frame.motionMs) : null
            x: overlay.modelData.x * frame.width - width / 2
            y: overlay.modelData.y * frame.height - height / 2 + (overlay.move ? overlay.move.lift * height : 0)
            opacity: overlay.move ? overlay.move.alpha : 1
            width: label.implicitWidth + (overlay.modelData.type === "text" ? frame.width * 0.05 : 0)
            height: label.implicitHeight + (overlay.modelData.type === "text" ? frame.width * 0.025 : 0)
            rotation: overlay.modelData.rotation
            scale: overlay.modelData.scale * (overlay.move ? overlay.move.scale : 1)
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

    // Subtitles of a video or voice story, over everything else.
    Rectangle {
        objectName: "storyCaption"
        visible: frame.playable && frame.caption.length > 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        // Above the reply bar in the viewer, and above the share controls in the composer.
        anchors.bottomMargin: parent.height * (frame.editing ? 0.25 : 0.16)
        width: Math.min(parent.width * 0.9, captionText.implicitWidth + 20)
        height: captionText.implicitHeight + 10
        radius: 6
        color: Theme.storyScrim
        Text {
            id: captionText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, frame.width * 0.9 - 20)
            text: frame.caption
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            color: Theme.storyColors.white
            font.pixelSize: Math.max(11, frame.width * 0.042)
            font.weight: Font.DemiBold
        }
    }
}
