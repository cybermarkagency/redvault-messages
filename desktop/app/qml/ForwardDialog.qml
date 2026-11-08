// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Forward one or more messages to other chats: pick them (search by name),
// then Forward. Files are encrypted and uploaded again for each chat. Also
// forwards a channel's post (openForChannelPost), as a post from that channel.
RvDialog {
    id: dialog
    required property ChatSession chats
    // Sending files is Pro-only; forwarding a file is sending it again.
    property bool canSendFiles: true
    property var messageIds: []
    /// Forwarding a channel post instead: {text, channelId, postId, channelName}.
    property var channelPost: null
    property bool hasFile: false
    property var targets: []
    property var picked: ({})
    readonly property int pickedCount: Object.keys(picked).length
    property string error: ""
    property bool busy: false
    /// Everything was forwarded.
    signal forwarded()
    title: channelPost ? "Forward post" : messageIds.length > 1 ? "Forward " + messageIds.length + " messages" : "Forward message"
    width: 460

    function openFor(ids, containsFile) {
        dialog.channelPost = null
        dialog.messageIds = ids
        dialog.hasFile = containsFile
        dialog.start()
    }
    function openForChannelPost(post) {
        dialog.channelPost = post
        dialog.messageIds = []
        dialog.hasFile = false
        dialog.start()
    }
    function start() {
        dialog.targets = dialog.chats.forwardTargets()
        dialog.picked = ({})
        dialog.error = ""
        dialog.busy = false
        filter.text = ""
        open()
        filter.forceActiveFocus()
    }
    function toggle(chatId) {
        const next = Object.assign({}, dialog.picked)
        if (next[chatId]) delete next[chatId]
        else next[chatId] = true
        dialog.picked = next
    }

    Connections {
        target: dialog.chats
        function onForwardDone(sent, error) {
            // Another dialog's forward (the chat's, or a channel page's).
            if (!dialog.busy) return
            dialog.busy = false
            if (error.length > 0) dialog.error = error
            else {
                dialog.close()
                dialog.forwarded()
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 10

        SearchField {
            id: filter
            Layout.fillWidth: true
            placeholderText: "Search chats"
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.preferredHeight: 320
            clip: true
            model: dialog.targets.filter(t => filter.text.trim().length === 0
                                         || t.title.toLowerCase().indexOf(filter.text.trim().toLowerCase()) >= 0)
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                id: row
                required property var modelData
                readonly property bool on: dialog.picked[row.modelData.chatId] === true
                width: list.width
                implicitHeight: 52
                leftPadding: 8
                rightPadding: 8
                onClicked: dialog.toggle(row.modelData.chatId)
                background: Rectangle {
                    radius: Theme.radiusControl
                    color: row.on ? Theme.accentSoft : row.hovered ? Theme.surface2 : "transparent"
                }
                contentItem: RowLayout {
                    spacing: 10
                    Avatar {
                        size: 34
                        initial: row.modelData.initial
                        group: row.modelData.isGroup
                        source: row.modelData.avatarUrl
                        communityImage: row.modelData.communityImage || ""
                        tintKey: row.modelData.isGroup ? row.modelData.chatId : row.modelData.userId
                    }
                    Text {
                        Layout.fillWidth: true
                        text: row.modelData.title
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text
                    }
                    Rectangle {
                        implicitWidth: 20
                        implicitHeight: 20
                        radius: 10
                        color: row.on ? Theme.accent : "transparent"
                        border.color: row.on ? Theme.accent : Theme.borderStrong
                        border.width: 2
                        Icon {
                            anchors.centerIn: parent
                            visible: row.on
                            name: "check"
                            size: 13
                            color: Theme.accentContrast
                        }
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text: dialog.targets.length === 0 ? "You don't have a chat to forward to yet." : "No chats match."
                textFormat: Text.PlainText
                color: Theme.text3
            }
        }

        Text {
            Layout.fillWidth: true
            visible: dialog.channelPost !== null
            wrapMode: Text.Wrap
            text: dialog.channelPost ? "It's sent end-to-end encrypted, as a post from " + dialog.channelPost.channelName
                                       + ". Each copy that's seen counts as a view." : ""
            textFormat: Text.PlainText
            color: Theme.text3
            font.pixelSize: 12
        }
        Text {
            Layout.fillWidth: true
            visible: dialog.hasFile
            wrapMode: Text.Wrap
            text: dialog.canSendFiles ? "Files are encrypted again on this computer and uploaded once for each chat."
                                      : "Forwarding a file means sending it again, which needs Pro."
            textFormat: Text.PlainText
            color: dialog.canSendFiles ? Theme.text3 : Theme.warn
            font.pixelSize: 12
        }
        Banner {
            Layout.fillWidth: true
            text: dialog.error
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
                text: dialog.busy ? "Forwarding…"
                    : dialog.pickedCount > 1 ? "Forward to " + dialog.pickedCount + " chats" : "Forward"
                enabled: !dialog.busy && dialog.pickedCount > 0 && (!dialog.hasFile || dialog.canSendFiles)
                onClicked: {
                    dialog.busy = true
                    dialog.error = ""
                    const chatIds = Object.keys(dialog.picked)
                    if (dialog.channelPost) {
                        dialog.chats.forwardChannelPost(dialog.channelPost.text, dialog.channelPost.channelId,
                                                        dialog.channelPost.postId, dialog.channelPost.channelName, chatIds)
                    } else {
                        dialog.chats.forwardMessages(dialog.messageIds, chatIds)
                    }
                }
            }
        }
    }
}
