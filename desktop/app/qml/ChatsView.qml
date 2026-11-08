// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Chats: the list (360px), the open chat, and its details panel (320px).
Item {
    id: root
    required property ChatSession session
    required property PeopleSession people
    required property AccountSession account
    property string filter: "all"   // "all", "unread", "favorites", "groups"
    property bool detailsOpen: true

    FriendsDialog {
        id: friendsDialog
        people: root.people
    }
    NewGroupDialog {
        id: newGroupDialog
        chats: root.session
        people: root.people
    }
    MessageRequestsDialog {
        id: requestsDialog
        chats: root.session
        people: root.people
    }

    function openFriends() { friendsDialog.open() }
    function openVerify() { thread.openVerify() }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // --- Chat list ---
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.listWidth
            color: Theme.panel

            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: Theme.border
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.rightMargin: 1
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.headerHeight
                    Layout.leftMargin: 20
                    Layout.rightMargin: 12
                    spacing: 4

                    Text {
                        Layout.fillWidth: true
                        text: "Chats"
                        color: Theme.text
                        font.family: Theme.displayFamily
                        font.pixelSize: Theme.fontDisplay
                        font.weight: Font.Bold
                        font.letterSpacing: -0.5
                    }
                    IconButton {
                        iconName: "user-plus"
                        tip: "Friends and requests"
                        badge: root.people.incoming.length
                        onClicked: friendsDialog.open()
                    }
                    IconButton {
                        iconName: "link"
                        tip: "Join a group with a link"
                        onClicked: if (App.groupInvites) App.groupInvites.startPaste()
                    }
                    IconButton {
                        iconName: "plus"
                        tip: "New group"
                        onClicked: newGroupDialog.open()
                    }
                }

                StoryTray {
                    Layout.fillWidth: true
                    visible: App.stories !== null
                    stories: App.stories
                    account: root.account
                }

                SearchField {
                    id: search
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    placeholderText: "Search chats"
                }

                Row {
                    Layout.leftMargin: 16
                    Layout.topMargin: 12
                    Layout.bottomMargin: 8
                    spacing: 6
                    Repeater {
                        model: [["all", "All"], ["unread", "Unread"], ["favorites", "Favorites"], ["groups", "Groups"]]
                        delegate: Chip {
                            required property var modelData
                            text: modelData[1]
                            checked: root.filter === modelData[0]
                            onClicked: root.filter = modelData[0]
                        }
                    }
                }

                // Someone who isn't a friend started a chat: shown apart until answered.
                ItemDelegate {
                    id: requestsRow
                    visible: root.session.messageRequests.length > 0
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    implicitHeight: 52
                    leftPadding: 12
                    rightPadding: 12
                    onClicked: requestsDialog.open()
                    background: Rectangle {
                        radius: Theme.radiusControl
                        color: requestsRow.hovered ? Theme.surface : "transparent"
                    }
                    contentItem: RowLayout {
                        spacing: 12
                        Rectangle {
                            implicitWidth: 44
                            implicitHeight: 44
                            radius: 22
                            color: Theme.surface2
                            Icon {
                                anchors.centerIn: parent
                                name: "user-plus"
                                size: 20
                                color: Theme.text2
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: "Message requests"
                            color: Theme.text
                            font.weight: Font.DemiBold
                        }
                        Rectangle {
                            implicitWidth: Math.max(20, requestCount.implicitWidth + 10)
                            implicitHeight: 20
                            radius: 10
                            color: Theme.accent
                            Text {
                                id: requestCount
                                anchors.centerIn: parent
                                text: root.session.messageRequests.length
                                textFormat: Text.PlainText
                                color: Theme.accentContrast
                                font.pixelSize: 11
                                font.weight: Font.Bold
                            }
                        }
                    }
                }

                ListView {
                    id: chatList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.session.chatList
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    // Rows that match the search and filter; the others take no space.
                    property int shown: 0

                    delegate: ItemDelegate {
                        id: row
                        required property string chatId
                        required property string title
                        required property string initial
                        required property bool isGroup
                        required property string preview
                        required property string timeText
                        required property int unread
                        required property string avatarUrl
                        required property string userId
                        required property string draft
                        required property string communityImage
                        readonly property bool selected: root.session.currentChatId === chatId
                        // An unsent draft shows in other chats' rows, like Signal.
                        readonly property bool showDraft: draft.length > 0 && !selected
                        readonly property bool favorite: userId.length > 0 && root.session.favorites.indexOf(userId) >= 0
                        readonly property bool muted: userId.length > 0 && root.session.muted.indexOf(userId) >= 0
                        readonly property bool typing: App.live !== null && App.live.typingRevision >= 0
                                                       && App.live.typingIn(chatId).length > 0
                        readonly property bool matches: {
                            const q = search.text.trim().toLowerCase()
                            if (q.length > 0 && title.toLowerCase().indexOf(q) < 0)
                                return false
                            if (root.filter === "unread") return unread > 0
                            if (root.filter === "groups") return isGroup
                            if (root.filter === "favorites") return favorite
                            return true
                        }

                        width: ListView.view.width
                        height: matches ? 72 : 0
                        visible: matches
                        leftInset: 8
                        rightInset: 8
                        leftPadding: 18
                        rightPadding: 18
                        onClicked: root.session.openChat(chatId)

                        background: Rectangle {
                            radius: Theme.radiusControl
                            color: row.selected ? Theme.selected : (row.hovered ? Qt.alpha(Theme.selected, 0.5) : "transparent")
                        }

                        contentItem: RowLayout {
                            spacing: 12

                            Avatar {
                                size: 44
                                initial: row.initial
                                group: row.isGroup
                                source: row.avatarUrl
                                communityImage: row.communityImage
                                tintKey: row.isGroup ? row.chatId : row.userId
                                online: !row.isGroup && App.live !== null && App.live.online.indexOf(row.userId) >= 0
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Text {
                                        Layout.fillWidth: true
                                        text: row.title
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        color: Theme.text
                                        font.pixelSize: 15
                                        font.weight: row.unread > 0 ? Font.DemiBold : Font.Medium
                                    }
                                    Icon {
                                        visible: row.muted
                                        name: "bell-off"
                                        size: 14
                                        color: Theme.text3
                                    }
                                    Text {
                                        text: row.timeText
                                        textFormat: Text.PlainText
                                        color: row.unread > 0 ? Theme.accent : Theme.text3
                                        font.pixelSize: Theme.fontSmall
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        visible: row.showDraft && !row.typing
                                        text: "Draft:"
                                        color: Theme.warn
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: row.typing ? "typing…" : row.showDraft ? row.draft : row.preview
                                        textFormat: Text.PlainText
                                        maximumLineCount: 1
                                        elide: Text.ElideRight
                                        color: row.typing ? Theme.accent : (row.unread > 0 ? Theme.text2 : Theme.text3)
                                        font.pixelSize: 13
                                    }
                                    // Device keys (preview): messages in this chat that came with per-device encryption.
                                    Rectangle {
                                        readonly property int count: App.deviceKeys ? (App.deviceKeys.unreadChats[row.chatId] || 0) : 0
                                        visible: count > 0
                                        implicitWidth: dkRow.implicitWidth + 12
                                        implicitHeight: 20
                                        radius: 10
                                        color: Theme.accentSoft
                                        Row {
                                            id: dkRow
                                            anchors.centerIn: parent
                                            spacing: 3
                                            Icon { name: "shield-check"; size: 12; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                                            Text { textFormat: Text.PlainText; text: parent.parent.count; color: Theme.accent; font.pixelSize: 11; font.weight: Font.Bold }
                                        }
                                        Accessible.name: count + " new device-key messages"
                                    }
                                    Rectangle {
                                        visible: row.unread > 0
                                        implicitWidth: Math.max(20, unreadText.implicitWidth + 10)
                                        implicitHeight: 20
                                        radius: 10
                                        color: row.muted ? Theme.surface3 : Theme.accent
                                        Text {
                                            id: unreadText
                                            anchors.centerIn: parent
                                            text: row.unread > 99 ? "99+" : row.unread
                                            textFormat: Text.PlainText
                                            color: row.muted ? Theme.text2 : Theme.accentContrast
                                            font.pixelSize: 11
                                            font.weight: Font.Bold
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Empty and loading states.
                    Column {
                        anchors.centerIn: parent
                        width: parent.width - 48
                        spacing: 10
                        visible: chatList.count === 0

                        BusyIndicator {
                            anchors.horizontalCenter: parent.horizontalCenter
                            running: !root.session.loaded
                            visible: running
                        }
                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            text: root.session.loaded ? "No chats yet. Add a friend to start one."
                                                      : "Decrypting your chats…"
                            textFormat: Text.PlainText
                            color: Theme.text2
                        }
                        RvButton {
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: root.session.loaded
                            iconName: "user-plus"
                            text: "Add a friend"
                            onClicked: friendsDialog.open()
                        }
                    }
                }

                // Refresh problems (the list keeps showing the last good state).
                Rectangle {
                    Layout.fillWidth: true
                    visible: root.session.error.length > 0
                    implicitHeight: sessionError.implicitHeight + 16
                    color: Theme.dangerSoft
                    Text {
                        id: sessionError
                        anchors.fill: parent
                        anchors.margins: 8
                        text: root.session.error
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.danger
                        font.pixelSize: Theme.fontSmall
                    }
                }
            }
        }

        // --- Open chat ---
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ThreadView {
                id: thread
                anchors.fill: parent
                visible: root.session.currentChatId.length > 0
                session: root.session
                people: root.people
                account: root.account
                detailsOpen: root.detailsOpen
                onToggleDetails: root.detailsOpen = !root.detailsOpen
            }

            Column {
                anchors.centerIn: parent
                spacing: 10
                visible: root.session.currentChatId.length === 0

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 64
                    height: 64
                    radius: 20
                    color: Theme.accentSoft
                    border.color: Theme.accentBorder
                    Icon {
                        anchors.centerIn: parent
                        name: "chat"
                        size: 30
                        color: Theme.accent
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Select a chat"
                    color: Theme.text
                    font.family: Theme.displayFamily
                    font.pixelSize: 22
                    font.weight: Font.Bold
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Messages are end-to-end encrypted and decrypted only on this computer."
                    color: Theme.text3
                    font.pixelSize: 13
                }
            }
        }

        // --- Details ---
        ChatDetails {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.detailsWidth
            visible: root.detailsOpen && root.session.currentChatId.length > 0 && root.width > 1100
            session: root.session
            people: root.people
            onFileRequested: (messageId, name) => thread.showFileMenu(messageId, name)
            onVerifyRequested: thread.openVerify()
            onGroupInfoRequested: thread.openGroupInfo()
            onCallRequested: (kind) => thread.call(kind)
        }
    }
}
