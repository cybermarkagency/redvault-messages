// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// "Are you sure?" in the app's tokens: a message, Cancel, and one action
// button (red when `danger`). Emits `confirmed` when the action is chosen.
RvDialog {
    id: dialog
    property string message: ""
    property string confirmText: "OK"
    property bool danger: false
    signal confirmed()

    width: 440

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 18

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: dialog.message
            textFormat: Text.PlainText
            color: Theme.text2
            font.pixelSize: Theme.fontSize
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            RvButton {
                primary: false
                text: "Cancel"
                onClicked: dialog.close()
            }
            RvButton {
                text: dialog.confirmText
                danger: dialog.danger
                onClicked: {
                    dialog.close()
                    dialog.confirmed()
                }
            }
        }
    }
}
