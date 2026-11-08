// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Pick one of your contacts (people you have a 1:1 chat with) to share
// something with. What's shared is sealed to their key on this computer.
RvDialog {
    id: dialog
    required property ChatSession chats
    signal picked(string userId)
    width: 420

    contentItem: ColumnLayout {
        spacing: 12
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "It's encrypted to their key on this computer: only they can open it, and they can't change it."
            color: Theme.text2
            font.pixelSize: 13
        }
        SearchField {
            id: filter
            Layout.fillWidth: true
            placeholderText: "Search contacts"
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.preferredHeight: 280
            clip: true
            model: dialog.chats.contacts.filter(c => filter.text.trim().length === 0
                                                     || c.handle.toLowerCase().indexOf(filter.text.trim().toLowerCase()) >= 0)
            delegate: ItemDelegate {
                id: row
                required property var modelData
                width: ListView.view.width
                height: 52
                onClicked: {
                    dialog.picked(modelData.userId)
                    dialog.close()
                }
                background: Rectangle {
                    radius: Theme.radiusControl
                    color: row.hovered ? Theme.selected : "transparent"
                }
                contentItem: RowLayout {
                    spacing: 12
                    Avatar { size: 34; initial: row.modelData.initial; source: row.modelData.avatarUrl; tintKey: row.modelData.userId }
                    Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: row.modelData.handle; color: Theme.text; elide: Text.ElideRight }
                    Icon { visible: row.modelData.verified; name: "shield-check"; size: 15; color: Theme.accent }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text: "No contacts yet. Add a friend first."
                color: Theme.text3
            }
        }
    }
    onOpened: filter.text = ""
}
