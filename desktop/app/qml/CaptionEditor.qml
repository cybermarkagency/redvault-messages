// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// Subtitles for a video or voice story: typed line by line, spread evenly from typed text, or read from an .srt file.
// Nothing is transcribed: that would mean sending the recording to a service. The lines are checked the way every
// app reads them (StudioTools.checkCaptions): at most 60, 120 characters each, within 10 minutes.
Rectangle {
    id: panel
    property var lines: []
    property real durationMs: 0
    signal done(var captions)
    signal cancelled()

    function open(captions, duration) {
        panel.lines = captions.map(c => ({ startMs: c.startMs, endMs: c.endMs, text: c.text }))
        panel.durationMs = duration
        panel.visible = true
        typed.text = ""
    }
    function change(index, field, value) {
        const next = panel.lines.slice()
        next[index] = Object.assign({}, next[index])
        next[index][field] = value
        panel.lines = next
    }

    color: Theme.panel
    Accessible.role: Accessible.Dialog
    Accessible.name: "Subtitles"
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }

    FileDialog {
        id: pick
        title: qsTr("Open subtitles")
        fileMode: FileDialog.OpenFile
        nameFilters: ["SubRip subtitles (*.srt)"]
        onAccepted: {
            const read = StudioTools.readSrt(selectedFile)
            if (read.length === 0) message.text = qsTr("No subtitles could be read from that file.")
            else {
                panel.lines = read
                message.text = read.length + qsTr(" lines read.")
            }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 680)
        height: Math.min(parent.height - 32, 720)
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            RvButton { secondary: true; text: qsTr("Cancel"); onClicked: panel.cancelled() }
            Text {
                Layout.fillWidth: true
                text: qsTr("Subtitles")
                horizontalAlignment: Text.AlignHCenter
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            RvButton {
                objectName: "captionsDone"
                text: qsTr("Done")
                onClicked: panel.done(StudioTools.checkCaptions(panel.lines))
            }
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Typed by you: nothing is sent anywhere to be transcribed. Times are minutes:seconds, like 0:04.5. The sound is %1 long.").arg(StudioTools.formatClock(panel.durationMs))
            wrapMode: Text.Wrap
            color: Theme.text3
            font.pixelSize: Theme.fontSmall
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: panel.lines
            delegate: RowLayout {
                id: line
                required property var modelData
                required property int index
                width: ListView.view.width
                spacing: 6
                RvTextField {
                    Layout.preferredWidth: 80
                    text: StudioTools.formatClock(line.modelData.startMs)
                    Accessible.name: "Start of line " + (line.index + 1)
                    onEditingFinished: {
                        const ms = StudioTools.parseClock(text)
                        if (ms >= 0) panel.change(line.index, "startMs", ms)
                        else text = StudioTools.formatClock(line.modelData.startMs)
                    }
                }
                RvTextField {
                    Layout.preferredWidth: 80
                    text: StudioTools.formatClock(line.modelData.endMs)
                    Accessible.name: "End of line " + (line.index + 1)
                    onEditingFinished: {
                        const ms = StudioTools.parseClock(text)
                        if (ms >= 0) panel.change(line.index, "endMs", ms)
                        else text = StudioTools.formatClock(line.modelData.endMs)
                    }
                }
                RvTextField {
                    Layout.fillWidth: true
                    text: line.modelData.text
                    maximumLength: 120
                    placeholderText: qsTr("What is said")
                    Accessible.name: "Words of line " + (line.index + 1)
                    onEditingFinished: panel.change(line.index, "text", text)
                }
                IconButton {
                    iconName: "trash"
                    tip: qsTr("Remove this line")
                    onClicked: {
                        const next = panel.lines.slice()
                        next.splice(line.index, 1)
                        panel.lines = next
                    }
                }
            }
        }
        RowLayout {
            spacing: 8
            RvButton {
                secondary: true
                iconName: "plus"
                text: qsTr("Add a line")
                enabled: panel.lines.length < 60
                onClicked: {
                    const start = panel.lines.length > 0 ? panel.lines[panel.lines.length - 1].endMs : 0
                    const end = Math.min(start + 2000, Math.max(start + 500, panel.durationMs))
                    panel.lines = panel.lines.concat([{ startMs: start, endMs: end, text: "" }])
                }
            }
            RvButton { secondary: true; text: qsTr("Open .srt file"); onClicked: pick.open() }
        }
        Text { text: qsTr("Or type the words, one line each, and spread them evenly over the sound:"); color: Theme.text2; font.pixelSize: Theme.fontSmall }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                TextArea {
                    id: typed
                    objectName: "captionsTyped"
                    wrapMode: TextEdit.Wrap
                    color: Theme.text
                    placeholderText: qsTr("First line\nSecond line")
                    placeholderTextColor: Theme.text3
                    background: Rectangle { radius: Theme.radiusSmall; color: Theme.surface2 }
                    Accessible.name: "Typed lines"
                }
            }
            RvButton {
                secondary: true
                text: qsTr("Spread")
                enabled: typed.text.trim().length > 0 && panel.durationMs > 0
                onClicked: panel.lines = StudioTools.evenCaptions(typed.text, panel.durationMs)
            }
        }
        Text {
            id: message
            Layout.fillWidth: true
            visible: text.length > 0
            textFormat: Text.PlainText
            color: Theme.text2
            font.pixelSize: Theme.fontSmall
        }
    }
}
