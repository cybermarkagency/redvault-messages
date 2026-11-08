// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Inbox: messages and polls from the Redvault team. Plain text, and said so:
// unlike chats they aren't end-to-end encrypted (the server counts votes).
Item {
    id: root
    required property InboxSession inbox
    property bool active: false

    // Opening the page reads everything in it, like the web app.
    onActiveChanged: if (active && inbox) {
        inbox.refresh()
        inbox.markAllRead()
    }
    Connections {
        target: root.inbox
        function onItemsChanged() {
            if (root.active) Qt.callLater(root.inbox.markAllRead)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Flickable {
        id: scroller
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight + 64
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            x: Math.max(32, (scroller.width - 680) / 2)
            y: 32
            width: Math.min(680, scroller.width - 64)
            spacing: 14

            Text {
                text: "Inbox"
                color: Theme.text
                font.family: Theme.displayFamily
                font.pixelSize: 26
                font.weight: Font.Bold
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Icon {
                    Layout.alignment: Qt.AlignTop
                    name: "info"
                    size: 16
                    color: Theme.warn
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Messages from the Redvault team. These aren't end-to-end encrypted like your chats, so the server can read them, and your votes and replies."
                    color: Theme.text2
                    font.pixelSize: 13
                }
            }
            Banner {
                Layout.fillWidth: true
                text: root.inbox ? root.inbox.error : ""
            }
            Text {
                visible: root.inbox !== null && root.inbox.loaded && root.inbox.items.length === 0
                Layout.topMargin: 20
                text: "Nothing here yet."
                color: Theme.text3
            }

            Repeater {
                model: root.inbox ? root.inbox.items : []
                delegate: Rectangle {
                    id: card
                    required property var modelData
                    readonly property bool poll: modelData.kind === "poll"
                    readonly property bool voted: (modelData.myOptionId || "").length > 0
                    Layout.fillWidth: true
                    implicitHeight: cardColumn.implicitHeight + 32
                    radius: Theme.radiusCard
                    color: Theme.surface
                    border.color: Theme.border

                    Component.onCompleted: if (modelData.allowReplies && !modelData.repliesLoaded) root.inbox.loadReplies(modelData.id)

                    ColumnLayout {
                        id: cardColumn
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: card.poll ? "POLL" : "MESSAGE"
                                textFormat: Text.PlainText
                                color: Theme.text3
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.8
                            }
                            Text {
                                text: card.modelData.when
                                textFormat: Text.PlainText
                                color: Theme.text3
                                font.pixelSize: 12
                            }
                        }
                        TextEdit {
                            Layout.fillWidth: true
                            text: card.modelData.body
                            textFormat: TextEdit.PlainText
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            color: Theme.text
                            selectionColor: Theme.accent
                            selectedTextColor: Theme.accentContrast
                            font.pixelSize: 15
                        }

                        // Poll options: one vote, final.
                        Repeater {
                            model: card.poll ? card.modelData.options : []
                            delegate: AbstractButton {
                                id: option
                                required property var modelData
                                readonly property bool mine: option.modelData.id === card.modelData.myOptionId
                                Layout.fillWidth: true
                                implicitHeight: 42
                                enabled: !card.voted && !root.inbox.busy
                                hoverEnabled: true
                                onClicked: root.inbox.vote(card.modelData.id, option.modelData.id)
                                background: Rectangle {
                                    radius: Theme.radiusControl
                                    color: option.mine ? Theme.accentSoft : option.hovered && option.enabled ? Theme.surface3 : Theme.surface2
                                    border.color: option.mine ? Theme.accent : "transparent"
                                }
                                contentItem: RowLayout {
                                    Text {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 14
                                        text: option.modelData.label
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        color: Theme.text
                                    }
                                    Text {
                                        visible: option.mine
                                        Layout.rightMargin: 14
                                        text: "Your vote"
                                        color: Theme.accent
                                        font.pixelSize: 12
                                        font.weight: Font.DemiBold
                                    }
                                }
                            }
                        }
                        Text {
                            visible: card.poll && !card.voted
                            text: "Pick one. Votes can't be changed."
                            color: Theme.text3
                            font.pixelSize: 12
                        }

                        // Your replies (only the team sees them).
                        Repeater {
                            model: card.modelData.replies
                            delegate: Rectangle {
                                id: replyBubble
                                required property var modelData
                                Layout.alignment: Qt.AlignRight
                                Layout.maximumWidth: cardColumn.width * 0.8
                                implicitWidth: replyText.implicitWidth + 24
                                implicitHeight: replyText.implicitHeight + 16
                                radius: Theme.radiusCard
                                color: Theme.bubbleOut
                                Text {
                                    id: replyText
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    text: replyBubble.modelData.body
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    color: Theme.text
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: card.modelData.allowReplies
                            spacing: 8
                            RvTextField {
                                id: replyField
                                Layout.fillWidth: true
                                placeholderText: "Reply to the team"
                                maximumLength: 2000
                                enabled: !root.inbox.busy
                                onAccepted: root.inbox.reply(card.modelData.id, text)
                                Connections {
                                    target: root.inbox
                                    function onReplied(id) {
                                        if (id === card.modelData.id) replyField.text = ""
                                    }
                                }
                            }
                            RvButton {
                                text: "Send"
                                enabled: !root.inbox.busy && replyField.text.trim().length > 0
                                onClicked: root.inbox.reply(card.modelData.id, replyField.text)
                            }
                        }
                        Text {
                            visible: !card.modelData.allowReplies
                            text: "Replies are turned off for this one."
                            color: Theme.text3
                            font.pixelSize: 12
                        }
                    }
                }
            }
        }
    }
}
