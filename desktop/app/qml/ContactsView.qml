pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Contacts: favorites, then everyone A–Z, and the selected person's profile
// (message, call, favorite, mute, verify, block, report).
Item {
    id: root
    required property ChatSession chats
    required property PeopleSession people
    signal openChat(string chatId)
    signal verify(string chatId)
    signal openFriends()

    property string selectedUserId: ""
    property string notice: ""
    property bool reporting: false

    readonly property var contacts: {
        const q = search.text.trim().toLowerCase()
        return root.chats.contacts.filter(c => q.length === 0 || c.handle.toLowerCase().indexOf(q) >= 0)
    }
    // Section headings: "Favorites", then the first letter.
    readonly property var rows: {
        const out = []
        const favs = root.contacts.filter(c => root.chats.favorites.indexOf(c.userId) >= 0)
        if (favs.length > 0) {
            out.push({ header: "Favorites" })
            favs.forEach(c => out.push(c))
        }
        let letter = ""
        for (const c of root.contacts) {
            const first = c.handle.charAt(0).toUpperCase()
            const key = /[A-Z]/.test(first) ? first : "#"
            if (key !== letter) {
                letter = key
                out.push({ header: key })
            }
            out.push(c)
        }
        return out
    }
    readonly property var selected: root.chats.contacts.find(c => c.userId === root.selectedUserId) || null
    readonly property bool favorite: selected !== null && root.chats.favorites.indexOf(selected.userId) >= 0
    readonly property bool muted: selected !== null && root.chats.muted.indexOf(selected.userId) >= 0
    readonly property bool blocked: selected !== null && root.people.blocked.indexOf(selected.userId) >= 0
    readonly property bool online: selected !== null && App.live !== null && App.live.online.indexOf(selected.userId) >= 0

    function call(kind) {
        if (!selected || !App.calls) return
        root.notice = App.calls.startCall(selected.chatId, kind)
    }

    Connections {
        target: root.people
        function onNoticeChanged() {
            if (root.people.notice.length > 0) root.notice = root.people.notice
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // --- List ---
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
                    Text {
                        Layout.fillWidth: true
                        text: "Contacts"
                        color: Theme.text
                        font.family: Theme.displayFamily
                        font.pixelSize: Theme.fontDisplay
                        font.weight: Font.Bold
                    }
                    IconButton {
                        iconName: "user-plus"
                        tip: "Add a friend"
                        badge: root.people.incoming.length
                        onClicked: root.openFriends()
                    }
                }

                SearchField {
                    id: search
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.bottomMargin: 8
                    placeholderText: "Search contacts"
                }

                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.rows
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Loader {
                        id: rowLoader
                        required property var modelData
                        width: ListView.view.width
                        sourceComponent: modelData.header !== undefined ? headerRow : contactRow

                        Component {
                            id: headerRow
                            Text {
                                leftPadding: 24
                                topPadding: 14
                                bottomPadding: 6
                                text: rowLoader.modelData.header.toUpperCase()
                                textFormat: Text.PlainText
                                color: Theme.text3
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.8
                            }
                        }
                        Component {
                            id: contactRow
                            ItemDelegate {
                                id: row
                                readonly property var c: rowLoader.modelData
                                height: 60
                                leftInset: 8
                                rightInset: 8
                                leftPadding: 18
                                rightPadding: 18
                                onClicked: {
                                    root.selectedUserId = c.userId
                                    root.notice = ""
                                    root.reporting = false
                                }
                                background: Rectangle {
                                    radius: Theme.radiusControl
                                    color: root.selectedUserId === row.c.userId ? Theme.selected
                                         : row.hovered ? Qt.alpha(Theme.selected, 0.5) : "transparent"
                                }
                                contentItem: RowLayout {
                                    spacing: 12
                                    Avatar {
                                        size: 40
                                        initial: row.c.initial
                                        source: row.c.avatarUrl
                                        tintKey: row.c.userId
                                        online: App.live !== null && App.live.online.indexOf(row.c.userId) >= 0
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: row.c.handle
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        color: Theme.text
                                        font.pixelSize: 15
                                    }
                                    Icon {
                                        visible: row.c.verified
                                        name: "shield-check"
                                        size: 15
                                        color: Theme.accent
                                    }
                                    Icon {
                                        visible: root.chats.muted.indexOf(row.c.userId) >= 0
                                        name: "bell-off"
                                        size: 15
                                        color: Theme.text3
                                    }
                                }
                            }
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 48
                        visible: list.count === 0
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: search.text.length > 0 ? "Nobody by that name." : "No contacts yet. Add a friend to start a chat."
                        textFormat: Text.PlainText
                        color: Theme.text3
                    }
                }
            }
        }

        // --- Profile ---
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.bg

            Text {
                anchors.centerIn: parent
                visible: root.selected === null
                text: "Select a contact"
                color: Theme.text3
                font.pixelSize: 15
            }

            Flickable {
                id: profile
                anchors.fill: parent
                visible: root.selected !== null
                clip: true
                contentWidth: width
                contentHeight: profileColumn.implicitHeight + 64
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                ColumnLayout {
                    id: profileColumn
                    x: Math.max(32, (profile.width - 560) / 2)
                    y: 40
                    width: Math.min(560, profile.width - 64)
                    spacing: 16

                    Avatar {
                        Layout.alignment: Qt.AlignHCenter
                        size: 104
                        initial: root.selected ? root.selected.initial : "?"
                        source: root.selected ? root.selected.avatarUrl : ""
                        tintKey: root.selectedUserId
                        online: root.online
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: root.selected ? root.selected.handle : ""
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.family: Theme.displayFamily
                        font.pixelSize: 32
                        font.weight: Font.Bold
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: -10
                        text: root.online ? "Online" : "@" + (root.selected ? root.selected.handle : "")
                        textFormat: Text.PlainText
                        color: root.online ? Theme.accent : Theme.text3
                        font.family: root.online ? Theme.fontFamily : Theme.monoFamily
                        font.pixelSize: 14
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 10
                        RvButton {
                            iconName: "chat"
                            text: "Message"
                            enabled: !root.blocked
                            onClicked: root.openChat(root.selected.chatId)
                        }
                        RvButton {
                            secondary: true
                            iconName: "phone"
                            text: "Voice"
                            visible: App.calls !== null && App.calls.callsAvailable
                            enabled: !root.blocked
                            onClicked: root.call("voice")
                        }
                        RvButton {
                            secondary: true
                            iconName: "video"
                            text: "Video"
                            visible: App.calls !== null && App.calls.callsAvailable
                            enabled: !root.blocked
                            onClicked: root.call("video")
                        }
                    }

                    Banner {
                        Layout.fillWidth: true
                        error: root.notice.indexOf("Blocked") !== 0 && root.notice.indexOf("Unblocked") !== 0 && root.notice.indexOf("Reported") !== 0
                        text: root.notice
                    }
                    Banner {
                        Layout.fillWidth: true
                        text: root.people.error
                    }

                    Card {
                        Layout.fillWidth: true
                        spacing: 0
                        RvSwitch {
                            Layout.fillWidth: true
                            label: "Favorite"
                            description: "Shown first in Contacts and under Favorites in Chats."
                            checked: root.favorite
                            onToggled: root.chats.toggleFavorite(root.selectedUserId)
                        }
                        Rectangle { Layout.fillWidth: true; Layout.topMargin: 8; Layout.bottomMargin: 8; implicitHeight: 1; color: Theme.border }
                        RvSwitch {
                            Layout.fillWidth: true
                            label: "Notifications"
                            description: root.muted ? "Muted: no notifications, and calls don't ring." : "On"
                            checked: !root.muted
                            onToggled: root.chats.toggleMuted(root.selectedUserId)
                        }
                    }

                    // Contact code: the safety number, to compare.
                    Card {
                        Layout.fillWidth: true
                        RowLayout {
                            Layout.fillWidth: true
                            Icon {
                                name: "shield-check"
                                size: 18
                                color: root.selected && root.selected.verified ? Theme.accent : Theme.text2
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.selected && root.selected.verified ? "Verified contact" : "Safety number"
                                textFormat: Text.PlainText
                                color: root.selected && root.selected.verified ? Theme.accent : Theme.text
                                font.weight: Font.DemiBold
                            }
                            RvButton {
                                primary: false
                                text: root.selected && root.selected.verified ? "View" : "Verify"
                                implicitHeight: 34
                                onClicked: root.verify(root.selected.chatId)
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: root.selected ? root.selected.safetyNumber : ""
                            textFormat: Text.PlainText
                            color: Theme.text2
                            font.family: Theme.monoFamily
                            font.pixelSize: 14
                            lineHeight: 1.3
                        }
                    }

                    Card {
                        Layout.fillWidth: true
                        spacing: 8
                        RowLayout {
                            Layout.fillWidth: true
                            RvButton {
                                primary: false
                                danger: true
                                iconName: "ban"
                                text: root.blocked ? "Unblock" : "Block"
                                enabled: !root.people.busy
                                onClicked: root.blocked ? root.people.unblock(root.selectedUserId) : root.people.block(root.selectedUserId)
                            }
                            RvButton {
                                primary: false
                                danger: true
                                iconName: "flag"
                                text: "Report"
                                visible: !root.reporting
                                onClicked: {
                                    root.people.clearMessages()
                                    root.reporting = true
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: root.reporting
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: "Moderators see your report and this account, never your messages: they're end-to-end encrypted."
                                color: Theme.text3
                                font.pixelSize: 13
                            }
                            RvTextField {
                                id: reason
                                Layout.fillWidth: true
                                placeholderText: "What happened?"
                                maximumLength: 1000
                            }
                            RowLayout {
                                Item { Layout.fillWidth: true }
                                RvButton {
                                    primary: false
                                    text: "Cancel"
                                    onClicked: root.reporting = false
                                }
                                RvButton {
                                    danger: true
                                    text: "Send report"
                                    enabled: reason.text.trim().length > 0 && !root.people.busy
                                    onClicked: {
                                        root.people.report(root.selectedUserId, reason.text)
                                        reason.text = ""
                                        root.reporting = false
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
