// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The open chat's details panel (320px): who, quick actions, encryption and
// verification, members (groups) and the files shared in the chat.
Rectangle {
    id: root
    required property ChatSession session
    required property PeopleSession people
    signal fileRequested(string messageId, string name)
    signal verifyRequested()
    signal groupInfoRequested()
    signal callRequested(string kind)

    readonly property string userId: session.currentUserId
    readonly property bool isGroup: session.currentIsGroup
    // A paid or free community: members and settings are on its page, and a channel's subscribers only read.
    readonly property bool isCommunity: session.currentCommunityId.length > 0
    readonly property bool readOnly: isCommunity && !session.currentCanPost
    readonly property bool favorite: userId.length > 0 && session.favorites.indexOf(userId) >= 0
    readonly property bool muted: userId.length > 0 && session.muted.indexOf(userId) >= 0
    readonly property bool online: userId.length > 0 && App.live !== null && App.live.online.indexOf(userId) >= 0

    color: Theme.panel

    Rectangle {
        anchors.left: parent.left
        width: 1
        height: parent.height
        color: Theme.border
    }

    component Caption: Text {
        Layout.topMargin: 6
        color: Theme.text3
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
        font.capitalization: Font.AllUppercase
    }

    component Action: ColumnLayout {
        id: action
        property string iconName
        property string label
        property bool on: false
        property bool enabledAction: true
        signal activated()
        spacing: 6
        Layout.fillWidth: true
        IconButton {
            Layout.alignment: Qt.AlignHCenter
            iconName: action.iconName
            tip: action.label
            filled: true
            round: true
            toggled: action.on
            enabled: action.enabledAction
            implicitWidth: 44
            implicitHeight: 44
            onClicked: action.activated()
        }
        Text {
            Layout.alignment: Qt.AlignHCenter
            text: action.label
            textFormat: Text.PlainText
            color: Theme.text3
            font.pixelSize: 11
        }
    }

    Flickable {
        id: scroller
        anchors.fill: parent
        anchors.leftMargin: 1
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight + 40
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            x: 20
            y: 24
            width: scroller.width - 40
            spacing: 12

            Avatar {
                Layout.alignment: Qt.AlignHCenter
                size: 80
                initial: root.session.currentTitle.length > 0 ? root.session.currentTitle.charAt(0).toUpperCase() : "?"
                group: root.isGroup
                source: root.session.currentAvatarUrl
                communityImage: root.session.currentCommunityImage
                tintKey: root.isGroup ? root.session.currentChatId : root.userId
                online: root.online
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: root.session.currentTitle
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.text
                font.family: Theme.displayFamily
                font.pixelSize: 22
                font.weight: Font.Bold
            }
            Text {
                Layout.fillWidth: true
                Layout.topMargin: -8
                horizontalAlignment: Text.AlignHCenter
                text: root.isCommunity ? (root.session.currentCommunityKind === "channel" ? "Channel" : "Community") + (root.readOnly ? " · read only" : "")
                    : root.isGroup ? root.session.currentMembers.length + " members"
                    : root.online ? "Online" : "@" + root.session.currentTitle
                textFormat: Text.PlainText
                color: root.online ? Theme.accent : Theme.text3
                font.family: root.isGroup || root.online ? Theme.fontFamily : Theme.monoFamily
                font.pixelSize: 13
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                visible: !root.isGroup
                spacing: 4
                Action {
                    iconName: "phone"
                    label: "Voice"
                    enabledAction: App.calls !== null && App.calls.callsAvailable
                    onActivated: root.callRequested("voice")
                }
                Action {
                    iconName: "video"
                    label: "Video"
                    enabledAction: App.calls !== null && App.calls.callsAvailable
                    onActivated: root.callRequested("video")
                }
                Action {
                    iconName: "star"
                    label: root.favorite ? "Favorite" : "Favorite"
                    on: root.favorite
                    onActivated: root.session.toggleFavorite(root.userId)
                }
                Action {
                    iconName: root.muted ? "bell-off" : "bell"
                    label: root.muted ? "Muted" : "Mute"
                    on: root.muted
                    onActivated: root.session.toggleMuted(root.userId)
                }
            }

            // Their story and the highlights you can see.
            HighlightsRow {
                Layout.fillWidth: true
                Layout.topMargin: 6
                stories: App.stories
                userId: root.isGroup ? "" : root.userId
            }

            // As a creator: tips, what messaging them costs, exclusive posts; and letting them message you free.
            CreatorOfferSection {
                Layout.fillWidth: true
                Layout.topMargin: 6
                earnings: App.earnings
                userId: root.isGroup ? "" : root.userId
                name: root.session.currentTitle
            }

            // Encryption and verification.
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 6
                implicitHeight: encColumn.implicitHeight + 28
                radius: Theme.radiusCard
                color: root.session.currentKeyChanged ? Theme.warnSoft : Theme.accentSoft
                border.color: root.session.currentKeyChanged ? Qt.alpha(Theme.warn, 0.35) : Theme.accentBorder

                ColumnLayout {
                    id: encColumn
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 8
                    RowLayout {
                        spacing: 8
                        Icon {
                            name: root.session.currentKeyChanged ? "triangle-alert" : "shield-check"
                            size: 18
                            color: root.session.currentKeyChanged ? Theme.warn : Theme.accent
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.session.currentKeyChanged ? "Safety number changed"
                                : root.isGroup ? "End-to-end encrypted"
                                : root.session.currentVerified ? "Verified" : "Encrypted, not verified yet"
                            textFormat: Text.PlainText
                            color: root.session.currentKeyChanged ? Theme.warn : Theme.accent
                            font.weight: Font.DemiBold
                            font.pixelSize: 14
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: root.isCommunity ? "Posts and files are encrypted on members' devices. When someone leaves, the members' apps switch to a new key they never get."
                            : root.isGroup ? "Messages and files are encrypted on each member's device. The server only relays ciphertext."
                            : root.session.currentKeyChanged ? "Their key changed since you last talked. Compare the new safety number before you trust this chat."
                            : root.session.currentVerified ? "You compared safety numbers. Calls and messages go only to their verified key."
                            : "Compare safety numbers to be sure nobody is in the middle."
                        textFormat: Text.PlainText
                        color: Theme.text2
                        font.pixelSize: 13
                    }
                    RvButton {
                        visible: !root.isGroup
                        primary: !root.session.currentVerified
                        iconName: "shield-check"
                        text: root.session.currentVerified ? "View safety number" : "Verify safety number"
                        implicitHeight: 36
                        onClicked: root.verifyRequested()
                    }
                    RvButton {
                        visible: root.isGroup && !root.isCommunity
                        primary: false
                        iconName: "info"
                        text: "Group info"
                        implicitHeight: 36
                        onClicked: root.groupInfoRequested()
                    }
                    RvButton {
                        visible: root.isCommunity && App.communities !== null
                        primary: false
                        iconName: "info"
                        text: "Community page"
                        implicitHeight: 36
                        onClicked: {
                            App.communities.open("community", root.session.currentCommunityId)
                            App.communities.showRequested()
                        }
                    }
                }
            }

            // Your disappearing-messages timer here (the server deletes what you send when it runs out).
            Caption {
                visible: !root.readOnly
                text: "Disappearing messages"
            }
            Text {
                visible: !root.readOnly
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: (root.session.currentDisappearing === 0 ? "Off: what you send stays until you delete it."
                      : root.session.currentDisappearing === 1 ? "What you send is deleted right after sending."
                      : "What you send is deleted " + root.session.currentDisappearingLabel.toLowerCase() + " after sending.")
                      + (root.isGroup ? " Each member has their own setting." : " " + root.session.currentTitle + " has their own setting.")
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: 13
            }
            Flow {
                Layout.fillWidth: true
                visible: !root.readOnly
                spacing: 6
                Repeater {
                    model: root.session.disappearingOptions
                    delegate: Chip {
                        required property var modelData
                        text: modelData.label
                        checked: modelData.seconds === root.session.currentDisappearing
                        onClicked: root.session.setDisappearing(modelData.seconds)
                    }
                }
            }

            // Members of a group. A community lists only who wrote what's loaded: its page has the rest, for staff.
            Caption {
                visible: root.isGroup
                text: root.isCommunity ? "Posting here" : "Members"
            }
            Repeater {
                model: root.isGroup ? root.session.currentMembers : []
                delegate: RowLayout {
                    id: member
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 10
                    Avatar {
                        size: 32
                        initial: member.modelData.handle.length > 0 ? member.modelData.handle.charAt(0).toUpperCase() : "?"
                        source: member.modelData.avatarUrl
                        tintKey: member.modelData.userId
                    }
                    Text {
                        Layout.fillWidth: true
                        text: member.modelData.isSelf ? member.modelData.handle + " (you)" : member.modelData.handle
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text
                    }
                    Text {
                        visible: member.modelData.role !== "member"
                        text: member.modelData.role
                        textFormat: Text.PlainText
                        color: Theme.text3
                        font.pixelSize: 12
                    }
                }
            }

            // Shared files, decrypted only when opened.
            RowLayout {
                Layout.fillWidth: true
                Caption {
                    Layout.fillWidth: true
                    text: "Shared media and files"
                }
                Text {
                    Layout.topMargin: 6
                    text: root.session.currentFiles.length
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.pixelSize: 12
                }
            }
            Text {
                visible: root.session.currentFiles.length === 0
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Photos, videos and files you share here show up in this list."
                color: Theme.text3
                font.pixelSize: 13
            }
            Repeater {
                model: root.session.currentFiles
                delegate: ItemDelegate {
                    id: fileRow
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 52
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: root.fileRequested(modelData.messageId, modelData.name)
                    background: Rectangle {
                        radius: Theme.radiusControl
                        color: fileRow.hovered ? Theme.surface : "transparent"
                    }
                    contentItem: RowLayout {
                        spacing: 10
                        Rectangle {
                            id: thumbBox
                            // A picture already decrypted for the chat shows as itself.
                            readonly property bool hasThumb: fileRow.modelData.image && root.session.imagesRevision >= 0
                                                             && root.session.imageInfo(fileRow.modelData.messageId).state === "ready"
                            implicitWidth: 38
                            implicitHeight: 38
                            radius: 10
                            color: Theme.surface2
                            clip: true
                            Image {
                                anchors.fill: parent
                                visible: thumbBox.hasThumb
                                asynchronous: true
                                cache: false
                                fillMode: Image.PreserveAspectCrop
                                sourceSize.width: 76
                                source: thumbBox.hasThumb ? "image://chatimage/thumb/" + fileRow.modelData.messageId : ""
                            }
                            Icon {
                                anchors.centerIn: parent
                                visible: !thumbBox.hasThumb
                                name: fileRow.modelData.image ? "image" : fileRow.modelData.video ? "video" : "file"
                                size: 18
                                color: Theme.text2
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: fileRow.modelData.name
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: Theme.text
                                font.pixelSize: 13
                            }
                            Text {
                                text: fileRow.modelData.size + " · " + fileRow.modelData.day + (fileRow.modelData.fromMe ? " · you" : "")
                                textFormat: Text.PlainText
                                color: Theme.text3
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }
        }
    }
}
