// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Import a key made outside the app (64 hex characters, or base64 /
// base64url of 256 bits), checked as it's pasted. With "Protect with a
// password" on, the password is set on a second step.
LockDialog {
    id: dialog
    required property FilesSession files
    property string error
    property int step: 1
    // Re-checked when keys change too (a duplicate names the key you have).
    readonly property var check: files.keys, files.checkImport(keyText.text)
    readonly property bool keyOk: check.state === "valid"

    title: step === 1 ? "Import a key" : "Set a password for this key"
    iconName: "download"
    subtitle: step === 1 ? "Use a 256-bit key made outside the app. It's saved to your vault like any other file key."
                         : "Using “" + nameField.text.trim() + "” will need your account and this password."

    onOpened: {
        step = 1
        error = ""
        keyText.text = ""
        nameField.text = ""
        protect.checked = true
        keyText.forceActiveFocus()
    }
    onClosed: keyText.text = ""

    Connections {
        target: dialog.files
        function onLockDone(action, error) {
            if (action !== "importKey" || !dialog.opened) return
            if (error.length === 0) dialog.close()
            else dialog.error = error
        }
    }

    contentItem: ColumnLayout {
        spacing: 14

        // --- Step 1: the key ---
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.step === 1
            spacing: 8
            Text { text: "Key"; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 92
                TextArea {
                    id: keyText
                    wrapMode: TextEdit.Wrap
                    color: Theme.text
                    font.family: Theme.monoFamily
                    font.pixelSize: 13
                    placeholderText: "8d41 f07c 2b9e 55a3 …"
                    placeholderTextColor: Theme.text3
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.accentContrast
                    padding: 14
                    Accessible.name: "Key"
                    background: Rectangle {
                        radius: Theme.radiusControl
                        color: Theme.surface
                        border.width: keyText.activeFocus ? 2 : 1
                        border.color: keyText.activeFocus ? Theme.accent : Theme.border
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.StyledText
                text: "Paste 64 hex characters (for example from <font face=\"" + Theme.monoFamily + "\">openssl rand -hex 32</font>), or the base64 or base64url form. Spaces, line breaks and dashes are ignored."
                color: Theme.text2
                font.pixelSize: 13
            }
        }
        InfoCard {
            Layout.fillWidth: true
            visible: dialog.step === 1 && dialog.check.state === "valid"
            tone: "ok"
            text: "Valid 256-bit key · " + (dialog.check.format || "")
            detail: "Fingerprint <font face=\"" + Theme.monoFamily + "\">" + (dialog.check.fingerprint || "") + "</font>"
        }
        InfoCard {
            Layout.fillWidth: true
            visible: dialog.step === 1 && dialog.check.state === "invalid"
            tone: "error"
            text: dialog.check.message || ""
        }
        InfoCard {
            Layout.fillWidth: true
            visible: dialog.step === 1 && dialog.check.state === "duplicate"
            tone: "error"
            rich: true
            text: "You already have this key: <b>" + (dialog.check.name || "") + "</b> (" + (dialog.check.fingerprint || "") + ")."
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.step === 1
            spacing: 8
            Text { text: "Name"; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
            RvTextField {
                id: nameField
                Layout.fillWidth: true
                implicitHeight: 46
                placeholderText: "Offsite backup key"
                maximumLength: 80
            }
        }
        Rectangle {
            Layout.fillWidth: true
            visible: dialog.step === 1
            implicitHeight: protect.implicitHeight + 28
            radius: 14
            color: Theme.surface
            RvSwitch {
                id: protect
                anchors.fill: parent
                anchors.margins: 14
                label: "Protect with a password"
                description: checked ? "You'll set it on the next step. Weak passwords aren't allowed."
                                     : "Your account alone will protect this key. You can add a password later."
                checked: true
            }
        }

        // --- Step 2: its password ---
        Loader {
            id: passwordLoader
            Layout.fillWidth: true
            active: dialog.step === 2
            visible: active
            sourceComponent: LockPasswordField { files: dialog.files; label: "Key password" }
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
            RvButton {
                visible: dialog.step === 2
                primary: false
                implicitHeight: 46
                text: "Back"
                onClicked: { dialog.step = 1; dialog.error = "" }
            }
            Item { Layout.fillWidth: true }
            RvButton { secondary: true; implicitHeight: 46; text: "Cancel"; onClicked: dialog.close() }
            RvButton {
                implicitHeight: 46
                text: dialog.files.lockBusy ? "Importing…" : "Import key"
                enabled: !dialog.files.lockBusy && dialog.keyOk && nameField.text.trim().length > 0
                         && (dialog.step === 1 || (passwordLoader.item && passwordLoader.item.acceptable))
                onClicked: {
                    dialog.error = ""
                    if (dialog.step === 1 && protect.checked) {
                        dialog.step = 2
                        return
                    }
                    dialog.files.importKey(nameField.text, keyText.text, dialog.step === 2 ? passwordLoader.item.text : "")
                }
            }
        }
    }
}
