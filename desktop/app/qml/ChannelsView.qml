pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Channels, as the web app's Channels screen: one-way and not end-to-end
// encrypted. Following (kept on this computer only), Discover (the public
// directory) and Yours (Premium members create channels here). One channel
// opens in ChannelPage.
Item {
    id: root
    required property ChannelsSession channels
    property bool active: false
    property string tab: ""
    signal openPlans()

    onActiveChanged: {
        if (active) root.refresh()
        if (channels) channels.pageShown = active
    }
    Component.onCompleted: if (active) root.refresh()

    function refresh() {
        if (!channels) return
        if (tab.length === 0) tab = channels.followingCount > 0 ? "following" : "discover"
        channels.refreshFollowing()
        channels.refreshMine()
        if (!channels.discoverLoaded) channels.search(searchField.text)
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // One channel.
    ChannelPage {
        anchors.fill: parent
        visible: root.channels !== null && root.channels.channelId.length > 0
        channels: root.channels
        onBack: {
            root.channels.closeChannel()
            root.channels.refreshFollowing()
        }
    }

    // The list.
    Flickable {
        id: scroller
        anchors.fill: parent
        visible: root.channels !== null && root.channels.channelId.length === 0
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight + 64
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            x: Math.max(32, (scroller.width - 680) / 2)
            y: 32
            width: Math.max(0, Math.min(680, scroller.width - 64))
            spacing: 14

            Text {
                text: "Channels"
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
                    name: "globe"
                    size: 16
                    color: Theme.warn
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Channels aren't end-to-end encrypted like your chats: anyone can read a public channel, and anyone with the link can read a private one. Who follows a channel is kept only on each person's device, so there's no limit on followers."
                    color: Theme.text2
                    font.pixelSize: 13
                }
            }

            Row {
                spacing: 6
                Repeater {
                    model: [["following", "Following"], ["discover", "Discover"], ["yours", "Yours"]]
                    delegate: Chip {
                        required property var modelData
                        text: modelData[1]
                        checked: root.tab === modelData[0]
                        onClicked: root.tab = modelData[0]
                    }
                }
            }

            // --- Following ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.tab === "following"
                spacing: 8

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 12
                    visible: root.channels.followingCount === 0
                    spacing: 10
                    Text {
                        text: "You don't follow any channels yet."
                        color: Theme.text2
                    }
                    RvButton {
                        secondary: true
                        text: "Discover channels"
                        onClicked: root.tab = "discover"
                    }
                }
                Repeater {
                    model: root.channels.following
                    delegate: ChannelRow {
                        required property var modelData
                        Layout.fillWidth: true
                        channel: modelData
                        session: root.channels
                    }
                }
            }

            // --- Discover ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.tab === "discover"
                spacing: 8

                SearchField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: "Search channels"
                    onTextChanged: searchTimer.restart()
                }
                Timer {
                    id: searchTimer
                    interval: 250
                    onTriggered: root.channels.search(searchField.text)
                }
                Banner {
                    Layout.fillWidth: true
                    text: root.channels.discoverError
                }
                Text {
                    visible: !root.channels.discoverLoaded && root.channels.discoverError.length === 0
                    text: "Loading…"
                    color: Theme.text3
                }
                Text {
                    visible: root.channels.discoverLoaded && root.channels.discover.length === 0
                    text: root.channels.discoverQuery.trim().length > 0 ? "No channels match." : "No channels yet."
                    textFormat: Text.PlainText
                    color: Theme.text3
                }
                Repeater {
                    model: root.channels.discover
                    delegate: ChannelRow {
                        required property var modelData
                        Layout.fillWidth: true
                        channel: modelData
                        session: root.channels
                    }
                }
                RvButton {
                    visible: root.channels.discoverMore
                    secondary: true
                    text: "Show more"
                    onClicked: root.channels.loadMoreDiscover()
                }
            }

            // --- Yours ---
            ColumnLayout {
                id: yours
                Layout.fillWidth: true
                visible: root.tab === "yours"
                spacing: 8
                property bool creating: false
                property string error: ""

                Connections {
                    target: root.channels
                    function onCreated(error) {
                        yours.error = error
                        if (error.length === 0) {
                            yours.creating = false
                            newName.text = ""
                            newDescription.text = ""
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: !root.channels.canCreate
                    spacing: 10
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: "Creating a channel needs Premium. Anyone can follow channels on any plan."
                        color: Theme.text2
                    }
                    RvButton {
                        secondary: true
                        text: "See plans"
                        onClicked: root.openPlans()
                    }
                }

                RvButton {
                    visible: root.channels.canCreate && !yours.creating
                    iconName: "plus"
                    text: "New channel"
                    onClicked: {
                        yours.error = ""
                        yours.creating = true
                        newName.forceActiveFocus()
                    }
                }

                Card {
                    Layout.fillWidth: true
                    visible: root.channels.canCreate && yours.creating
                    spacing: 10
                    RvTextField {
                        id: newName
                        Layout.fillWidth: true
                        placeholderText: "Channel name"
                        maximumLength: root.channels.maxNameLength
                        Accessible.name: "Channel name"
                    }
                    PostField {
                        id: newDescription
                        Layout.fillWidth: true
                        placeholderText: "What's it about? (optional)"
                        Accessible.name: "Description"
                        limit: root.channels.maxDescriptionLength
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: "Your channel starts public: anyone can find it and read everything you post, with no limit on followers. You can make it private later. Posts show the channel's name and photo, never who runs it."
                        color: Theme.text3
                        font.pixelSize: Theme.fontSmall + 1
                    }
                    Banner {
                        Layout.fillWidth: true
                        text: yours.error
                    }
                    RowLayout {
                        spacing: 8
                        RvButton {
                            text: root.channels.creating ? "Creating…" : "Create channel"
                            enabled: !root.channels.creating && newName.text.trim().length > 0 && newDescription.text.length <= root.channels.maxDescriptionLength
                            onClicked: root.channels.create(newName.text, newDescription.text)
                        }
                        RvButton {
                            secondary: true
                            text: "Cancel"
                            enabled: !root.channels.creating
                            onClicked: yours.creating = false
                        }
                    }
                }

                Text {
                    visible: !root.channels.mineLoaded
                    text: "Loading…"
                    color: Theme.text3
                }
                Text {
                    visible: root.channels.mineLoaded && root.channels.mine.length === 0 && root.channels.canCreate && !yours.creating
                    text: "You haven't made a channel yet."
                    color: Theme.text3
                }
                Repeater {
                    model: root.channels.mine
                    delegate: ChannelRow {
                        required property var modelData
                        Layout.fillWidth: true
                        channel: modelData
                        session: root.channels
                    }
                }
            }
        }
    }

    // A channel in a list: its photo, its name (marked when private, with a dot for new posts), what it's
    // about, and when it last posted. Anchors rather than layouts inside, so a wrapped description can't
    // feed its height back into its width.
    component ChannelRow: ItemDelegate {
        id: row
        property var channel: ({})
        property ChannelsSession session
        implicitHeight: Math.max(64, texts.implicitHeight + 20)
        leftPadding: 14
        rightPadding: 14
        onClicked: if (!row.channel.deleted && row.session) row.session.openChannel(row.channel.id)
        background: Rectangle {
            radius: Theme.radiusControl
            color: row.hovered && !row.channel.deleted ? Theme.surface2 : Theme.surface
            border.color: Theme.border
        }
        contentItem: Item {
            ChannelAvatar {
                id: rowAvatar
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                size: 40
                name: row.channel.name || ""
                photoKey: row.channel.photo || ""
            }
            Column {
                id: texts
                anchors.left: rowAvatar.right
                anchors.leftMargin: 12
                anchors.right: side.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                Row {
                    spacing: 8
                    Text {
                        id: nameText
                        width: Math.min(implicitWidth, Math.max(0, texts.width - 16 - (privateTag.visible ? privateTag.width + 8 : 0)))
                        text: row.channel.name || ""
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text
                        font.weight: Font.DemiBold
                    }
                    Rectangle {
                        id: privateTag
                        anchors.verticalCenter: nameText.verticalCenter
                        visible: row.channel.isPrivate === true
                        width: privateLabel.implicitWidth + 12
                        height: privateLabel.implicitHeight + 4
                        radius: height / 2
                        color: Theme.surface2
                        border.color: Theme.border
                        Text {
                            id: privateLabel
                            anchors.centerIn: parent
                            text: "Private"
                            color: Theme.text2
                            font.pixelSize: Theme.fontSmall
                        }
                    }
                    Rectangle {
                        anchors.verticalCenter: nameText.verticalCenter
                        visible: row.channel.unread === true
                        width: 8
                        height: 8
                        radius: 4
                        color: Theme.accent
                        Accessible.name: "New posts"
                    }
                }
                Text {
                    width: texts.width
                    visible: (row.channel.description || "").length > 0
                    text: row.channel.description || ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    maximumLineCount: 2
                    wrapMode: Text.Wrap
                    color: Theme.text2
                    font.pixelSize: Theme.fontSmall + 1
                }
            }
            Item {
                id: side
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: row.channel.deleted ? removeButton.implicitWidth : whenText.implicitWidth
                height: row.channel.deleted ? removeButton.implicitHeight : whenText.implicitHeight
                Text {
                    id: whenText
                    visible: !row.channel.deleted
                    text: row.channel.when || ""
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall
                }
                RvButton {
                    id: removeButton
                    visible: row.channel.deleted === true
                    primary: false
                    text: "Remove"
                    onClicked: if (row.session) row.session.removeFollowed(row.channel.id)
                }
            }
        }
    }
}
