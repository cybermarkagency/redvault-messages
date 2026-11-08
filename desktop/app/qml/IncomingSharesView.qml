// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// Files > Incoming: files people uploaded through your Contributor links,
// encrypted for you in their browser. Review each one, then add it to your
// files (moved, not copied) or reject it. Uploaders are anonymous, and the
// server can't scan what it can't read: the page says so.
Item {
    id: view
    required property SharesSession shares
    onVisibleChanged: shares.setIncomingVisible(visible)

    FileDialog {
        id: saveDialog
        property string targetId
        title: "Save a decrypted copy"
        fileMode: FileDialog.SaveFile
        currentFolder: StandardPaths.writableLocation(StandardPaths.DownloadLocation)
        onAccepted: view.shares.downloadIncoming(targetId, selectedFile)
    }
    ConfirmDialog {
        id: riskyDialog
        property string targetId
        property string targetName
        title: "Save a file that can run code?"
        message: "“" + targetName + "” is a kind of file that can run programs or scripts when opened. Someone anonymous uploaded it. Only open it if you know what it is."
        confirmText: "Save anyway"
        danger: true
        onConfirmed: view.save(targetId, targetName)
    }
    ConfirmDialog {
        id: rejectDialog
        property string targetId
        property string targetName
        title: "Reject this file?"
        message: "“" + targetName + "” is deleted for good and stops counting toward your storage. Whoever uploaded it isn't told."
        confirmText: "Reject"
        danger: true
        onConfirmed: view.shares.rejectIncoming(targetId)
    }
    RvDialog {
        id: acceptDialog
        property string targetId
        property string targetName
        title: "Add “" + targetName + "” to your files"
        width: 460
        contentItem: ColumnLayout {
            spacing: 12
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "It's moved, not copied: it stays encrypted, now with your own Files key, and keeps counting toward your storage."
                color: Theme.text2
            }
            RvChoice {
                id: folderChoice
                Layout.fillWidth: true
                textRole: "name"
                model: acceptDialog.opened ? view.shares.folderChoices() : []
                Accessible.name: "Folder"
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                RvButton { primary: false; text: "Cancel"; onClicked: acceptDialog.close() }
                RvButton {
                    text: "Add to Files"
                    onClicked: {
                        const choices = view.shares.folderChoices()
                        const chosen = choices[Math.max(0, folderChoice.currentIndex)]
                        view.shares.acceptIncoming(acceptDialog.targetId, chosen ? chosen.id : "")
                        acceptDialog.close()
                    }
                }
            }
        }
    }

    function save(id, name) {
        saveDialog.targetId = id
        saveDialog.selectedFile = saveDialog.currentFolder + "/" + encodeURIComponent(name)
        App.lock.expectExternalReturn()
        saveDialog.open()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        anchors.topMargin: 20
        spacing: 14

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            Text { text: "Incoming"; color: Theme.text; font.family: Theme.displayFamily; font.pixelSize: Theme.fontDisplay; font.weight: Font.Bold }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Files people uploaded through your links. Review them, then add them to your files or reject them."
                color: Theme.text2
            }
        }
        InfoCard {
            Layout.fillWidth: true
            tone: "warn"
            text: "Anyone with a Contributor link can upload, and uploaders are anonymous. These files are encrypted for you alone, so RedVault can't scan them for malware: only open what you expect, and scan saved files with your own antivirus."
        }
        Banner { Layout.fillWidth: true; visible: view.shares.error.length > 0; text: view.shares.error }
        InfoCard { Layout.fillWidth: true; visible: view.shares.notice.length > 0; tone: "ok"; text: view.shares.notice }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: view.shares.incomingCount === 0
            Item { Layout.fillHeight: true }
            Icon { Layout.alignment: Qt.AlignHCenter; name: "inbox"; size: 28; color: Theme.text3 }
            Text { Layout.alignment: Qt.AlignHCenter; text: "Nothing waiting for review."; color: Theme.text3 }
            Item { Layout.fillHeight: true }
        }

        ListView {
            objectName: "incomingList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: view.shares.incomingCount > 0
            clip: true
            spacing: 6
            model: view.shares.incoming
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: row
                required property var modelData
                width: ListView.view.width
                implicitHeight: 60
                radius: Theme.radiusControl
                color: hover.hovered ? Theme.selected : Theme.surface
                HoverHandler { id: hover }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 10
                    spacing: 10
                    Icon { name: row.modelData.risky ? "triangle-alert" : "file"; size: 18; color: row.modelData.risky ? Theme.warn : Theme.accent }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            text: row.modelData.name + (row.modelData.folder.length > 0 ? "  ·  from " + row.modelData.folder + "/" : "")
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.weight: Font.Medium
                        }
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: row.modelData.size + " · " + row.modelData.sent + " · through " + row.modelData.via
                            textFormat: Text.PlainText
                            color: Theme.text3
                            font.pixelSize: 12
                        }
                    }
                    IconButton {
                        iconName: "download"
                        tip: "Save a decrypted copy"
                        enabled: row.modelData.openable && !view.shares.busy
                        onClicked: {
                            if (view.shares.isRisky(row.modelData.name)) {
                                riskyDialog.targetId = row.modelData.id
                                riskyDialog.targetName = row.modelData.name
                                riskyDialog.open()
                            } else {
                                view.save(row.modelData.id, row.modelData.name)
                            }
                        }
                    }
                    RvButton {
                        secondary: true
                        text: "Add to Files"
                        enabled: row.modelData.openable && !view.shares.busy
                        onClicked: {
                            acceptDialog.targetId = row.modelData.id
                            acceptDialog.targetName = row.modelData.name
                            acceptDialog.open()
                        }
                    }
                    RvButton {
                        primary: false
                        danger: true
                        text: "Reject"
                        enabled: !view.shares.busy
                        onClicked: {
                            rejectDialog.targetId = row.modelData.id
                            rejectDialog.targetName = row.modelData.name
                            rejectDialog.open()
                        }
                    }
                }
            }
        }
    }
}
