// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// One post in a channel's thread, in MessageDelegate's look: a day header when
// a new day starts, then an incoming bubble on the left. A post is the
// channel's: its small avatar and name, never who posted it. The footer is the
// time and the view count and nothing else: channels aren't end-to-end
// encrypted, so a post has no lock, security row or delivery marks.
Item {
    id: root
    // The model's roles.
    required property var post
    required property string dayHeader
    required property ChannelsSession session
    property bool canModerate: false
    property bool isOwner: false
    signal linkRequested(string url)
    signal photoRequested(var media)
    signal saveRequested(var media)
    /// The forward button.
    signal forwardRequested(var post)
    /// The delete button, for the owner and admins.
    signal deleteRequested(string postId)
    /// The "more" button or a right-click.
    signal menuRequested(var post)

    readonly property int avatarSize: 30
    readonly property int bubbleMargin: 18 + avatarSize + 8
    // A chat's width, leaving room beside the bubble for the hover actions.
    readonly property int maxBubbleWidth: Math.max(160, Math.min(560, width * 0.72, width - bubbleMargin - 116))

    implicitHeight: dayRow.height + bubbleRow.height + 4

    HoverHandler {
        id: hover
    }

    // Day header
    Item {
        id: dayRow
        width: parent.width
        height: root.dayHeader.length > 0 ? 40 : 0
        visible: height > 0
        Rectangle {
            anchors.centerIn: parent
            implicitWidth: dayText.implicitWidth + 20
            implicitHeight: 24
            radius: 12
            color: Theme.surface2
            Text {
                id: dayText
                anchors.centerIn: parent
                text: root.dayHeader
                textFormat: Text.PlainText
                color: Theme.text2
                font.pixelSize: Theme.fontSmall
            }
        }
    }

    Item {
        id: bubbleRow
        anchors.top: dayRow.bottom
        width: parent.width
        height: bubble.height

        ChannelAvatar {
            x: 18
            anchors.bottom: parent.bottom
            size: root.avatarSize
            name: root.session.channel.name || ""
            photoKey: root.session.channel.photo || ""
        }

        Rectangle {
            id: bubble
            objectName: "channelPostBubble"
            x: root.bubbleMargin
            width: Math.min(root.maxBubbleWidth, content.wide ? root.maxBubbleWidth
                            : Math.max(nameText.implicitWidth, footer.implicitWidth, content.implicitWidth) + 24)
            height: bubbleColumn.implicitHeight + 16
            radius: Theme.radiusCard
            color: Theme.selected

            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: root.menuRequested(root.post)
            }

            ColumnLayout {
                id: bubbleColumn
                x: 12
                y: 8
                width: bubble.width - 24
                spacing: 4

                Text {
                    id: nameText
                    Layout.fillWidth: true
                    text: root.session.channel.name || ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.accent
                    font.pixelSize: Theme.fontSmall
                    font.weight: Font.DemiBold
                }

                ChannelPostContent {
                    id: content
                    Layout.fillWidth: true
                    post: root.post
                    session: root.session
                    isOwner: root.isOwner
                    onLinkRequested: (url) => root.linkRequested(url)
                    onPhotoRequested: (media) => root.photoRequested(media)
                    onSaveRequested: (media) => root.saveRequested(media)
                }

                Text {
                    id: footer
                    objectName: "channelPostFooter"
                    Layout.alignment: Qt.AlignRight
                    text: root.post.time + " · " + root.post.views
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.pixelSize: 11
                }
            }
        }

        // Hover actions, as a chat message has.
        Row {
            anchors.left: bubble.right
            anchors.leftMargin: 6
            anchors.verticalCenter: bubble.verticalCenter
            spacing: 2
            opacity: hover.hovered ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 90 } }

            RvButton {
                visible: App.chats !== null
                primary: false
                iconName: "share"
                iconSize: 16
                implicitHeight: 30
                Accessible.name: "Forward"
                ToolTip.visible: hovered
                ToolTip.text: "Forward"
                onClicked: root.forwardRequested(root.post)
            }
            RvButton {
                visible: root.canModerate
                primary: false
                danger: true
                iconName: "trash"
                iconSize: 16
                implicitHeight: 30
                enabled: !root.session.busy
                Accessible.name: "Delete post"
                ToolTip.visible: hovered
                ToolTip.text: "Delete post"
                onClicked: root.deleteRequested(root.post.id)
            }
            RvButton {
                primary: false
                iconName: "dots"
                iconSize: 16
                implicitHeight: 30
                Accessible.name: "More actions"
                ToolTip.visible: hovered
                ToolTip.text: "More"
                onClicked: root.menuRequested(root.post)
            }
        }
    }
}
