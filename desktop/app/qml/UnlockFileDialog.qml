// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Unlocks a locked file for this session, with a key or the file's own
// password. The file's key is never saved back to the server unlocked.
LockDialog {
    id: dialog
    required property FilesSession files
    property string targetId
    property string targetName
    property string lockKind
    property string lockKeyId
    property string lockKeyName
    property string lockFingerprint
    property string mode: "key"
    property string keyId
    property string error
    /// What to do once it's unlocked (e.g. "changeLock"), for whoever opened this.
    property string then
    signal unlocked(string id, string then)

    function openFor(selected, next) {
        targetId = selected.id
        targetName = selected.name
        lockKind = selected.lockKind
        lockKeyId = selected.lockKeyId || ""
        lockKeyName = selected.lockKeyName || ""
        lockFingerprint = selected.lockFingerprint || ""
        mode = lockKind === "password" ? "password" : "key"
        keyId = selected.lockKeyAvailable ? lockKeyId : (files.keys.length > 0 ? files.keys[0].id : "")
        then = next || ""
        open()
    }

    title: "Unlock " + targetName
    iconName: "lock"
    subtitleRich: lockKind === "password" ? "Locked with a password"
                  : "Locked with the key <b><font color=\"" + Theme.text + "\">" + lockKeyName + "</font></b> · <font face=\""
                    + Theme.monoFamily + "\" color=\"" + Theme.lockText + "\">" + lockFingerprint + "</font>"
    width: 580

    onOpened: {
        error = ""
        keyPassword.text = ""
        filePassword.text = ""
        if (mode === "password") filePassword.forceActiveFocus()
        else keyPassword.forceActiveFocus()
    }
    onClosed: { keyPassword.text = ""; filePassword.text = "" }

    Connections {
        target: dialog.files
        function onLockDone(action, error) {
            if (action !== "unlock" || !dialog.opened) return
            if (error.length === 0) {
                dialog.unlocked(dialog.targetId, dialog.then)
                dialog.close()
            } else {
                dialog.error = error
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 14

        // Use a key | Use a password
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 48
            radius: 14
            color: Theme.surface
            RowLayout {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4
                Repeater {
                    model: [{ value: "key", text: "Use a key" }, { value: "password", text: "Use a password" }]
                    delegate: AbstractButton {
                        id: tab
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        checked: dialog.mode === modelData.value
                        onClicked: { dialog.mode = modelData.value; dialog.error = "" }
                        Accessible.role: Accessible.PageTab
                        Accessible.name: modelData.text
                        background: Rectangle { radius: 10; color: tab.checked ? Theme.surface3 : "transparent" }
                        contentItem: Text {
                            textFormat: Text.PlainText
                            text: tab.modelData.text
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            color: tab.checked ? Theme.text : Theme.text2
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.mode === "key"
            spacing: 8
            Text { text: "Key"; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
            KeyPicker {
                Layout.fillWidth: true
                files: dialog.files
                keyId: dialog.keyId
                onKeyIdChanged: { dialog.keyId = keyId; dialog.error = "" }
            }
            Text {
                Layout.topMargin: 6
                visible: keyPassword.visible
                text: "Key password"
                color: Theme.text
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            RvTextField {
                id: keyPassword
                Layout.fillWidth: true
                // Re-read when keys change: a key opened this session needs no password.
                visible: dialog.files.keys, dialog.files.keyNeedsPassword(dialog.keyId)
                implicitHeight: 46
                echoMode: TextInput.Password
                Accessible.name: "Key password"
                onAccepted: unlockButton.clicked()
            }
            Text {
                Layout.fillWidth: true
                visible: !keyPassword.visible && dialog.keyId.length > 0
                wrapMode: Text.Wrap
                // A key with no password needs nothing more; a password-protected one was opened earlier this session.
                text: {
                    const key = dialog.files.keys.find(k => k.id === dialog.keyId)
                    return key && !key.passwordProtected ? "This key has no password, so nothing else is needed."
                                                         : "This key is already open this session."
                }
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: 13
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.mode === "password"
            spacing: 8
            Text { text: "File password"; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
            RvTextField {
                id: filePassword
                Layout.fillWidth: true
                implicitHeight: 46
                echoMode: TextInput.Password
                Accessible.name: "File password"
                onAccepted: unlockButton.clicked()
            }
        }

        InfoCard {
            Layout.fillWidth: true
            visible: dialog.error.length > 0
            tone: "error"
            text: dialog.error
            RvButton {
                visible: dialog.error.indexOf("newer version") >= 0
                secondary: true
                implicitHeight: 34
                text: "Update app"
                onClicked: Updater.check()
            }
        }

        InfoCard {
            Layout.fillWidth: true
            tone: "ok"
            iconName: "shield"
            text: ""
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Unlocks for this session only. The file's key is never saved back to the server unlocked, even if you rename or move the file. Until it's unlocked, it can't be downloaded, shared or given a link."
                color: Theme.text
                font.pixelSize: 13
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 10
            Item { Layout.fillWidth: true }
            RvButton { secondary: true; implicitHeight: 46; text: "Cancel"; onClicked: dialog.close() }
            RvButton {
                id: unlockButton
                implicitHeight: 46
                text: dialog.files.lockBusy ? "Unlocking…" : "Unlock"
                enabled: !dialog.files.lockBusy
                         && (dialog.mode === "password" ? filePassword.text.length > 0
                                                        : dialog.keyId.length > 0 && (!keyPassword.visible || keyPassword.text.length > 0))
                onClicked: {
                    if (!enabled) return
                    dialog.error = ""
                    dialog.files.unlockFile(dialog.targetId, dialog.mode, dialog.keyId, keyPassword.text, filePassword.text)
                }
            }
        }
    }
}
