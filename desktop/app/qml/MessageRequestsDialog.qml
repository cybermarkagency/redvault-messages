// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Chats started by people who aren't your friends: read them here without
// anything being sent back, then accept, ignore or block.
RvDialog {
    id: dialog
    required property ChatSession chats
    required property PeopleSession people
    title: "Message requests"
    width: 520

    contentItem: ColumnLayout {
        spacing: 12

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "These people aren't your friends. Reading a request sends nothing back: they don't learn you've seen it unless you accept. Ignoring hides it without telling them."
            color: Theme.text3
            font.pixelSize: 13
        }

        Text {
            visible: dialog.chats.messageRequests.length === 0
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.bottomMargin: 8
            horizontalAlignment: Text.AlignHCenter
            text: "No message requests."
            color: Theme.text3
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(440, requestsColumn.implicitHeight)
            visible: dialog.chats.messageRequests.length > 0
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                id: requestsColumn
                width: parent.width
                spacing: 10

                Repeater {
                    model: dialog.chats.messageRequests
                    delegate: Rectangle {
                        id: card
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: cardColumn.implicitHeight + 24
                        radius: Theme.radiusCard
                        color: Theme.surface2

                        ColumnLayout {
                            id: cardColumn
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 8

                            RowLayout {
                                spacing: 10
                                Avatar {
                                    size: 36
                                    initial: card.modelData.initial
                                    source: card.modelData.avatarUrl
                                    tintKey: card.modelData.userId
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: "@" + card.modelData.handle
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: Theme.text
                                    font.family: Theme.monoFamily
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    text: card.modelData.time
                                    textFormat: Text.PlainText
                                    color: Theme.text3
                                    font.pixelSize: 12
                                }
                            }

                            // Their last few messages, as plain text.
                            Repeater {
                                model: card.modelData.messages.slice(-3)
                                delegate: Text {
                                    required property string modelData
                                    Layout.fillWidth: true
                                    text: modelData
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 4
                                    elide: Text.ElideRight
                                    color: Theme.text2
                                    font.pixelSize: 13
                                }
                            }

                            RowLayout {
                                Layout.alignment: Qt.AlignRight
                                spacing: 6
                                RvButton {
                                    primary: false
                                    danger: true
                                    text: "Block"
                                    implicitHeight: 34
                                    onClicked: {
                                        dialog.people.block(card.modelData.userId)
                                        dialog.chats.respondToRequest(card.modelData.chatId, false)
                                    }
                                }
                                RvButton {
                                    primary: false
                                    text: "Ignore"
                                    implicitHeight: 34
                                    onClicked: dialog.chats.respondToRequest(card.modelData.chatId, false)
                                }
                                RvButton {
                                    text: "Accept"
                                    implicitHeight: 34
                                    onClicked: {
                                        dialog.chats.respondToRequest(card.modelData.chatId, true)
                                        dialog.close()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
