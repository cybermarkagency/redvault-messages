// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Redvault

// Reporting a community or one of its messages, as the web app's
// CommunityReportDialog. Reporting never reads anything for you: for a
// message, the reporter chooses to disclose its decrypted text, and is told
// exactly who will see it. Unticked, only the category and the details go.
RvDialog {
    id: dialog
    required property CommunitySession communities
    property string communityId: ""
    /// "community" or "content" (a message).
    property string targetKind: "community"
    property string messageId: ""
    property string senderId: ""
    property string messageText: ""

    readonly property var categories: [
        { value: "spam", label: "Spam" },
        { value: "scam", label: "Scam or fraud" },
        { value: "harassment", label: "Harassment or hate" },
        { value: "impersonation", label: "Impersonation" },
        { value: "copyright", label: "Copyright" },
        { value: "refund_dispute", label: "Billing or refund problem" },
        { value: "child_safety", label: "Child safety", platformOnly: true },
        { value: "nonconsensual_imagery", label: "Intimate images shared without consent", platformOnly: true },
        { value: "illegal", label: "Other illegal content", platformOnly: true },
        { value: "other", label: "Something else" }
    ]
    readonly property bool platformOnly: targetKind === "community" || (categories.find(c => c.value === categoryBox.value) || {}).platformOnly === true
    property string error: ""

    /// Opens the dialog for the community itself.
    function reportCommunity(id) {
        communityId = id
        targetKind = "community"
        messageId = ""
        senderId = ""
        messageText = ""
        open()
    }

    /// Opens the dialog for one message of the community's chat.
    function reportMessage(id, message, sender, text) {
        communityId = id
        targetKind = "content"
        messageId = message
        senderId = sender
        messageText = text
        open()
    }

    title: targetKind === "community" ? "Report this community" : "Report this message"
    width: 500
    onOpened: {
        categoryBox.value = "spam"
        details.text = ""
        share.checked = false
        dialog.error = ""
    }

    Connections {
        target: dialog.communities
        function onReported(error) {
            if (!dialog.visible) return
            dialog.error = error
            if (error.length === 0) dialog.close()
        }
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 12

        Text {
            text: "What's wrong?"
            color: Theme.text2
            font.pixelSize: 13
        }
        RvComboBox {
            id: categoryBox
            Layout.fillWidth: true
            Accessible.name: "What's wrong"
            options: dialog.categories
            value: "spam"
        }
        Text {
            text: "Details (optional)"
            color: Theme.text2
            font.pixelSize: 13
        }
        PostField {
            id: details
            Layout.fillWidth: true
            Accessible.name: "Details"
            limit: 2000
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.targetKind === "content"
            spacing: 10
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: quote.implicitHeight + 20
                radius: Theme.radiusSmall
                color: Theme.surface2
                Text {
                    id: quote
                    anchors.fill: parent
                    anchors.margins: 10
                    text: dialog.messageText.slice(0, 600)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    maximumLineCount: 8
                    elide: Text.ElideRight
                    color: Theme.text2
                    font.pixelSize: 13
                }
            }
            ConsentBox {
                id: share
                Layout.fillWidth: true
                label: "Share this message's text with " + (dialog.platformOnly ? "RedVault's safety reviewers" : "this community's moderators")
                       + ". It's end-to-end encrypted, so they can only see it if you share it. Your name isn't shown to the community's moderators."
            }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.text3
            font.pixelSize: Theme.fontSmall + 1
            text: dialog.platformOnly ? "This goes to RedVault's own reviewers, not to the community's moderators."
                                      : "This goes to the community's moderators. Serious categories go to RedVault instead."
            textFormat: Text.PlainText
        }
        Banner {
            Layout.fillWidth: true
            text: dialog.error
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            RvButton {
                primary: false
                text: "Cancel"
                onClicked: dialog.close()
            }
            RvButton {
                danger: true
                text: "Send report"
                enabled: !dialog.communities.busy && !details.tooLong
                onClicked: dialog.communities.report(dialog.communityId, dialog.targetKind, dialog.senderId, categoryBox.value, details.text,
                                                     dialog.messageId, dialog.messageText, dialog.targetKind === "content" && share.checked)
            }
        }
    }
}
