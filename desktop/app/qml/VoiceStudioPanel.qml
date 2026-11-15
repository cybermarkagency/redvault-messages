// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import QtMultimedia
import Redvault

// Voice stories in the story studio, as on Android and the web: record from the microphone or open a sound file,
// cut the start and end, pick one of five voice effects, listen, add a title, a look and subtitles. The finished
// voice is a WAV made on this computer (VoiceStudio in studio_tools.hpp); nothing is sent anywhere until it is shared.
Rectangle {
    id: panel
    required property VoiceStudio studio
    property string title: ""
    property string background: "night"
    property var captions: []
    property var backgrounds: []
    /// {url, durationMs, waveform, title, background, captions}.
    signal done(var result)
    signal cancelled()
    signal editCaptions(var captions, real durationMs)

    function clock(ms) { return StudioTools.formatClock(ms) }
    function stopListening() {
        listen.stop()
        listen.source = ""
    }

    color: Theme.panel
    Accessible.role: Accessible.Dialog
    Accessible.name: "Voice story"
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }

    MediaPlayer {
        id: listen
        audioOutput: AudioOutput {}
        onMediaStatusChanged: if (mediaStatus === MediaPlayer.EndOfMedia) panel.stopListening()
    }
    FileDialog {
        id: pick
        title: qsTr("Open a sound file")
        fileMode: FileDialog.OpenFile
        nameFilters: ["Sound (*.wav *.mp3 *.m4a *.aac *.ogg *.opus *.webm *.flac)"]
        onAccepted: panel.studio.openFile(selectedFile)
    }
    Connections {
        target: panel.studio
        function onEditChanged() { panel.stopListening() }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 640)
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            RvButton {
                secondary: true
                text: qsTr("Cancel")
                onClicked: {
                    panel.stopListening()
                    if (panel.studio.recording) panel.studio.stopRecording()
                    panel.cancelled()
                }
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Voice story")
                horizontalAlignment: Text.AlignHCenter
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            RvButton {
                objectName: "voiceDone"
                text: qsTr("Use this")
                enabled: panel.studio.hasSound && !panel.studio.recording
                onClicked: {
                    panel.stopListening()
                    const r = panel.studio.finish()
                    if (r.url.toString().length === 0) return
                    // Subtitles past the end are dropped, and the last one stops at the end.
                    const kept = panel.captions.filter(c => c.startMs < r.durationMs).map(c => ({ startMs: c.startMs, endMs: Math.min(c.endMs, r.durationMs), text: c.text }))
                    panel.done({ url: r.url, durationMs: r.durationMs, waveform: r.waveform, title: panel.title.trim().substring(0, 120),
                                 background: panel.background, captions: StudioTools.checkCaptions(kept) })
                }
            }
        }

        // Nothing yet: record or open.
        ColumnLayout {
            Layout.fillWidth: true
            visible: !panel.studio.hasSound && !panel.studio.recording
            spacing: 10
            RvButton {
                Layout.fillWidth: true
                iconName: "mic"
                text: panel.studio.busy ? qsTr("Opening…") : qsTr("Record your voice")
                enabled: !panel.studio.busy
                onClicked: panel.studio.startRecording()
            }
            RvButton {
                Layout.fillWidth: true
                secondary: true
                text: qsTr("Open a sound file")
                enabled: !panel.studio.busy
                onClicked: pick.open()
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Up to 5 minutes. The sound is edited on this computer and only leaves it, encrypted, when you share the story.")
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
        }

        // Recording.
        ColumnLayout {
            Layout.fillWidth: true
            visible: panel.studio.recording
            spacing: 10
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: panel.clock(panel.studio.elapsedMs) + " / " + panel.clock(panel.studio.maxMs)
                color: Theme.text
                font.pixelSize: 28
                font.weight: Font.DemiBold
                font.family: Theme.monoFamily
            }
            Rectangle {
                Layout.fillWidth: true
                height: 8
                radius: 4
                color: Qt.alpha(Theme.text, 0.14)
                Rectangle { width: parent.width * panel.studio.level; height: parent.height; radius: 4; color: Theme.accent }
                Accessible.name: "Loudness"
            }
            RvButton {
                Layout.alignment: Qt.AlignHCenter
                danger: true
                text: qsTr("Stop")
                onClicked: panel.studio.stopRecording()
            }
        }

        // Editing.
        ColumnLayout {
            Layout.fillWidth: true
            visible: panel.studio.hasSound && !panel.studio.recording
            spacing: 10
            // The whole recording; the part kept is lit.
            Row {
                id: wave
                objectName: "voiceStudioWaveform"
                Layout.fillWidth: true
                height: 64
                spacing: 2
                readonly property real barWidth: Math.max(1, (width - spacing * 47) / 48)
                Repeater {
                    model: panel.studio.waveform
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        readonly property real at: (index + 0.5) / 48 * panel.studio.durationMs
                        anchors.verticalCenter: parent.verticalCenter
                        width: wave.barWidth
                        height: Math.max(3, 64 * modelData / 100)
                        radius: width / 2
                        color: at >= panel.studio.trimStartMs && at <= panel.studio.trimEndMs ? Theme.accent : Qt.alpha(Theme.text, 0.2)
                    }
                }
            }
            RangeSlider {
                id: trim
                objectName: "voiceTrim"
                Layout.fillWidth: true
                from: 0
                to: Math.max(1, panel.studio.durationMs)
                first.value: panel.studio.trimStartMs
                second.value: panel.studio.trimEndMs
                first.onMoved: panel.studio.setTrim(first.value, Math.min(second.value, first.value + panel.studio.maxMs))
                second.onMoved: panel.studio.setTrim(Math.max(first.value, second.value - panel.studio.maxMs), second.value)
                Accessible.name: "Trim"
                background: Rectangle {
                    x: trim.leftPadding
                    y: trim.topPadding + trim.availableHeight / 2 - height / 2
                    width: trim.availableWidth
                    height: 4
                    radius: 2
                    color: Qt.alpha(Theme.text, 0.14)
                    Rectangle {
                        x: trim.first.visualPosition * parent.width
                        width: (trim.second.visualPosition - trim.first.visualPosition) * parent.width
                        height: parent.height
                        radius: 2
                        color: Theme.accent
                    }
                }
                first.handle: Rectangle {
                    x: trim.leftPadding + trim.first.visualPosition * (trim.availableWidth - width)
                    y: trim.topPadding + trim.availableHeight / 2 - height / 2
                    width: 18
                    height: 18
                    radius: 9
                    color: Theme.text
                    Accessible.name: "Start"
                }
                second.handle: Rectangle {
                    x: trim.leftPadding + trim.second.visualPosition * (trim.availableWidth - width)
                    y: trim.topPadding + trim.availableHeight / 2 - height / 2
                    width: 18
                    height: 18
                    radius: 9
                    color: Theme.text
                    Accessible.name: "End"
                }
            }
            Text {
                text: qsTr("Keeping %1 to %2 of %3").arg(panel.clock(panel.studio.trimStartMs)).arg(panel.clock(panel.studio.trimEndMs)).arg(panel.clock(panel.studio.durationMs))
                color: Theme.text2
                font.pixelSize: Theme.fontSmall
            }
            Text { text: qsTr("Voice"); color: Theme.text; font.pixelSize: Theme.fontSize; font.weight: Font.DemiBold }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: panel.studio.filters
                    delegate: Chip {
                        required property var modelData
                        text: modelData.label
                        checked: panel.studio.filter === modelData.id
                        onClicked: panel.studio.filter = modelData.id
                    }
                }
            }
            RowLayout {
                spacing: 8
                RvButton {
                    secondary: true
                    iconName: listen.playbackState === MediaPlayer.PlayingState ? "pause" : "play"
                    text: listen.playbackState === MediaPlayer.PlayingState ? qsTr("Stop") : qsTr("Listen")
                    onClicked: {
                        if (listen.playbackState === MediaPlayer.PlayingState) {
                            panel.stopListening()
                            return
                        }
                        listen.source = panel.studio.preview()
                        listen.play()
                    }
                }
                RvButton {
                    secondary: true
                    text: qsTr("Start again")
                    onClicked: {
                        panel.stopListening()
                        panel.studio.clear()
                    }
                }
            }
            RvTextField {
                Layout.fillWidth: true
                placeholderText: qsTr("Title (optional)")
                text: panel.title
                maximumLength: 120
                onTextChanged: panel.title = text
            }
            RowLayout {
                spacing: 8
                Text { text: qsTr("Look"); color: Theme.text2; font.pixelSize: Theme.fontSmall }
                Repeater {
                    model: panel.backgrounds
                    delegate: AbstractButton {
                        id: look
                        required property string modelData
                        width: 26
                        height: 26
                        Accessible.name: look.modelData
                        onClicked: panel.background = look.modelData
                        background: Rectangle {
                            radius: 13
                            gradient: Gradient {
                                GradientStop { position: 0; color: Theme.storyBackgrounds[look.modelData][0] }
                                GradientStop { position: 1; color: Theme.storyBackgrounds[look.modelData][1] }
                            }
                            border.width: panel.background === look.modelData ? 3 : 1
                            border.color: Theme.text
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                RvButton {
                    secondary: true
                    text: panel.captions.length > 0 ? qsTr("Subtitles (%1)").arg(panel.captions.length) : qsTr("Subtitles")
                    onClicked: panel.editCaptions(panel.captions, panel.studio.trimEndMs - panel.studio.trimStartMs)
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: panel.studio.error.length > 0
            text: panel.studio.error
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.danger
            font.pixelSize: Theme.fontSmall
        }
    }
}
