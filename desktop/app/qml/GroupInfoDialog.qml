pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The open group: rename, members and roles, add friends, the invite link, leave.
// Owners and admins manage members; the server enforces the same rules.
RvDialog {
    id: dialog
    required property ChatSession chats
    required property PeopleSession people
    title: "Group info"
    width: 520

    readonly property bool canManage: chats.currentMyRole === "owner" || chats.currentMyRole === "admin"
    readonly property bool isOwner: chats.currentMyRole === "owner"
    property string error: ""
    property bool adding: false
    property bool confirmLeave: false

    onOpened: {
        nameField.text = chats.currentTitle
        error = ""
        adding = false
        confirmLeave = false
        people.refresh()
        if (App.groupInvites) App.groupInvites.load(chats.currentChatId)
    }

    Connections {
        target: dialog.chats
        function onGroupActionDone(message) {
            if (!dialog.visible)
                return
            dialog.error = message
            if (message.length === 0) {
                dialog.adding = false
                picker.reset()
            }
        }
        function onCurrentChatChanged() {
            // Left the group (or were removed): nothing to show any more.
            if (dialog.visible && !dialog.chats.currentIsGroup)
                dialog.close()
        }
    }

    contentItem: ColumnLayout {
        spacing: 12

        // Name
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.canManage
            RvTextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: "Group name"
                maximumLength: 100
                enabled: !dialog.chats.groupBusy
                onAccepted: renameButton.clicked()
            }
            RvButton {
                id: renameButton
                text: "Rename"
                enabled: !dialog.chats.groupBusy && nameField.text.trim().length > 0
                         && nameField.text.trim() !== dialog.chats.currentTitle
                onClicked: dialog.chats.renameGroup(nameField.text)
            }
        }

        Text {
            text: dialog.chats.currentSubtitle
            textFormat: Text.PlainText
            color: Theme.text2
            font.pixelSize: Theme.fontSmall
            font.weight: Font.DemiBold
        }

        // Members
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(260, memberList.contentHeight)
            clip: true

            ListView {
                id: memberList
                model: dialog.chats.currentMembers
                spacing: 4
                delegate: RowLayout {
                    id: member
                    required property var modelData
                    width: ListView.view.width
                    spacing: 10

                    Avatar {
                        size: 30
                        initial: member.modelData.handle.charAt(0).toUpperCase()
                        source: member.modelData.avatarUrl
                    }
                    Text {
                        Layout.fillWidth: true
                        text: member.modelData.handle + (member.modelData.isSelf ? " (you)" : "")
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text
                    }
                    Text {
                        text: member.modelData.role === "owner" ? "Owner" : member.modelData.role === "admin" ? "Admin" : ""
                        textFormat: Text.PlainText
                        color: Theme.accent
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.DemiBold
                    }
                    // Only the owner changes roles; owners and admins remove members (never the owner).
                    RvButton {
                        visible: dialog.isOwner && !member.modelData.isSelf
                        primary: false
                        implicitHeight: 28
                        text: member.modelData.role === "admin" ? "Make member" : "Make admin"
                        enabled: !dialog.chats.groupBusy
                        onClicked: dialog.chats.setMemberRole(member.modelData.userId,
                                                              member.modelData.role === "admin" ? "member" : "admin")
                    }
                    RvButton {
                        visible: dialog.canManage && !member.modelData.isSelf && member.modelData.role !== "owner"
                                 && (dialog.isOwner || member.modelData.role === "member")
                        primary: false
                        danger: true
                        implicitHeight: 28
                        text: "Remove"
                        enabled: !dialog.chats.groupBusy
                        onClicked: dialog.chats.removeMember(member.modelData.userId)
                    }
                }
            }
        }

        // Add friends
        RvButton {
            visible: dialog.canManage && !dialog.adding
            primary: false
            text: "Add people"
            onClicked: dialog.adding = true
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.adding
            spacing: 8
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "New members see messages sent from now on, not the history."
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
            FriendPicker {
                id: picker
                Layout.fillWidth: true
                friends: dialog.people.friends
                exclude: dialog.chats.currentMembers.map(m => m.userId)
            }
            RvButton {
                Layout.fillWidth: true
                text: dialog.chats.groupBusy ? "Adding…" : "Add " + picker.selected.length + " to the group"
                enabled: !dialog.chats.groupBusy && picker.selected.length > 0
                onClicked: dialog.chats.addMembers(picker.selected)
            }
        }

        GroupInviteSection {
            Layout.fillWidth: true
            invites: App.groupInvites
        }

        Banner {
            Layout.fillWidth: true
            text: dialog.error
        }

        // Leave
        RvButton {
            visible: !dialog.confirmLeave
            primary: false
            danger: true
            iconName: "log-out"
            text: "Leave group"
            onClicked: dialog.confirmLeave = true
        }
        RowLayout {
            visible: dialog.confirmLeave
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Leave this group? You'll stop receiving its messages."
                color: Theme.text
            }
            RvButton {
                primary: false
                text: "Cancel"
                onClicked: dialog.confirmLeave = false
            }
            RvButton {
                text: "Leave"
                enabled: !dialog.chats.groupBusy
                onClicked: dialog.chats.leaveGroup()
            }
        }
    }
}
