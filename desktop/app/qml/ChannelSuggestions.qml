// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// Suggested posts, as the web app's Suggestions: the owner reviews those
// waiting (each to publish, text editable, now or scheduled, or turn down),
// never seeing who sent them; a follower suggests a post (text and up to
// four photos) and sees what became of earlier ones. The page shows the
// owner's half in the panel above the composer, and a follower's in a dialog.
ColumnLayout {
    id: root
    required property ChannelsSession channels
    signal linkRequested(string url)
    /// A follower dropped the form.
    signal cancelled()
    spacing: 14

    readonly property bool loaded: channels !== null && channels.channel.id !== undefined
    readonly property bool accepting: loaded && channels.channel.acceptSuggestions === true
    property string editingId: ""
    property bool composing: false

    /// A follower: an empty form, ready to type in.
    function start() {
        root.channels.resetSuggestionForm()
        suggestionBody.text = ""
        root.composing = true
    }
    function focusBody() {
        if (root.composing) suggestionBody.forceActiveFocus()
    }

    Connections {
        target: root.channels
        function onSuggestionDecided() { root.editingId = "" }
        function onChannelChanged() {
            if (!root.channels.channelId || root.channels.channelId.length === 0) {
                root.editingId = ""
                root.composing = false
            }
        }
        function onSuggestionsChanged() {
            if (root.channels.suggestionSent) root.composing = false
        }
    }

    // --- The owner: suggestions waiting ---
    ColumnLayout {
        Layout.fillWidth: true
        visible: root.accepting && root.channels.canManage && root.channels.suggestions.length > 0
        spacing: 10
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "From followers. Published, they appear as the channel's, marked as suggested; who sent them is never shown."
            color: Theme.text2
            font.pixelSize: Theme.fontSmall + 1
        }
        Repeater {
            model: root.channels.suggestions
            delegate: Rectangle {
                id: suggestion
                required property var modelData
                readonly property bool editing: root.editingId === modelData.id
                Layout.fillWidth: true
                implicitHeight: suggestionColumn.implicitHeight + 24
                radius: Theme.radiusCard
                color: Theme.selected
                ColumnLayout {
                    id: suggestionColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    spacing: 8
                    ChannelPostContent {
                        Layout.fillWidth: true
                        session: root.channels
                        preview: true
                        post: ({ photos: suggestion.modelData.photos, others: [], body: suggestion.editing ? "" : suggestion.modelData.body,
                                 bodyRich: suggestion.editing ? "" : suggestion.modelData.bodyRich,
                                 bodyRevealed: suggestion.editing ? "" : suggestion.modelData.bodyRich, suggested: false })
                        onLinkRequested: (url) => root.linkRequested(url)
                    }
                    Text {
                        visible: !suggestion.editing
                        text: "Sent " + suggestion.modelData.sent
                        textFormat: Text.PlainText
                        color: Theme.text3
                        font.pixelSize: Theme.fontSmall
                    }
                    PostField {
                        id: editBody
                        Layout.fillWidth: true
                        visible: suggestion.editing
                        Accessible.name: "Edit before publishing"
                        limit: root.channels.maxPostLength
                    }
                    ChannelScheduleField {
                        id: editAt
                        Layout.fillWidth: true
                        visible: suggestion.editing
                        label: "Post at (optional)"
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        RvButton {
                            visible: !suggestion.editing
                            text: "Review and publish"
                            enabled: !root.channels.busy
                            onClicked: {
                                editBody.text = suggestion.modelData.body
                                editAt.clear()
                                root.editingId = suggestion.modelData.id
                            }
                        }
                        RvButton {
                            visible: !suggestion.editing
                            secondary: true
                            text: "Turn down"
                            enabled: !root.channels.busy
                            onClicked: root.channels.rejectSuggestion(suggestion.modelData.id)
                        }
                        RvButton {
                            visible: suggestion.editing
                            text: editAt.iso.length > 0 ? "Schedule" : "Publish"
                            enabled: !root.channels.busy && !editBody.tooLong && (!editAt.filled || editAt.iso.length > 0)
                            onClicked: root.channels.approveSuggestion(suggestion.modelData.id, editBody.text, editAt.iso)
                        }
                        RvButton {
                            visible: suggestion.editing
                            secondary: true
                            text: "Cancel"
                            enabled: !root.channels.busy
                            onClicked: root.editingId = ""
                        }
                    }
                }
            }
        }
    }

    // --- A follower: suggest a post ---
    ColumnLayout {
        Layout.fillWidth: true
        visible: root.accepting && !root.channels.canManage
        spacing: 10

        FileDialog {
            id: photoDialog
            title: "Add photos"
            fileMode: FileDialog.OpenFiles
            nameFilters: ["Photos (*.jpg *.jpeg *.png *.gif *.webp *.avif *.heic)"]
            currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
            onAccepted: root.channels.suggestionUploads.addFiles(selectedFiles)
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !root.composing
            spacing: 10
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: root.channels.suggestionSent ? "Sent. The owner will look at it."
                                                   : (root.channels.channel.name || "") + " takes suggested posts from followers."
                textFormat: Text.PlainText
                color: Theme.text2
            }
            RvButton {
                secondary: true
                text: "Suggest a post"
                onClicked: {
                    root.start()
                    root.focusBody()
                }
            }
        }

        PostField {
            id: suggestionBody
            Layout.fillWidth: true
            visible: root.composing
            placeholderText: "Your post: fan art, a review, news… *bold*, _italic_ and links work"
            Accessible.name: "Suggested post"
            limit: root.channels.maxPostLength
        }
        ChannelUploadList {
            Layout.fillWidth: true
            visible: root.composing && root.channels.suggestionUploads.count > 0
            uploads: root.channels.suggestionUploads
        }
        Text {
            Layout.fillWidth: true
            visible: root.composing
            wrapMode: Text.Wrap
            text: "The owner sees your post but not who sent it, and may edit it before publishing."
            color: Theme.text3
            font.pixelSize: Theme.fontSmall + 1
        }
        Text {
            Layout.fillWidth: true
            visible: root.composing && root.channels.suggestionError.length > 0
            wrapMode: Text.Wrap
            text: root.channels.suggestionError
            textFormat: Text.PlainText
            color: Theme.danger
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.composing
            spacing: 8
            Chip {
                text: "Photos"
                enabled: !root.channels.suggestionUploads.full
                onClicked: photoDialog.open()
            }
            RvButton {
                secondary: true
                text: "Cancel"
                onClicked: {
                    root.composing = false
                    root.channels.resetSuggestionForm()
                    root.cancelled()
                }
            }
            Item { Layout.fillWidth: true }
            RvButton {
                text: root.channels.busy ? "Sending…" : "Send suggestion"
                enabled: !root.channels.busy && !root.channels.suggestionUploads.busy && !suggestionBody.tooLong
                         && (suggestionBody.text.trim().length > 0 || root.channels.suggestionUploads.readyCount > 0)
                onClicked: root.channels.sendSuggestion(suggestionBody.text)
            }
        }

        // What became of earlier ones.
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.channels.mySuggestions.length > 0
            spacing: 4
            property bool open: false
            RvButton {
                primary: false
                implicitHeight: 28
                iconName: parent.open ? "chevron-down" : "chevron-right"
                text: "Your suggestions (" + root.channels.mySuggestions.length + ")"
                onClicked: parent.open = !parent.open
            }
            Repeater {
                model: parent.open ? root.channels.mySuggestions : []
                delegate: RowLayout {
                    id: mine
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: mine.modelData.body.length > 0 ? mine.modelData.body : "Photos"
                        textFormat: Text.PlainText
                        color: Theme.text
                    }
                    Text {
                        text: mine.modelData.status
                        textFormat: Text.PlainText
                        color: Theme.text3
                        font.pixelSize: Theme.fontSmall
                    }
                }
            }
        }
    }
}
