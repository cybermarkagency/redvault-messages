// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The attachments being uploaded for a post or a suggestion: what each is,
// its progress (or "Ready", or what went wrong), and Remove.
ColumnLayout {
    id: list
    required property ChannelUploads uploads
    visible: uploads !== null && uploads.count > 0
    spacing: 6

    Repeater {
        model: list.uploads ? list.uploads.items : []
        delegate: Rectangle {
            id: row
            required property var modelData
            Layout.fillWidth: true
            implicitHeight: 40
            radius: Theme.radiusControl
            color: Theme.surface2
            border.color: Theme.border

            Text {
                id: name
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: status.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideMiddle
                text: row.modelData.kindLabel + ": " + row.modelData.name + " · " + row.modelData.sizeLabel
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontSmall + 1
            }
            Item {
                id: status
                anchors.right: remove.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: row.modelData.error.length > 0 ? Math.min(errorText.implicitWidth, row.width * 0.45) : 110
                height: 20
                Text {
                    id: errorText
                    anchors.fill: parent
                    visible: row.modelData.error.length > 0
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    text: row.modelData.error
                    textFormat: Text.PlainText
                    color: Theme.danger
                    font.pixelSize: Theme.fontSmall
                }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.modelData.ready === true
                    spacing: 4
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: "check"
                        size: 14
                        color: Theme.accent
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Ready"
                        color: Theme.accent
                        font.pixelSize: Theme.fontSmall
                    }
                }
                ProgressBar {
                    anchors.fill: parent
                    visible: row.modelData.ready !== true && row.modelData.error.length === 0
                    from: 0
                    to: 1
                    value: row.modelData.progress
                    Accessible.name: "Uploading " + row.modelData.name
                }
            }
            RvButton {
                id: remove
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                primary: false
                implicitHeight: 30
                text: "Remove"
                Accessible.name: "Remove " + row.modelData.name
                onClicked: list.uploads.remove(row.modelData.key)
            }
        }
    }
}
