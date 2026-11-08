// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Files > Shared links: every public link you made, with what it allows,
// its state and its counts (numbers only: visitors are anonymous), and what
// you can do with it.
Item {
    id: view
    required property SharesSession shares

    ShareLinkEditDialog { id: editDialog; shares: view.shares }
    ConfirmDialog {
        id: revokeDialog
        property string linkId
        title: "Revoke this link?"
        message: "It stops working at once for everyone: no more opening, downloads or uploads. Files people already downloaded stay on their devices; nothing can take those back. This can't be undone."
        confirmText: "Revoke link"
        danger: true
        onConfirmed: view.shares.revoke(linkId)
    }
    ConfirmDialog {
        id: deleteDialog
        property string linkId
        property bool active
        title: "Delete this link?"
        message: (active ? "It stops working at once, and " : "It ") + "leaves your list. Files that came in through it stay in Incoming."
        confirmText: "Delete link"
        danger: true
        onConfirmed: view.shares.remove(linkId)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        anchors.topMargin: 20
        spacing: 14

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            Text { text: "Shared links"; color: Theme.text; font.family: Theme.displayFamily; font.pixelSize: Theme.fontDisplay; font.weight: Font.Bold }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Links anyone can open in a browser, without an account. Each one is separate: its own permission, password and expiry."
                color: Theme.text2
            }
        }
        Banner { Layout.fillWidth: true; visible: view.shares.error.length > 0; text: view.shares.error }
        InfoCard { Layout.fillWidth: true; visible: view.shares.notice.length > 0; tone: "ok"; text: view.shares.notice }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: view.shares.links.length === 0
            Item { Layout.fillHeight: true }
            Icon { Layout.alignment: Qt.AlignHCenter; name: "link"; size: 28; color: Theme.text3 }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: view.shares.loaded ? "No links yet. Use Share on a file or folder." : "Loading…"
                textFormat: Text.PlainText
                color: Theme.text3
            }
            Item { Layout.fillHeight: true }
        }

        ListView {
            id: list
            objectName: "shareLinksList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: view.shares.links.length > 0
            clip: true
            spacing: 12
            model: view.shares.links
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: card
                required property var modelData
                width: ListView.view.width
                implicitHeight: cardColumn.implicitHeight + 32
                radius: Theme.radiusCard
                color: Theme.surface
                border.color: Theme.borderStrong
                opacity: card.modelData.active ? 1 : 0.8

                ColumnLayout {
                    id: cardColumn
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Icon { name: card.modelData.kind === "folder" ? "folder" : "file"; size: 18; color: Theme.accent }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text { textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight; text: card.modelData.label; color: Theme.text; font.weight: Font.DemiBold }
                            Text {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: card.modelData.itemMissing ? "The shared item is no longer in your files"
                                      : (card.modelData.kind === "folder" ? "Folder · " : "File · ") + card.modelData.itemName
                                textFormat: Text.PlainText
                                color: Theme.text3
                                font.pixelSize: 12
                            }
                        }
                        Pill { text: card.modelData.permission === "contributor" ? "View, download, upload" : "View and download" }
                        Pill { text: card.modelData.statusText; tone: card.modelData.active ? "accent" : "danger" }
                        Pill { visible: card.modelData.hasPassword; text: "Password" }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 5
                        columnSpacing: 18
                        rowSpacing: 2
                        Repeater {
                            model: [
                                { k: "Created", v: card.modelData.created },
                                { k: "Expires", v: card.modelData.expires },
                                { k: "Opened", v: card.modelData.opens + "×" },
                                { k: "Downloads", v: card.modelData.downloads },
                                { k: "Uploads", v: card.modelData.permission === "contributor" || card.modelData.toReview > 0
                                      ? card.modelData.uploads + (card.modelData.uploadsInProgress > 0 ? " · " + card.modelData.uploadsInProgress + " in progress" : "")
                                        + (card.modelData.toReview > 0 ? " · " + card.modelData.toReview + " to review" : "")
                                      : "—" }
                            ]
                            delegate: ColumnLayout {
                                id: fact
                                required property var modelData
                                spacing: 1
                                Text { textFormat: Text.PlainText; text: fact.modelData.k; color: Theme.text3; font.pixelSize: 11 }
                                Text { textFormat: Text.PlainText; text: fact.modelData.v; color: Theme.text; font.pixelSize: 13 }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: (card.modelData.permission === "contributor"
                               ? "Anyone with this link can view and download files, and upload new files. They cannot delete or replace your files."
                               : "Anyone with this link can view and download files. Uploading and editing are disabled.")
                              + " Visitors are anonymous: counts only, no names or addresses."
                        textFormat: Text.PlainText
                        color: Theme.text3
                        font.pixelSize: 12
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        RvButton {
                            visible: card.modelData.active && card.modelData.url.length > 0
                            secondary: true
                            iconName: "copy"
                            text: "Copy link"
                            onClicked: view.shares.copyLink(card.modelData.id)
                        }
                        RvButton {
                            visible: card.modelData.status !== "revoked"
                            secondary: true
                            iconName: "sliders"
                            text: "Settings"
                            enabled: card.modelData.canManage && !view.shares.busy
                            onClicked: editDialog.openFor(card.modelData, "settings")
                        }
                        RvButton {
                            visible: card.modelData.status !== "revoked" && card.modelData.permission === "contributor"
                            secondary: true
                            iconName: "upload"
                            text: "Turn off uploads"
                            enabled: !view.shares.busy
                            onClicked: view.shares.turnOffUploads(card.modelData.id)
                        }
                        RvButton {
                            visible: card.modelData.status !== "revoked"
                            secondary: true
                            iconName: "key"
                            text: card.modelData.hasPassword ? "Password" : "Add password"
                            enabled: card.modelData.canManage && !view.shares.busy
                            onClicked: editDialog.openFor(card.modelData, "password")
                        }
                        RvButton {
                            visible: card.modelData.status !== "revoked"
                            secondary: true
                            iconName: "refresh"
                            text: "New link"
                            enabled: card.modelData.canManage && !view.shares.busy
                            onClicked: editDialog.openFor(card.modelData, "regenerate")
                        }
                        RvButton {
                            visible: card.modelData.status !== "revoked"
                            primary: false
                            danger: true
                            text: "Revoke"
                            enabled: !view.shares.busy
                            onClicked: {
                                revokeDialog.linkId = card.modelData.id
                                revokeDialog.open()
                            }
                        }
                        RvButton {
                            primary: false
                            danger: true
                            iconName: "trash"
                            text: "Delete"
                            enabled: !view.shares.busy
                            onClicked: {
                                deleteDialog.linkId = card.modelData.id
                                deleteDialog.active = card.modelData.active
                                deleteDialog.open()
                            }
                        }
                    }
                }
            }
        }
    }
}
