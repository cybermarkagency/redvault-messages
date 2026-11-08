// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Files › Encryption keys: the person's own 256-bit keys for locking files
// (kept in the password vault), and the selected key's details: its
// fingerprint, its password, an export for backup, and deleting it.
Item {
    id: root
    required property FilesSession files

    KeyCreateDialog { id: createDialog; files: root.files }
    KeyImportDialog { id: importDialog; files: root.files }

    // Rename
    LockDialog {
        id: renameDialog
        title: "Rename key"
        iconName: "key"
        showIcon: false
        width: 460
        onOpened: { renameField.text = root.files.selectedKey.name || ""; renameField.forceActiveFocus() }
        contentItem: ColumnLayout {
            spacing: 14
            RvTextField { id: renameField; Layout.fillWidth: true; implicitHeight: 46; maximumLength: 80; onAccepted: renameOk.clicked() }
            RowLayout {
                Item { Layout.fillWidth: true }
                RvButton { secondary: true; implicitHeight: 44; text: "Cancel"; onClicked: renameDialog.close() }
                RvButton {
                    id: renameOk
                    implicitHeight: 44
                    text: "Rename"
                    enabled: renameField.text.trim().length > 0
                    onClicked: { root.files.renameKey(root.files.selectedKeyId, renameField.text); renameDialog.close() }
                }
            }
        }
    }

    // Set, change or remove the key's password.
    LockDialog {
        id: passwordDialog
        property string action: "change"   // "set", "change" or "remove"
        property string error
        title: action === "set" ? "Set a password" : action === "remove" ? "Remove the password" : "Change the password"
        iconName: "lock"
        subtitle: action === "remove"
                  ? "Your account alone will protect “" + (root.files.selectedKey.name || "") + "”. Files locked with it aren't re-encrypted."
                  : "Using “" + (root.files.selectedKey.name || "") + "” will need your account and this password. Files locked with it aren't re-encrypted."
        onOpened: { error = ""; currentPassword.text = ""; if (currentPassword.visible) currentPassword.forceActiveFocus() }
        onClosed: currentPassword.text = ""
        Connections {
            target: root.files
            function onLockDone(action, error) {
                if (action !== "keyPassword" || !passwordDialog.opened) return
                if (error.length === 0) passwordDialog.close()
                else passwordDialog.error = error
            }
        }
        contentItem: ColumnLayout {
            spacing: 14
            ColumnLayout {
                Layout.fillWidth: true
                visible: passwordDialog.action !== "set"
                spacing: 8
                Text { text: "Current password"; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
                RvTextField { id: currentPassword; Layout.fillWidth: true; implicitHeight: 46; echoMode: TextInput.Password; Accessible.name: "Current password" }
            }
            Loader {
                id: newPassword
                Layout.fillWidth: true
                active: passwordDialog.opened && passwordDialog.action !== "remove"
                visible: active
                sourceComponent: LockPasswordField { files: root.files; label: "New password" }
            }
            InfoCard { Layout.fillWidth: true; visible: passwordDialog.error.length > 0; tone: "error"; text: passwordDialog.error }
            RowLayout {
                Item { Layout.fillWidth: true }
                RvButton { secondary: true; implicitHeight: 46; text: "Cancel"; onClicked: passwordDialog.close() }
                RvButton {
                    implicitHeight: 46
                    danger: passwordDialog.action === "remove"
                    text: root.files.lockBusy ? "Saving…" : passwordDialog.action === "remove" ? "Remove password" : "Save password"
                    enabled: !root.files.lockBusy && (passwordDialog.action === "set" || currentPassword.text.length > 0)
                             && (passwordDialog.action === "remove" || (newPassword.item && newPassword.item.acceptable))
                    onClicked: {
                        passwordDialog.error = ""
                        root.files.setKeyPassword(root.files.selectedKeyId, currentPassword.text,
                                                  passwordDialog.action === "remove" ? "" : newPassword.item.text)
                    }
                }
            }
        }
    }

    // The key's password, to reveal its export.
    LockDialog {
        id: exportDialog
        property string error
        title: "Show the key"
        iconName: "eye"
        subtitle: "Enter the password of “" + (root.files.selectedKey.name || "") + "” to show it for backup."
        width: 480
        onOpened: { error = ""; exportPassword.text = ""; exportPassword.forceActiveFocus() }
        onClosed: exportPassword.text = ""
        Connections {
            target: root.files
            function onLockDone(action, error) {
                if (action !== "export" || !exportDialog.opened) return
                if (error.length === 0) exportDialog.close()
                else exportDialog.error = error
            }
        }
        contentItem: ColumnLayout {
            spacing: 14
            RvTextField { id: exportPassword; Layout.fillWidth: true; implicitHeight: 46; echoMode: TextInput.Password; Accessible.name: "Key password"; onAccepted: exportOk.clicked() }
            InfoCard { Layout.fillWidth: true; visible: exportDialog.error.length > 0; tone: "error"; text: exportDialog.error }
            RowLayout {
                Item { Layout.fillWidth: true }
                RvButton { secondary: true; implicitHeight: 44; text: "Cancel"; onClicked: exportDialog.close() }
                RvButton {
                    id: exportOk
                    implicitHeight: 44
                    text: root.files.lockBusy ? "Opening…" : "Show key"
                    enabled: !root.files.lockBusy && exportPassword.text.length > 0
                    onClicked: { exportDialog.error = ""; root.files.revealKeyExport(root.files.selectedKeyId, exportPassword.text) }
                }
            }
        }
    }

    // Deleting a key.
    LockDialog {
        id: deleteDialog
        title: "Delete this key?"
        iconName: "trash"
        showIcon: false
        width: 500
        subtitle: {
            const n = root.files.selectedKey.files || 0
            return n > 0 ? n + (n === 1 ? " file is" : " files are") + " locked with “" + (root.files.selectedKey.name || "")
                           + "”. After you delete it they can't be opened unless you have an export."
                         : "No files are locked with “" + (root.files.selectedKey.name || "") + "”."
        }
        contentItem: RowLayout {
            Item { Layout.fillWidth: true }
            RvButton { secondary: true; implicitHeight: 44; text: "Cancel"; onClicked: deleteDialog.close() }
            RvButton {
                implicitHeight: 44
                danger: true
                text: "Delete key"
                onClicked: { root.files.deleteKey(root.files.selectedKeyId); deleteDialog.close() }
            }
        }
    }

    RowLayout {
    anchors.fill: parent
    spacing: 0

    // --- The key table ---
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 28
            anchors.topMargin: 22
            spacing: 18

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    spacing: 6
                    Text {
                        text: "Encryption keys"
                        color: Theme.text
                        font.family: Theme.displayFamily
                        font.pixelSize: 28
                        font.weight: Font.Bold
                        font.letterSpacing: -0.5
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 440
                        wrapMode: Text.Wrap
                        text: "Your own 256-bit keys for locking files. They're saved as file-key items in your password vault, so they stay encrypted and sync to your other devices."
                        color: Theme.text2
                        font.pixelSize: 14
                        lineHeight: 1.2
                    }
                }
                RvButton {
                    Layout.alignment: Qt.AlignTop
                    secondary: true
                    iconName: "download"
                    text: "Import key"
                    onClicked: importDialog.open()
                }
                RvButton {
                    Layout.alignment: Qt.AlignTop
                    iconName: "plus"
                    text: "Create key"
                    onClicked: createDialog.open()
                }
            }

            Banner {
                Layout.fillWidth: true
                text: root.files.error
                MouseArea { anchors.fill: parent; onClicked: root.files.clearMessages() }
            }
            Banner {
                Layout.fillWidth: true
                error: false
                text: root.files.notice
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 39 + Math.max(1, keyTable.count) * 60
                Layout.maximumHeight: 39 + 60 * 6
                radius: Theme.radiusCard
                color: "transparent"
                border.color: Theme.borderStrong
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        spacing: 12
                        Text { Layout.fillWidth: true; Layout.preferredWidth: 1; text: "KEY"; color: Theme.text3; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.8 }
                        Text { Layout.preferredWidth: 190; text: "FINGERPRINT"; color: Theme.text3; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.8 }
                        Text { Layout.preferredWidth: 150; text: "PROTECTED BY"; color: Theme.text3; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.8 }
                        Text { Layout.preferredWidth: 50; text: "FILES"; color: Theme.text3; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.8 }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                    ListView {
                        id: keyTable
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: root.files.keys
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}
                        delegate: ItemDelegate {
                            id: keyRow
                            required property var modelData
                            width: ListView.view.width
                            height: 60
                            leftPadding: 16
                            rightPadding: 16
                            onClicked: root.files.selectKey(modelData.id)
                            Accessible.name: modelData.name
                            background: Rectangle {
                                color: root.files.selectedKeyId === keyRow.modelData.id ? Theme.selected
                                     : keyRow.hovered ? Qt.alpha(Theme.selected, 0.5) : "transparent"
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
                            }
                            contentItem: RowLayout {
                                spacing: 12
                                Rectangle {
                                    implicitWidth: 34; implicitHeight: 34; radius: 9
                                    color: Theme.lockBg
                                    Icon { anchors.centerIn: parent; name: "key"; size: 16; color: Theme.lockText }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    spacing: 1
                                    Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: keyRow.modelData.name; color: Theme.text; font.pixelSize: 15; font.weight: Font.DemiBold; elide: Text.ElideRight }
                                    Text { textFormat: Text.PlainText; text: keyRow.modelData.createdText; color: Theme.text3; font.pixelSize: 12 }
                                }
                                Text { textFormat: Text.PlainText; Layout.preferredWidth: 190; text: keyRow.modelData.fingerprint; color: Theme.text; font.family: Theme.monoFamily; font.pixelSize: 12 }
                                Text {
                                    Layout.preferredWidth: 150
                                    text: keyRow.modelData.protectedBy
                                    textFormat: Text.PlainText
                                    color: keyRow.modelData.passwordProtected ? Theme.accent : Theme.warn
                                    font.pixelSize: 13
                                }
                                Text { textFormat: Text.PlainText; Layout.preferredWidth: 50; text: keyRow.modelData.files; color: Theme.text2; font.family: Theme.monoFamily; font.pixelSize: 12 }
                            }
                        }
                        Column {
                            anchors.centerIn: parent
                            width: parent.width - 80
                            visible: keyTable.count === 0
                            spacing: 8
                            BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: !root.files.keysLoaded; visible: running }
                            Text {
                                width: parent.width
                                visible: root.files.keysLoaded
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                                text: "No keys yet. Create one, or import a key you made elsewhere."
                                color: Theme.text3
                            }
                        }
                    }
                }
            }

            // What keys are, in three cards.
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Repeater {
                    model: [
                        { title: "Fingerprints", text: "The short code is the same on every device, so you can check you're using the right key. It doesn't reveal the key." },
                        { title: "Key passwords", text: "With a password, using the key needs your account and that password (stretched with Argon2id). Changing it is instant." },
                        { title: "Not on the Passwords page", text: "File keys live in your vault but stay out of your password list, so they're never flagged as weak or broken passwords." }
                    ]
                    delegate: Rectangle {
                        id: explainer
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.alignment: Qt.AlignTop
                        implicitHeight: explainerColumn.implicitHeight + 28
                        radius: Theme.radiusCard
                        color: Theme.surface
                        ColumnLayout {
                            id: explainerColumn
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 6
                            Text { textFormat: Text.PlainText; text: explainer.modelData.title; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
                            Text { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; text: explainer.modelData.text; color: Theme.text2; font.pixelSize: 13; lineHeight: 1.15 }
                        }
                    }
                }
            }
            Item { Layout.fillHeight: true }
        }
    }

    // --- The selected key ---
    Rectangle {
        id: details
        Layout.fillHeight: true
        Layout.preferredWidth: 360
        color: Theme.panel
        readonly property var k: root.files.selectedKey
        Rectangle { width: 1; height: parent.height; color: Theme.border }

        Text {
            anchors.centerIn: parent
            width: parent.width - 48
            visible: details.k.id === undefined
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: "Select a key to see its fingerprint, its password and its backup."
            color: Theme.text3
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            anchors.leftMargin: 21
            visible: details.k.id !== undefined
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Rectangle {
                    implicitWidth: 48; implicitHeight: 48; radius: 12
                    color: Theme.lockBg
                    Icon { anchors.centerIn: parent; name: "key"; size: 22; color: Theme.lockText }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: details.k.name || ""
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.family: Theme.displayFamily
                        font.pixelSize: 22
                        font.weight: Font.Bold
                    }
                    Text { textFormat: Text.PlainText; text: (details.k.createdText || "") + " · " + (details.k.filesText || ""); color: Theme.text3; font.pixelSize: 12 }
                }
                RvButton { secondary: true; implicitHeight: 36; text: "Rename"; onClicked: renameDialog.open() }
            }

            // Fingerprint
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: fpColumn.implicitHeight + 32
                radius: Theme.radiusCard
                color: Theme.lockSurface
                border.color: Theme.lockBorder
                ColumnLayout {
                    id: fpColumn
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 6
                    Text { text: "Fingerprint"; color: Theme.lockText; font.pixelSize: 12 }
                    Text {
                        text: details.k.fingerprint || ""
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.family: Theme.monoFamily
                        font.pixelSize: 19
                        font.weight: Font.Medium
                        font.letterSpacing: 0.6
                    }
                    Text { text: "Same on every device. Doesn't reveal the key."; color: Theme.lockText; font.pixelSize: 12 }
                }
            }

            // Key password
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: pwColumn.implicitHeight + 32
                radius: Theme.radiusCard
                color: Theme.surface
                ColumnLayout {
                    id: pwColumn
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 8
                    RowLayout {
                        Text { Layout.fillWidth: true; text: "Key password"; color: Theme.text; font.pixelSize: 14; font.weight: Font.DemiBold }
                        Text {
                            text: details.k.passwordProtected ? "On" : "Off"
                            textFormat: Text.PlainText
                            color: details.k.passwordProtected ? Theme.accent : Theme.warn
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: details.k.passwordProtected
                              ? "Using this key needs your account and its password. The password is stretched with Argon2id at the stronger setting."
                              : "Your account alone protects this key. Add a password so a signed-in device isn’t enough to open locked files."
                        textFormat: Text.PlainText
                        color: Theme.text2
                        font.pixelSize: 13
                        lineHeight: 1.15
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        RvButton {
                            Layout.fillWidth: true
                            secondary: true
                            implicitHeight: 38
                            text: details.k.passwordProtected ? "Change password" : "Set a password"
                            onClicked: { passwordDialog.action = details.k.passwordProtected ? "change" : "set"; passwordDialog.open() }
                        }
                        RvButton {
                            Layout.fillWidth: true
                            visible: details.k.passwordProtected === true
                            secondary: true
                            implicitHeight: 38
                            text: "Remove password"
                            onClicked: { passwordDialog.action = "remove"; passwordDialog.open() }
                        }
                    }
                    Text { text: "Instant: locked files aren't re-encrypted."; color: Theme.accent; font.pixelSize: 12 }
                }
            }

            // Export for backup
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: exColumn.implicitHeight + 28
                radius: Theme.radiusCard
                color: Theme.surface
                ColumnLayout {
                    id: exColumn
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10
                    RowLayout {
                        Text { Layout.fillWidth: true; text: "Export for backup"; color: Theme.text; font.pixelSize: 14; font.weight: Font.DemiBold }
                        IconButton {
                            filled: true
                            implicitWidth: 34; implicitHeight: 34; iconSize: 16
                            iconName: root.files.exportedKey.length > 0 ? "eye-off" : "eye"
                            tip: root.files.exportedKey.length > 0 ? "Hide the key" : "Show the key"
                            onClicked: {
                                if (root.files.exportedKey.length > 0) root.files.hideKeyExport()
                                else if (root.files.keyNeedsPassword(root.files.selectedKeyId)) exportDialog.open()
                                else root.files.revealKeyExport(root.files.selectedKeyId, "")
                            }
                        }
                        IconButton {
                            filled: true
                            implicitWidth: 34; implicitHeight: 34; iconSize: 16
                            iconName: "copy"
                            enabled: root.files.exportedKey.length > 0
                            tip: "Copy the key (clears from the clipboard in 30 seconds)"
                            onClicked: root.files.copyKeyExport()
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: exportText.implicitHeight + 24
                        radius: 12
                        color: Theme.bg
                        Text {
                            id: exportText
                            anchors.fill: parent
                            anchors.margins: 12
                            wrapMode: Text.Wrap
                            text: root.files.exportedKey.length > 0 ? root.files.exportedKey
                                  : "•••••••• •••••••• •••••••• •••••••• •••••••• •••••••• •••••••• ••••••••"
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.family: Theme.monoFamily
                            font.pixelSize: 13
                            lineHeight: 1.3
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Icon { Layout.alignment: Qt.AlignTop; name: "triangle-alert"; size: 14; color: Theme.warn }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: "This is the raw key: 64 hex characters. The key password doesn't protect it, so store it offline."
                            color: Theme.text
                            font.pixelSize: 12
                        }
                    }
                }
            }

            Item { Layout.fillHeight: true }

            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: (details.k.files || 0) > 0
                      ? details.k.files + (details.k.files === 1 ? " file is" : " files are") + " locked with this key. After you delete it they can’t be opened unless you have an export."
                      : "No files are locked with this key."
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: 12
            }
            RvButton {
                Layout.fillWidth: true
                primary: false
                danger: true
                iconName: "trash"
                text: "Delete key"
                onClicked: deleteDialog.open()
            }
        }
    }
    }
}
