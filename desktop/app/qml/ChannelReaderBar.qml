// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Redvault

// What a reader has where a chat has its composer: nothing to type in. Only
// the owner posts in a channel; a reader follows it, mutes it and, where the
// channel takes them, suggests a post for the owner to review.
Rectangle {
    id: bar
    required property ChannelsSession channels
    signal suggestRequested()

    implicitHeight: row.implicitHeight + 24
    color: Theme.bg

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.border
    }

    RowLayout {
        id: row
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 18
        anchors.rightMargin: 12
        spacing: 8

        Text {
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            wrapMode: Text.Wrap
            text: "Only the owner posts here."
            color: Theme.text2
            font.pixelSize: Theme.fontSmall + 1
        }
        RvButton {
            objectName: "channelSuggestButton"
            visible: bar.channels.channel.acceptSuggestions === true
            secondary: true
            implicitHeight: 36
            text: "Suggest a post"
            onClicked: bar.suggestRequested()
        }
        IconButton {
            visible: bar.channels.isFollowing
            iconName: bar.channels.isMuted ? "bell-off" : "bell"
            tip: bar.channels.isMuted ? "Notifications are off for this channel" : "You're notified of new posts"
            onClicked: bar.channels.toggleMuted()
        }
        RvButton {
            objectName: "channelFollowButton"
            primary: !bar.channels.isFollowing
            secondary: bar.channels.isFollowing
            implicitHeight: 36
            iconName: bar.channels.isFollowing ? "check" : ""
            text: bar.channels.isFollowing ? "Following" : "Follow"
            Accessible.checked: bar.channels.isFollowing
            onClicked: bar.channels.toggleFollow()
        }
    }
}
