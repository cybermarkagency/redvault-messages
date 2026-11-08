// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// The channel's info panel, beside the thread as a chat's details panel is:
// what it is, Follow, mute, Copy link, Report, and Delete; for the owner, the
// name, description and photo, public or private, suggested posts and the
// private link. It says plainly that a channel isn't end-to-end encrypted, and
// offers nothing a chat's panel has for encryption: there's nothing to verify.
Rectangle {
    id: panel
    required property ChannelsSession channels
    signal closeRequested()

    readonly property bool loaded: channels !== null && channels.channel.id !== undefined
    readonly property bool canManage: channels !== null && channels.canManage
    readonly property bool isPrivate: loaded && channels.channel.isPrivate === true
    readonly property string link: loaded ? channels.channel.link : ""
    property bool editing: false
    /// Switching between public and private, waiting for the owner to confirm.
    property bool pendingPrivate: false

    color: Theme.panel

    Connections {
        target: panel.channels
        function onDetailsSaved() { panel.editing = false }
    }
    // Another channel opened, or back to the list: the old one's form goes.
    readonly property string shownId: channels ? channels.channelId : ""
    onShownIdChanged: editing = false

    ConfirmDialog {
        id: deleteChannelDialog
        title: "Delete this channel?"
        message: panel.loaded ? "Delete \"" + panel.channels.channel.name + "\" and all its posts? This can't be undone." : ""
        confirmText: "Delete channel"
        danger: true
        onConfirmed: panel.channels.deleteChannel()
    }
    ConfirmDialog {
        id: visibilityDialog
        title: panel.pendingPrivate ? "Make this channel private?" : "Make this channel public?"
        message: panel.pendingPrivate
                 ? "It leaves Discover, and only people with its private link can read it. Followers who don't have that link lose access."
                 : "Anyone can find it in Discover and read every post."
        confirmText: panel.pendingPrivate ? "Make private" : "Make public"
        onConfirmed: panel.channels.setPrivate(panel.pendingPrivate)
    }
    ConfirmDialog {
        id: resetLinkDialog
        title: "Reset the private link?"
        message: "Everyone who has the current link, followers included, loses access until you share the new one."
        confirmText: "Reset link"
        danger: true
        onConfirmed: panel.channels.setPrivate(true)
    }
    FileDialog {
        id: photoDialog
        title: "Choose a photo for the channel"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Images (*.jpg *.jpeg *.png *.bmp *.gif)"]
        currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        onAccepted: panel.channels.setPhoto(selectedFile)
    }
    RvDialog {
        id: reportDialog
        title: "Report this channel"
        width: 460
        onOpened: {
            reason.text = ""
            reason.forceActiveFocus()
        }
        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 14
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "What's wrong with this channel? Your report goes to the RedVault admins."
                color: Theme.text2
            }
            PostField {
                id: reason
                Layout.fillWidth: true
                placeholderText: "What's wrong"
                Accessible.name: "Reason"
                limit: 1000
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                RvButton {
                    primary: false
                    text: "Cancel"
                    onClicked: reportDialog.close()
                }
                RvButton {
                    text: "Send report"
                    enabled: reason.text.trim().length > 0 && !reason.tooLong
                    onClicked: {
                        panel.channels.report(reason.text)
                        reportDialog.close()
                    }
                }
            }
        }
    }

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
            y: 12
            width: scroller.width - 40
            spacing: 12

            IconButton {
                Layout.alignment: Qt.AlignRight
                iconName: "x"
                tip: "Close channel info"
                onClicked: panel.closeRequested()
            }

            ChannelAvatar {
                Layout.alignment: Qt.AlignHCenter
                size: 80
                name: panel.loaded ? panel.channels.channel.name : ""
                photoKey: panel.loaded ? panel.channels.channel.photo : ""
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: panel.loaded ? panel.channels.channel.name : ""
                textFormat: Text.PlainText
                color: Theme.text
                font.family: Theme.displayFamily
                font.pixelSize: 22
                font.weight: Font.Bold
            }
            Text {
                objectName: "channelDisclosure"
                Layout.fillWidth: true
                Layout.topMargin: -6
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: panel.isPrivate ? "Private channel · only people with its link can read it · not end-to-end encrypted"
                                      : "Public channel · not end-to-end encrypted"
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: 13
            }
            TextEdit {
                Layout.fillWidth: true
                visible: text.length > 0 && !panel.editing
                text: panel.loaded ? panel.channels.channel.description : ""
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                color: Theme.text2
                selectionColor: Theme.accent
                selectedTextColor: Theme.accentContrast
                font.pixelSize: Theme.fontSize
            }

            Flow {
                Layout.fillWidth: true
                Layout.topMargin: 4
                visible: panel.loaded && !panel.editing
                spacing: 8
                RvButton {
                    primary: !panel.channels.isFollowing
                    secondary: panel.channels.isFollowing
                    iconName: panel.channels.isFollowing ? "check" : ""
                    text: panel.channels.isFollowing ? "Following" : "Follow"
                    Accessible.checked: panel.channels.isFollowing
                    onClicked: panel.channels.toggleFollow()
                }
                RvButton {
                    visible: panel.channels.isFollowing
                    secondary: true
                    iconName: panel.channels.isMuted ? "bell-off" : "bell"
                    text: panel.channels.isMuted ? "Muted" : "Notifications on"
                    Accessible.checked: !panel.channels.isMuted
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: panel.channels.isMuted ? "Notifications are off for this channel" : "You're notified of new posts"
                    onClicked: panel.channels.toggleMuted()
                }
                RvButton {
                    visible: panel.link.length > 0
                    secondary: true
                    iconName: "copy"
                    text: "Copy link"
                    onClicked: panel.channels.copyLink()
                }
                RvButton {
                    visible: !panel.canManage
                    primary: false
                    iconName: "flag"
                    text: "Report"
                    enabled: !panel.channels.busy
                    onClicked: reportDialog.open()
                }
                RvButton {
                    visible: panel.canManage
                    secondary: true
                    iconName: "pen"
                    text: "Edit"
                    onClicked: {
                        editName.text = panel.channels.channel.name
                        editDescription.text = panel.channels.channel.description
                        panel.editing = true
                    }
                }
                RvButton {
                    visible: panel.canManage
                    secondary: true
                    iconName: "image"
                    text: panel.loaded && panel.channels.channel.hasPhoto ? "Change photo" : "Add photo"
                    enabled: !panel.channels.busy
                    onClicked: photoDialog.open()
                }
                RvButton {
                    visible: panel.canManage && panel.loaded && panel.channels.channel.hasPhoto
                    primary: false
                    text: "Remove photo"
                    enabled: !panel.channels.busy
                    onClicked: panel.channels.removePhoto()
                }
                RvButton {
                    visible: panel.canManage
                    primary: false
                    danger: true
                    iconName: "trash"
                    text: "Delete channel"
                    enabled: !panel.channels.busy
                    onClicked: deleteChannelDialog.open()
                }
            }

            // --- Editing the name and description (the owner) ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: panel.loaded && panel.editing
                spacing: 10
                RvTextField {
                    id: editName
                    Layout.fillWidth: true
                    placeholderText: "Channel name"
                    maximumLength: panel.channels.maxNameLength
                    Accessible.name: "Channel name"
                }
                PostField {
                    id: editDescription
                    Layout.fillWidth: true
                    placeholderText: "What's it about? (optional)"
                    Accessible.name: "Description"
                    limit: panel.channels.maxDescriptionLength
                }
                RowLayout {
                    spacing: 8
                    RvButton {
                        text: "Save"
                        enabled: !panel.channels.busy && editName.text.trim().length > 0 && !editDescription.tooLong
                        onClicked: panel.channels.saveDetails(editName.text, editDescription.text)
                    }
                    RvButton {
                        secondary: true
                        text: "Cancel"
                        enabled: !panel.channels.busy
                        onClicked: panel.editing = false
                    }
                }
            }

            // --- Public or private, suggestions and the link (the owner) ---
            Caption {
                visible: panel.loaded && panel.canManage
                text: "Channel settings"
            }
            RvSwitch {
                id: privateSwitch
                Layout.fillWidth: true
                visible: panel.loaded && panel.channels.isMine
                label: "Private channel"
                description: panel.isPrivate ? "Not in Discover. Only people with the private link can read it."
                                             : "Off: anyone can find it in Discover and read it."
                checked: panel.isPrivate
                enabled: !panel.channels.busy
                Accessible.name: "Private channel"
                onToggled: (on) => {
                    // Nothing changes until it's confirmed and the server agrees.
                    privateSwitch.checked = Qt.binding(() => panel.isPrivate)
                    panel.pendingPrivate = on
                    visibilityDialog.open()
                }
            }
            RvSwitch {
                id: suggestionsSwitch
                Layout.fillWidth: true
                visible: panel.loaded && panel.canManage
                label: "Suggested posts"
                description: panel.loaded && panel.channels.channel.acceptSuggestions
                             ? "Followers can send you posts to review. You see the post, never who sent it."
                             : "Off: only you post in this channel."
                checked: panel.loaded && panel.channels.channel.acceptSuggestions === true
                enabled: !panel.channels.busy
                Accessible.name: "Suggested posts"
                onToggled: (on) => {
                    suggestionsSwitch.checked = Qt.binding(() => panel.loaded && panel.channels.channel.acceptSuggestions === true)
                    panel.channels.setAcceptSuggestions(on)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                visible: panel.loaded && panel.channels.isMine && panel.isPrivate && panel.link.length > 0
                spacing: 8
                RvTextField {
                    Layout.fillWidth: true
                    readOnly: true
                    text: panel.link
                    Accessible.name: "Private link"
                    // Show the link from its start, not scrolled to the end.
                    onTextChanged: cursorPosition = 0
                    onActiveFocusChanged: if (activeFocus) selectAll()
                }
                RvButton {
                    iconName: "copy"
                    text: "Copy"
                    onClicked: panel.channels.copyLink()
                }
            }
            RvButton {
                visible: panel.loaded && panel.channels.isMine && panel.isPrivate && panel.link.length > 0
                primary: false
                iconName: "refresh"
                text: "Reset link"
                enabled: !panel.channels.busy
                onClicked: resetLinkDialog.open()
            }
        }
    }
}
