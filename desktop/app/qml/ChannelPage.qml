// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// One channel, laid out as a chat (ThreadView): a header bar, the posts with
// the oldest at the top and the newest at the bottom, and what's at the bottom
// of a chat. The owner gets a composer (ChannelComposer) with their scheduled
// posts and suggestions above it (ChannelOwnerExtras); a reader gets a bar with
// nothing to type in (ChannelReaderBar), since channels are one-way. Each post
// is the channel's (ChannelPostBubble): its name and photo, never who posted
// it. Everything else about the channel is in its info panel (ChannelInfoPanel).
//
// Channels aren't end-to-end encrypted. The header says so in words, and
// nothing here borrows a chat's locks, security rows or delivery marks.
Item {
    id: page
    required property ChannelsSession channels
    signal back()

    readonly property bool loaded: channels !== null && channels.channel.id !== undefined
    readonly property bool canManage: channels !== null && channels.canManage
    readonly property bool isPrivate: loaded && channels.channel.isPrivate === true
    property bool infoOpen: false
    /// Too narrow for the thread and the panel side by side: the panel takes over.
    readonly property bool infoCovers: width < 760
    property string deletingPostId: ""

    // Another channel opened, or back to the list: start again at its newest post, panel closed.
    readonly property string shownId: channels ? channels.channelId : ""
    onShownIdChanged: {
        infoOpen = false
        Qt.callLater(thread.positionViewAtBeginning)
    }
    Connections {
        target: page.channels
        // Your own post goes where you can see it, wherever you'd scrolled to.
        function onPosted() { Qt.callLater(thread.positionViewAtBeginning) }
    }

    ConfirmDialog {
        id: deletePostDialog
        title: "Delete this post?"
        message: "It's removed for everyone who follows the channel."
        confirmText: "Delete"
        danger: true
        onConfirmed: page.channels.deletePost(page.deletingPostId)
    }
    ForwardDialog {
        id: forwardDialog
        chats: App.chats
        onForwarded: page.channels.setNotice("Forwarded.")
    }
    ChannelPhotoViewer {
        id: photoViewer
        onSaveRequested: (media) => page.saveMedia(media)
    }
    // Saving a post's file (or a photo): streamed to where you choose.
    FileDialog {
        id: saveDialog
        property string url: ""
        title: "Save"
        fileMode: FileDialog.SaveFile
        currentFolder: StandardPaths.writableLocation(StandardPaths.DownloadLocation)
        onAccepted: page.channels.saveMedia(url, selectedFile)
    }
    function saveMedia(media) {
        saveDialog.url = media.url
        saveDialog.currentFile = saveDialog.currentFolder + "/" + encodeURIComponent(media.name)
        saveDialog.open()
    }
    function forward(post) {
        forwardDialog.openForChannelPost({
            text: post.body,
            channelId: page.channels.channel.id,
            postId: post.id,
            channelName: page.channels.channel.name
        })
    }
    function askDelete(postId) {
        page.deletingPostId = postId
        deletePostDialog.open()
    }
    // Links in posts open only after you've seen the whole address.
    ConfirmDialog {
        id: linkDialog
        property string url: ""
        title: "Open this link?"
        message: linkDialog.url + "\n\nIt opens in your browser. Check the address first: a link can look like one site and go to another."
        confirmText: "Open link"
        function ask(link) {
            // This app's own links open here.
            if (App.groupCalls && App.groupCalls.isOwnCallLink(link)) {
                page.channels.setNotice(App.groupCalls.joinLink(link))
                return
            }
            if (App.groupInvites && App.groupInvites.isOwnInviteLink(link)) {
                App.groupInvites.openInvite(link)
                return
            }
            if (page.channels.isOwnChannelLink(link)) {
                page.channels.openChannelLink(link)
                return
            }
            if (!/^https?:\/\//i.test(link)) return
            linkDialog.url = link
            open()
        }
        onConfirmed: {
            App.lock.expectExternalReturn()
            Qt.openUrlExternally(linkDialog.url)
        }
    }

    // Only for its hex() helper, to color the menu's icons.
    Icon {
        id: menuIcon
        visible: false
    }

    // What you can do with one post (the "more" button or a right-click).
    Menu {
        id: postMenu
        property var post: ({})
        function show(post) {
            postMenu.post = post
            popup()
        }
        MenuItem {
            text: "Forward…"
            icon.source: "image://icon/share/" + menuIcon.hex(Theme.text)
            visible: App.chats !== null
            height: visible ? implicitHeight : 0
            onTriggered: page.forward(postMenu.post)
        }
        MenuItem {
            text: "Copy text"
            icon.source: "image://icon/copy/" + menuIcon.hex(Theme.text)
            visible: (postMenu.post.body || "").length > 0
            height: visible ? implicitHeight : 0
            onTriggered: App.copySecret(postMenu.post.body)
        }
        MenuItem {
            text: "Delete post"
            icon.source: "image://icon/trash/" + menuIcon.hex(Theme.danger)
            visible: page.canManage
            height: visible ? implicitHeight : 0
            onTriggered: page.askDelete(postMenu.post.id)
        }
    }

    // A follower's suggested post, opened from the read-only bar.
    RvDialog {
        id: suggestDialog
        title: "Suggest a post"
        width: 520
        onOpened: suggestForm.focusBody()
        ChannelSuggestions {
            id: suggestForm
            anchors.left: parent.left
            anchors.right: parent.right
            channels: page.channels
            onLinkRequested: (url) => linkDialog.ask(url)
            onCancelled: suggestDialog.close()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !(page.infoOpen && page.infoCovers)
            spacing: 0

            // --- Header ---
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Theme.headerHeight
                color: Theme.bg

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 16
                    spacing: 10

                    IconButton {
                        iconName: "chevron-left"
                        tip: "Back to channels"
                        onClicked: page.back()
                    }
                    ChannelAvatar {
                        name: page.loaded ? page.channels.channel.name : ""
                        photoKey: page.loaded ? page.channels.channel.photo : ""
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        // Take the space that's left rather than asking for the text's full width.
                        Layout.preferredWidth: 0
                        spacing: 1
                        Text {
                            Layout.fillWidth: true
                            text: page.loaded ? page.channels.channel.name
                                              : page.channels.error.length > 0 ? "Channel" : "Loading…"
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.text
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                        }
                        // The honest disclosure: where a chat says who it's with, this says what it is.
                        Text {
                            objectName: "channelSubtitle"
                            Layout.fillWidth: true
                            visible: page.loaded
                            text: (page.isPrivate ? "Private channel" : "Public channel") + " · not end-to-end encrypted"
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.text3
                            font.pixelSize: Theme.fontSmall + 1
                        }
                    }
                    IconButton {
                        objectName: "channelInfoButton"
                        enabled: page.loaded
                        iconName: "info"
                        tip: page.infoOpen ? "Hide channel info" : "Channel info"
                        toggled: page.infoOpen
                        onClicked: page.infoOpen = !page.infoOpen
                    }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Theme.border
                }
            }

            // --- The posts: the model is newest first, and row 0 sits at the bottom ---
            ListView {
                id: thread
                objectName: "channelThread"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: page.channels.posts
                // New rows go in at the bottom and older ones on top without moving what's on screen.
                verticalLayoutDirection: ListView.BottomToTop
                spacing: 4
                topMargin: 12
                bottomMargin: 12
                // Posts keep their state (a player, a poll being picked) while nearby.
                cacheBuffer: 4000
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                // Past the last row, which here is the top of the thread.
                footer: Item {
                    width: thread.width
                    height: page.loaded && page.channels.hasOlder ? 44 : 0
                    visible: height > 0
                    RvButton {
                        objectName: "channelOlderButton"
                        anchors.centerIn: parent
                        primary: false
                        implicitHeight: 30
                        text: "Show older posts"
                        onClicked: page.channels.loadOlder()
                    }
                }

                delegate: ChannelPostBubble {
                    width: thread.width
                    session: page.channels
                    isOwner: page.canManage
                    canModerate: page.canManage
                    onLinkRequested: (url) => linkDialog.ask(url)
                    onPhotoRequested: (media) => photoViewer.show(media)
                    onSaveRequested: (media) => page.saveMedia(media)
                    onForwardRequested: (post) => page.forward(post)
                    onDeleteRequested: (postId) => page.askDelete(postId)
                    onMenuRequested: (post) => postMenu.show(post)
                }

                Text {
                    anchors.centerIn: parent
                    visible: page.loaded && thread.count === 0
                    text: "No posts yet."
                    color: Theme.text3
                }
            }

            // --- Problems and confirmations ---
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 18
                Layout.rightMargin: 12
                Layout.bottomMargin: 8
                visible: page.channels.error.length > 0 || page.channels.notice.length > 0
                Banner {
                    Layout.fillWidth: true
                    error: page.channels.error.length > 0
                    text: page.channels.error.length > 0 ? page.channels.error : page.channels.notice
                }
                RvButton {
                    primary: false
                    iconName: "x"
                    implicitHeight: 30
                    ToolTip.visible: hovered
                    ToolTip.text: "Dismiss"
                    onClicked: page.channels.clearMessages()
                }
            }

            // --- The bottom: the owner's composer, or a reader's bar with nothing to type in ---
            Loader {
                Layout.fillWidth: true
                active: page.loaded && page.canManage
                sourceComponent: Component {
                    ChannelOwnerExtras {
                        objectName: "channelOwnerExtras"
                        channels: page.channels
                        maxPanelHeight: Math.max(160, page.height * 0.45)
                        onLinkRequested: (url) => linkDialog.ask(url)
                        onPhotoRequested: (media) => photoViewer.show(media)
                        onSaveRequested: (media) => page.saveMedia(media)
                    }
                }
            }
            // Only ever made for the owner or a super admin: a reader's page has no text box or send button at all.
            Loader {
                Layout.fillWidth: true
                active: page.loaded && page.canManage
                sourceComponent: Component {
                    ChannelComposer {
                        objectName: "channelComposer"
                        channels: page.channels
                        maxPanelHeight: Math.max(160, page.height * 0.45)
                    }
                }
            }
            ChannelReaderBar {
                objectName: "channelReaderBar"
                Layout.fillWidth: true
                visible: page.loaded && !page.canManage
                channels: page.channels
                onSuggestRequested: {
                    suggestForm.start()
                    suggestDialog.open()
                }
            }
        }

        ChannelInfoPanel {
            objectName: "channelInfoPanel"
            Layout.fillHeight: true
            Layout.fillWidth: page.infoCovers
            Layout.preferredWidth: Theme.detailsWidth
            visible: page.infoOpen
            channels: page.channels
            onCloseRequested: page.infoOpen = false
        }
    }
}
