// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Layouts
import Redvault

// Name a group and pick friends. The name is encrypted with the group key,
// and the key is sealed separately to each member: the server learns neither.
RvDialog {
    id: dialog
    required property ChatSession chats
    required property PeopleSession people
    title: "New group"
    property string error: ""

    onOpened: {
        nameField.text = ""
        picker.reset()
        error = ""
        people.refresh()
        nameField.forceActiveFocus()
    }

    Connections {
        target: dialog.chats
        function onGroupActionDone(message) {
            if (!dialog.visible)
                return
            dialog.error = message
            if (message.length === 0)
                dialog.close()
        }
    }

    contentItem: ColumnLayout {
        spacing: 12

        RvTextField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: "Group name"
            maximumLength: 100
            enabled: !dialog.chats.groupBusy
        }

        Text {
            text: "Members"
            color: Theme.text2
            font.pixelSize: Theme.fontSmall
            font.weight: Font.DemiBold
        }

        FriendPicker {
            id: picker
            Layout.fillWidth: true
            friends: dialog.people.friends
        }

        Banner {
            Layout.fillWidth: true
            text: dialog.error
        }

        RvButton {
            Layout.fillWidth: true
            text: dialog.chats.groupBusy ? "Creating…" : "Create group"
            enabled: !dialog.chats.groupBusy && nameField.text.trim().length > 0 && picker.selected.length > 0
            onClicked: dialog.chats.createGroup(nameField.text, picker.selected)
        }
    }
}
