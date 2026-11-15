// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import QtMultimedia
import Redvault

// Scan the safety-number code on someone's phone: with the webcam, from a
// picture file, or from a screenshot on the clipboard. Decoded on this
// computer (QrScanner); the caller decides what the text means.
RvDialog {
    id: dialog
    title: qsTr("Scan their code")
    width: 520
    property string problem: ""
    signal scanned(string text)

    function finish(text) {
        if (text.length === 0) return
        camera.active = false
        dialog.close()
        dialog.scanned(text)
    }

    onOpened: {
        problem = ""
        camera.active = true
    }
    onClosed: camera.active = false

    CaptureSession {
        camera: Camera {
            id: camera
            active: false
            onErrorOccurred: (error, message) => dialog.problem = qsTr("The camera isn't available: ") + message
        }
        videoOutput: preview
    }

    QrScanner {
        id: scanner
        videoSink: dialog.opened ? preview.videoSink : null
        onFound: (text) => dialog.finish(text)
    }

    FileDialog {
        id: pictureDialog
        title: qsTr("Choose a picture of their code")
        nameFilters: ["Pictures (*.png *.jpg *.jpeg *.bmp *.webp)"]
        onAccepted: {
            const text = scanner.decodeFile(selectedFile)
            if (text.length > 0) dialog.finish(text)
            else dialog.problem = qsTr("There's no readable code in that picture.")
        }
    }

    ColumnLayout {
        width: parent.width
        spacing: 12

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Hold their phone up to your camera with the code showing (Verify screen). Nothing leaves this computer.")
            color: Theme.text2
            font.pixelSize: Theme.fontSmall + 1
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 300
            radius: Theme.radiusCard
            color: Theme.callBg
            clip: true
            VideoOutput {
                id: preview
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectCrop
            }
            // The aiming square.
            Rectangle {
                anchors.centerIn: parent
                width: 200
                height: 200
                color: "transparent"
                radius: 12
                border.width: 2
                border.color: "#EBFFFFFF"
            }
            Text {
                anchors.centerIn: parent
                visible: !camera.active || dialog.problem.length > 0
                text: qsTr("No camera")
                color: "#B3FFFFFF"
            }
        }

        Banner {
            Layout.fillWidth: true
            text: dialog.problem
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            RvButton {
                secondary: true
                iconName: "image"
                text: qsTr("From a picture…")
                onClicked: {
                    App.lock.expectExternalReturn()
                    pictureDialog.open()
                }
            }
            RvButton {
                secondary: true
                iconName: "copy"
                text: qsTr("Paste a screenshot")
                onClicked: {
                    const text = scanner.decodeClipboard()
                    if (text.length > 0) dialog.finish(text)
                    else dialog.problem = qsTr("There's no picture with a readable code on the clipboard.")
                }
            }
            Item { Layout.fillWidth: true }
            RvButton { primary: false; text: qsTr("Cancel"); onClicked: dialog.close() }
        }
    }
}
