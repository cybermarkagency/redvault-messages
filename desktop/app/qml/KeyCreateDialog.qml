// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Create a key: a random 256-bit key made on this PC and saved to the
// password vault as a file key, with an optional password of its own.
LockDialog {
    id: dialog
    required property FilesSession files
    property string error

    title: "Create a key"
    iconName: "key"
    subtitle: "A random 256-bit key is made on this PC and saved to your password vault as a file key."

    onOpened: {
        error = ""
        nameField.text = ""
        protect.checked = true
        password.words = false
        password.generate()
        nameField.forceActiveFocus()
    }

    Connections {
        target: dialog.files
        function onLockDone(action, error) {
            if (action !== "createKey" || !dialog.opened) return
            if (error.length === 0) dialog.close()
            else dialog.error = error
        }
    }

    contentItem: ColumnLayout {
        spacing: 16

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            Text { text: "Name"; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
            RvTextField {
                id: nameField
                Layout.fillWidth: true
                implicitHeight: 46
                placeholderText: "Legal documents"
                maximumLength: 80
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: protect.implicitHeight + 28
            radius: 14
            color: Theme.surface
            RvSwitch {
                id: protect
                anchors.fill: parent
                anchors.margins: 14
                label: "Protect with a password"
                description: checked ? "Using the key will need your account and this password."
                                     : "Your account alone will protect this key. You can add a password later."
                checked: true
            }
        }

        LockPasswordField {
            id: password
            Layout.fillWidth: true
            visible: protect.checked
            files: dialog.files
            label: "Key password"
        }
        Text {
            Layout.fillWidth: true
            visible: protect.checked
            wrapMode: Text.Wrap
            text: "You'll need it to open files locked with this key, so keep it somewhere safe. It's stretched with Argon2id at the stronger setting."
            color: Theme.text2
            font.pixelSize: 13
        }

        InfoCard {
            Layout.fillWidth: true
            visible: dialog.error.length > 0
            tone: "error"
            text: dialog.error
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 10
            Item { Layout.fillWidth: true }
            RvButton { secondary: true; implicitHeight: 46; text: "Cancel"; onClicked: dialog.close() }
            RvButton {
                implicitHeight: 46
                text: dialog.files.lockBusy ? "Creating…" : "Create key"
                enabled: !dialog.files.lockBusy && nameField.text.trim().length > 0 && (!protect.checked || password.acceptable)
                onClicked: {
                    dialog.error = ""
                    dialog.files.createKey(nameField.text, protect.checked ? password.text : "")
                }
            }
        }
    }
}
