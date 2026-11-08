// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The owner's work that isn't in the thread yet, in a compact strip above the
// composer: scheduled posts (Publish now, Delete) and suggestions to review.
// A chip opens its list in a panel over the end of the thread's space.
Rectangle {
    id: extras
    required property ChannelsSession channels
    /// How tall the open panel may get.
    property int maxPanelHeight: 320
    signal linkRequested(string url)
    signal photoRequested(var media)
    signal saveRequested(var media)

    /// "", "scheduled" or "suggestions".
    property string section: ""
    readonly property int scheduledCount: channels.scheduled.length
    readonly property int suggestionCount: channels.channel.acceptSuggestions === true ? channels.suggestions.length : 0
    readonly property bool panelOpen: (section === "scheduled" && scheduledCount > 0) || (section === "suggestions" && suggestionCount > 0)

    visible: scheduledCount > 0 || suggestionCount > 0
    implicitHeight: visible ? column.implicitHeight : 0
    color: Theme.bg

    ConfirmDialog {
        id: deleteScheduledDialog
        property string postId: ""
        title: "Delete this scheduled post?"
        message: "It won't be published."
        confirmText: "Delete"
        danger: true
        onConfirmed: extras.channels.deleteScheduled(postId)
    }

    ColumnLayout {
        id: column
        width: parent.width
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }

        Flow {
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 12
            Layout.topMargin: 8
            Layout.bottomMargin: 8
            spacing: 6
            Chip {
                objectName: "channelScheduledChip"
                visible: extras.scheduledCount > 0
                text: "Scheduled (" + extras.scheduledCount + ")"
                checked: extras.panelOpen && extras.section === "scheduled"
                onClicked: extras.section = extras.section === "scheduled" ? "" : "scheduled"
            }
            Chip {
                objectName: "channelSuggestionsChip"
                visible: extras.suggestionCount > 0
                text: "Suggestions (" + extras.suggestionCount + ")"
                checked: extras.panelOpen && extras.section === "suggestions"
                onClicked: extras.section = extras.section === "suggestions" ? "" : "suggestions"
            }
        }

        Flickable {
            id: panel
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(panelColumn.implicitHeight + 8, extras.maxPanelHeight)
            visible: extras.panelOpen
            clip: true
            contentWidth: width
            contentHeight: panelColumn.implicitHeight + 8
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: panelColumn
                x: 18
                width: panel.width - 36
                spacing: 10

                // --- Scheduled posts ---
                Repeater {
                    model: extras.section === "scheduled" ? extras.channels.scheduled : []
                    delegate: Rectangle {
                        id: scheduledPost
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: scheduledColumn.implicitHeight + 24
                        radius: Theme.radiusCard
                        color: Theme.selected
                        ColumnLayout {
                            id: scheduledColumn
                            x: 12
                            y: 12
                            width: parent.width - 24
                            spacing: 8
                            Text {
                                text: scheduledPost.modelData.scheduledFor
                                textFormat: Text.PlainText
                                color: Theme.warn
                                font.pixelSize: Theme.fontSmall
                                font.weight: Font.DemiBold
                            }
                            ChannelPostContent {
                                Layout.fillWidth: true
                                post: scheduledPost.modelData
                                session: extras.channels
                                isOwner: true
                                preview: true
                                onLinkRequested: (url) => extras.linkRequested(url)
                                onPhotoRequested: (media) => extras.photoRequested(media)
                                onSaveRequested: (media) => extras.saveRequested(media)
                            }
                            RowLayout {
                                spacing: 8
                                RvButton {
                                    secondary: true
                                    implicitHeight: 32
                                    text: "Publish now"
                                    enabled: !extras.channels.busy
                                    onClicked: extras.channels.publishScheduled(scheduledPost.modelData.id)
                                }
                                RvButton {
                                    primary: false
                                    danger: true
                                    implicitHeight: 32
                                    text: "Delete"
                                    enabled: !extras.channels.busy
                                    onClicked: {
                                        deleteScheduledDialog.postId = scheduledPost.modelData.id
                                        deleteScheduledDialog.open()
                                    }
                                }
                            }
                        }
                    }
                }

                // --- Suggestions to review ---
                ChannelSuggestions {
                    Layout.fillWidth: true
                    visible: extras.section === "suggestions"
                    channels: extras.channels
                    onLinkRequested: (url) => extras.linkRequested(url)
                }
            }
        }
    }
}
