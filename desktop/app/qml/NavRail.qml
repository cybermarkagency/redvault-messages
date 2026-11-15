// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The 76px rail on the left: Chats, Calls, Contacts, Channels, Communities, Earnings, Settings, and you
// (and Admin, for RedVault's super admins).
Rectangle {
    id: rail
    property string current: "chats"
    property int chatsBadge: 0
    property int contactsBadge: 0
    property int inboxBadge: 0
    /// Followed channels with posts you haven't seen.
    property int channelsBadge: 0
    /// Files waiting in Incoming (uploaded through your public share links).
    property int filesBadge: 0
    property string avatarUrl: ""
    /// RedVault's super admins get an Admin button too.
    property bool showAdmin: false
    signal navigate(string page)
    /// A short window: smaller buttons, so everything still fits down to the window's minimum height (620).
    readonly property bool compact: height < (showAdmin ? 880 : 820)
    /// Eleven buttons in a short window: smaller still.
    readonly property bool tight: showAdmin && height < 660

    implicitWidth: Theme.railWidth
    color: Theme.rail

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
    }

    component RailButton: AbstractButton {
        id: button
        property string page
        property string iconName
        property int badge: 0
        property string current
        property bool compact: false
        property bool tight: false
        readonly property bool active: current === page
        signal picked(string page)

        Layout.alignment: Qt.AlignHCenter
        implicitWidth: 64
        implicitHeight: tight ? 42 : compact ? 46 : 60
        hoverEnabled: true
        Accessible.name: text
        onClicked: picked(page)

        focusPolicy: Qt.StrongFocus
        Accessible.role: Accessible.PageTab
        Accessible.checkable: true
        Accessible.checked: active
        background: Rectangle {
            radius: Theme.radiusControl
            color: button.active ? Theme.selected : (button.hovered ? Theme.hover : "transparent")
            border.width: button.visualFocus ? 2 : 0
            border.color: Theme.focusRing
            // The current page: a burgundy bar on the rail's edge as well as the lighter tile.
            Rectangle {
                visible: button.active
                x: -6
                anchors.verticalCenter: parent.verticalCenter
                width: 3
                height: 24
                radius: 2
                color: Theme.accentFill
            }
        }
        contentItem: Column {
            spacing: button.tight ? 1 : 4
            topPadding: button.tight ? 2 : button.compact ? 4 : 8
            Item {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 24
                height: 24
                Icon {
                    anchors.centerIn: parent
                    name: button.iconName
                    size: 22
                    color: button.active ? Theme.accent : Theme.text3
                }
                Rectangle {
                    visible: button.badge > 0
                    x: 14
                    y: -4
                    width: Math.max(18, badgeText.implicitWidth + 8)
                    height: 18
                    radius: 9
                    color: Theme.accentFill
                    Text {
                        id: badgeText
                        anchors.centerIn: parent
                        text: button.badge > 99 ? "99+" : button.badge
                        textFormat: Text.PlainText
                        color: Theme.accentContrast
                        font.pixelSize: 10
                        font.weight: Font.Bold
                    }
                }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: button.text
                textFormat: Text.PlainText
                color: button.active ? Theme.text : Theme.text3
                font.pixelSize: 11
                font.weight: button.active ? Font.DemiBold : Font.Normal
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 16
        anchors.bottomMargin: 16
        spacing: rail.compact ? 2 : 6

        // The official RedVaultMessages logo, as shipped (icons/app/redvault.svg).
        Image {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: rail.compact ? 4 : 12
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40
            source: "qrc:/qt/qml/Redvault/icons/app/redvault.svg"
            sourceSize: Qt.size(80, 80)
            fillMode: Image.PreserveAspectFit
            smooth: true
            Accessible.role: Accessible.Graphic
            Accessible.name: "RedVaultMessages"
        }

        RailButton { compact: rail.compact; tight: rail.tight; page: "chats"; iconName: "chat"; text: qsTr("Chats"); badge: rail.chatsBadge; current: rail.current; onPicked: (page) => rail.navigate(page) }
        RailButton { compact: rail.compact; tight: rail.tight; page: "calls"; iconName: "phone"; text: qsTr("Calls"); current: rail.current; onPicked: (page) => rail.navigate(page) }
        RailButton { compact: rail.compact; tight: rail.tight; page: "contacts"; iconName: "users"; text: qsTr("Contacts"); badge: rail.contactsBadge; current: rail.current; onPicked: (page) => rail.navigate(page) }
        RailButton { compact: rail.compact; tight: rail.tight; page: "channels"; iconName: "globe"; text: qsTr("Channels"); badge: rail.channelsBadge; current: rail.current; onPicked: (page) => rail.navigate(page) }
        RailButton { visible: App.communitiesEnabled; compact: rail.compact; tight: rail.tight; page: "communities"; iconName: "ticket"; text: qsTr("Communities"); current: rail.current; onPicked: (page) => rail.navigate(page) }
        RailButton { visible: App.earningsEnabled; compact: rail.compact; tight: rail.tight; page: "earnings"; iconName: "credit-card"; text: qsTr("Earnings"); current: rail.current; onPicked: (page) => rail.navigate(page) }
        RailButton { compact: rail.compact; tight: rail.tight; page: "files"; iconName: "cloud"; text: qsTr("Files"); badge: rail.filesBadge; current: rail.current; onPicked: (page) => rail.navigate(page) }
        RailButton { compact: rail.compact; tight: rail.tight; page: "passwords"; iconName: "key"; text: qsTr("Passwords"); current: rail.current; onPicked: (page) => rail.navigate(page) }
        RailButton { compact: rail.compact; tight: rail.tight; page: "inbox"; iconName: "inbox"; text: qsTr("Inbox"); badge: rail.inboxBadge; current: rail.current; onPicked: (page) => rail.navigate(page) }

        Item { Layout.fillHeight: true }

        RailButton { visible: rail.showAdmin; compact: rail.compact; tight: rail.tight; page: "admin"; iconName: "shield-check"; text: qsTr("Admin"); current: rail.current; onPicked: (page) => rail.navigate(page) }

        RailButton { compact: rail.compact; tight: rail.tight; page: "privacy"; iconName: "shield"; text: qsTr("Privacy"); current: rail.current; onPicked: (page) => rail.navigate(page) }
        RailButton { compact: rail.compact; tight: rail.tight; page: "settings"; iconName: "sliders"; text: qsTr("Settings"); current: rail.current; onPicked: (page) => rail.navigate(page) }

        AbstractButton {
            id: me
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 44
            implicitHeight: 44
            hoverEnabled: true
            Accessible.name: "Your account"
            ToolTip.visible: hovered
            ToolTip.text: App.handle
            onClicked: rail.navigate("settings")
            contentItem: Avatar {
                size: 40
                initial: App.handle.length > 0 ? App.handle.charAt(0).toUpperCase() : "?"
                tintKey: App.userId
                source: rail.avatarUrl
            }
            background: Item {}
        }
    }
}
