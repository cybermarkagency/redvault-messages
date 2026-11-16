// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtMultimedia
import Redvault

// The Files viewer: a picture, text, CSV, JSON, audio, video or PDF shown
// over the whole window, without handing the decrypted file to another app.
// FilesSession decrypts it into this session's private cache
// (files_viewer.cpp) and deletes the copy when this closes. Esc or the close
// button closes it.
Popup {
    id: viewer
    objectName: "filePreviewDialog"
    required property FilesSession files
    /// Download… from the header (FilesView opens its save dialog).
    signal saveRequested(string id, string name)

    readonly property var v: files ? files.viewer : ({})
    readonly property string kind: v.kind || ""
    readonly property string phase: v.state || ""
    readonly property bool isMedia: kind === "audio" || kind === "video"

    function openFor(id) {
        files.openPreview(id)
        open()
    }

    function clock(ms) {
        const s = Math.max(0, Math.floor(ms / 1000))
        const h = Math.floor(s / 3600)
        const m = Math.floor((s % 3600) / 60)
        return (h > 0 ? h + ":" + String(m).padStart(2, "0") : m) + ":" + String(s % 60).padStart(2, "0")
    }

    parent: Overlay.overlay
    x: 0
    y: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape
    background: Rectangle { color: "#F20B0C0F" }

    onClosed: {
        // Let go of the file before it's deleted.
        media.stop()
        media.source = ""
        files.closePreview()
    }

    // The viewer was closed from C++ (Lock now) or another file was opened.
    Connections {
        target: viewer.files
        function onViewerChanged() {
            if (viewer.opened && (viewer.files.viewer.id || "") === "") viewer.close()
        }
    }

    // Set by hand (not bound), so closing can let go of the file first.
    onPhaseChanged: {
        if (phase === "ready" && isMedia && (v.url || "").length > 0) {
            media.source = v.url
            media.play()
        }
    }

    MediaPlayer {
        id: media
        audioOutput: AudioOutput { id: audioOut }
        videoOutput: videoOut
    }

    contentItem: Item {
        focus: true
        Keys.onSpacePressed: (event) => {
            if (!viewer.isMedia) return
            event.accepted = true
            media.playbackState === MediaPlayer.PlayingState ? media.pause() : media.play()
        }

        // Header: name, size and type, Download, Close.
        RowLayout {
            id: header
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 18
            spacing: 8
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    Layout.fillWidth: true
                    text: viewer.v.name || ""
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: "#EDEEF0"
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
                RowLayout {
                    spacing: 6
                    Icon { name: "shield-check"; size: 13; color: Theme.ok }
                    Text {
                        text: (viewer.v.sizeText || "") + qsTr(" · decrypted on this PC, shown only inside RedVault")
                        textFormat: Text.PlainText
                        color: "#9599A3"
                        font.pixelSize: 12
                    }
                }
            }
            IconButton {
                iconName: "download"
                iconColor: "#C3C6CD"
                tip: qsTr("Download…")
                onClicked: viewer.saveRequested(viewer.v.id || "", viewer.v.name || "")
            }
            IconButton {
                objectName: "previewClose"
                iconName: "x"
                iconColor: "#C3C6CD"
                tip: qsTr("Close (Esc)")
                onClicked: viewer.close()
            }
        }

        Item {
            id: stage
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.bottom: parent.bottom
            anchors.margins: 24
            anchors.topMargin: 16

            // Decrypting.
            Column {
                anchors.centerIn: parent
                spacing: 12
                visible: viewer.phase === "loading"
                BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: parent.visible }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: (viewer.v.progress || 0) > 0 ? qsTr("Decrypting… %1%").arg(Math.round(viewer.v.progress * 100)) : qsTr("Decrypting…")
                    textFormat: Text.PlainText
                    color: "#C3C6CD"
                    font.pixelSize: 14
                }
            }

            // Couldn't show it.
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width, 440)
                spacing: 14
                visible: viewer.phase === "error"
                Icon { Layout.alignment: Qt.AlignHCenter; name: "triangle-alert"; size: 30; color: Theme.warn }
                Text {
                    objectName: "previewError"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: viewer.v.message || ""
                    textFormat: Text.PlainText
                    color: "#EDEEF0"
                    font.pixelSize: 14
                }
                RvButton {
                    Layout.alignment: Qt.AlignHCenter
                    secondary: true
                    iconName: "download"
                    text: qsTr("Download instead")
                    onClicked: viewer.saveRequested(viewer.v.id || "", viewer.v.name || "")
                }
            }

            Loader {
                anchors.fill: parent
                active: viewer.phase === "ready" && !viewer.isMedia
                sourceComponent: viewer.kind === "image" ? imageView
                               : viewer.kind === "csv" ? csvView
                               : viewer.kind === "pdf" ? pdfView
                               : textView
            }

            // Audio and video share the player and its controls.
            Item {
                anchors.fill: parent
                visible: viewer.phase === "ready" && viewer.isMedia
                VideoOutput {
                    id: videoOut
                    anchors.fill: parent
                    anchors.bottomMargin: controls.height + 16
                    visible: viewer.kind === "video"
                    fillMode: VideoOutput.PreserveAspectFit
                    TapHandler { onTapped: media.playbackState === MediaPlayer.PlayingState ? media.pause() : media.play() }
                }
                Column {
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -controls.height / 2
                    visible: viewer.kind === "audio"
                    spacing: 14
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 120
                        height: 120
                        radius: 60
                        color: "#20232A"
                        Icon { anchors.centerIn: parent; name: "headphones"; size: 48; color: "#E7677F" }
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: media.error !== MediaPlayer.NoError
                    width: Math.min(parent.width, 440)
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: qsTr("This file can't be played here (its format or codec isn't supported). Download it instead.")
                    textFormat: Text.PlainText
                    color: "#EDEEF0"
                    font.pixelSize: 14
                }

                Rectangle {
                    id: controls
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    width: Math.min(parent.width, 720)
                    height: 56
                    radius: 28
                    color: "#20232A"
                    border.color: "#323640"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 18
                        spacing: 10
                        IconButton {
                            objectName: "previewPlay"
                            round: true
                            filled: true
                            iconName: media.playbackState === MediaPlayer.PlayingState ? "pause" : "play"
                            iconColor: "#EDEEF0"
                            tip: media.playbackState === MediaPlayer.PlayingState ? qsTr("Pause (Space)") : qsTr("Play (Space)")
                            onClicked: media.playbackState === MediaPlayer.PlayingState ? media.pause() : media.play()
                        }
                        Text {
                            text: viewer.clock(media.position)
                            textFormat: Text.PlainText
                            color: "#C3C6CD"
                            font.family: Theme.monoFamily
                            font.pixelSize: 12
                        }
                        Slider {
                            id: seek
                            Layout.fillWidth: true
                            from: 0
                            to: Math.max(1, media.duration)
                            enabled: media.seekable
                            value: pressed ? value : media.position
                            onMoved: media.position = Math.round(value)
                            Accessible.name: qsTr("Position")
                        }
                        Text {
                            text: viewer.clock(media.duration)
                            textFormat: Text.PlainText
                            color: "#C3C6CD"
                            font.family: Theme.monoFamily
                            font.pixelSize: 12
                        }
                        IconButton {
                            iconName: audioOut.muted ? "speaker-off" : "speaker"
                            iconColor: "#C3C6CD"
                            tip: audioOut.muted ? qsTr("Unmute") : qsTr("Mute")
                            onClicked: audioOut.muted = !audioOut.muted
                        }
                        Slider {
                            Layout.preferredWidth: 90
                            from: 0
                            to: 1
                            value: audioOut.volume
                            onMoved: audioOut.volume = value
                            Accessible.name: qsTr("Volume")
                        }
                    }
                }
            }
        }
    }

    // A picture: fits the window; scroll to zoom, drag to move it.
    Component {
        id: imageView
        Item {
            id: imageBox
            clip: true
            property real zoom: 1
            AnimatedImage {
                id: picture
                objectName: "previewImage"
                width: parent.width
                height: parent.height
                scale: imageBox.zoom
                asynchronous: true
                cache: false
                fillMode: Image.PreserveAspectFit
                source: viewer.v.url || ""
                playing: status === AnimatedImage.Ready
                DragHandler { target: picture; enabled: imageBox.zoom > 1 }
            }
            WheelHandler {
                onWheel: (event) => {
                    imageBox.zoom = Math.max(1, Math.min(8, imageBox.zoom * (event.angleDelta.y > 0 ? 1.15 : 1 / 1.15)))
                    if (imageBox.zoom === 1) imageBox.reset()
                }
            }
            function reset() { zoom = 1; picture.x = 0; picture.y = 0 }
            TapHandler { onDoubleTapped: imageBox.reset() }
            Text {
                anchors.centerIn: parent
                visible: picture.status === AnimatedImage.Error
                text: qsTr("This picture can't be shown here. Download it instead.")
                textFormat: Text.PlainText
                color: "#EDEEF0"
                font.pixelSize: 14
            }
        }
    }

    // Text and JSON (pretty-printed when it parses): plain text only, never rendered.
    Component {
        id: textView
        Rectangle {
            radius: Theme.radiusCard
            color: Theme.surface
            border.color: Theme.border
            ScrollView {
                anchors.fill: parent
                anchors.margins: 1
                TextArea {
                    objectName: "previewText"
                    readOnly: true
                    selectByMouse: true
                    textFormat: TextEdit.PlainText
                    wrapMode: viewer.kind === "json" ? TextEdit.NoWrap : TextEdit.Wrap
                    text: viewer.v.text || ""
                    color: Theme.text
                    font.family: Theme.monoFamily
                    font.pixelSize: 13
                    padding: 18
                    background: null
                }
            }
        }
    }

    // CSV as a table: the first row as the header.
    Component {
        id: csvView
        Rectangle {
            id: table
            radius: Theme.radiusCard
            color: Theme.surface
            border.color: Theme.border
            clip: true
            readonly property var rows: viewer.v.rows || []
            readonly property int columns: rows.reduce((n, r) => Math.max(n, r.length), 0)
            readonly property int cellWidth: 170
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 1
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    Layout.margins: 12
                    visible: viewer.v.truncated === true
                    text: qsTr("Showing the first 1,000 rows and 50 columns. Download the file to see all of it.")
                    textFormat: Text.PlainText
                    color: Theme.warn
                    font.pixelSize: 12
                }
                Text {
                    Layout.fillWidth: true
                    Layout.margins: 12
                    visible: table.rows.length === 0
                    text: qsTr("This file is empty.")
                    textFormat: Text.PlainText
                    color: Theme.text2
                }
                ListView {
                    id: csvRows
                    objectName: "previewCsv"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: table.rows
                    contentWidth: Math.max(width, table.columns * table.cellWidth)
                    flickableDirection: Flickable.AutoFlickDirection
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    ScrollBar.horizontal: ScrollBar {}
                    delegate: Rectangle {
                        id: csvRow
                        required property var modelData
                        required property int index
                        width: csvRows.contentWidth
                        height: 34
                        color: index === 0 ? Theme.surface2 : index % 2 === 0 ? Qt.alpha(Theme.surface2, 0.4) : "transparent"
                        Row {
                            Repeater {
                                model: csvRow.modelData
                                delegate: Text {
                                    required property string modelData
                                    width: table.cellWidth
                                    height: 34
                                    leftPadding: 12
                                    rightPadding: 8
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                    text: modelData
                                    textFormat: Text.PlainText
                                    color: Theme.text
                                    font.pixelSize: 13
                                    font.weight: csvRow.index === 0 ? Font.DemiBold : Font.Normal
                                    ToolTip.visible: cellHover.hovered && truncated
                                    ToolTip.text: modelData
                                    HoverHandler { id: cellHover }
                                }
                            }
                        }
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
                    }
                }
            }
        }
    }

    // PDFs: only when built with Qt PDF (FilesSession.pdfSupported); otherwise previewKind is never "pdf".
    Component {
        id: pdfView
        Loader {
            source: viewer.files.pdfSupported ? "PdfPreview.qml" : ""
            onLoaded: item.source = Qt.binding(() => viewer.v.url || "")
        }
    }
}
