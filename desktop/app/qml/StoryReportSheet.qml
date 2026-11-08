// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Report a story to RedVault's admins. Nothing is attached unless the reporter
// ticks the box, and the box shows exactly what would go. A photo or video is
// never sent: the server can't read it, and the app doesn't send it for you.
StorySheet {
    id: sheet
    required property StoriesSession stories
    /// The story map being reported.
    property var story: null
    property bool sending: false
    property string problem: ""
    readonly property var reasons: ["Spam", "Harassment or bullying", "Nudity or sexual content", "Violence or threats", "Scam or fraud", "Something else"]
    readonly property string evidence: sheet.story && sheet.visible ? sheet.stories.storyEvidence(sheet.story.id) : ""

    title: "Report this story"
    note: "Reports go to RedVault's admins. They can't open the story themselves: it's encrypted, and only you choose what to share."

    onVisibleChanged: {
        if (!sheet.visible) return
        sheet.sending = false
        sheet.problem = ""
        reason.currentIndex = 0
        detail.text = ""
        includeBox.checked = false
    }

    Connections {
        target: sheet.stories
        function onReportDone(ok, message) {
            if (!sheet.sending) return
            sheet.sending = false
            if (ok) sheet.closed()
            else sheet.problem = message
        }
    }

    Text { text: "What's wrong?"; color: Theme.text2; font.pixelSize: Theme.fontSmall }
    StoryCombo {
        id: reason
        Layout.fillWidth: true
        model: sheet.reasons
        Accessible.name: "What's wrong?"
    }
    Text { text: "Details (optional)"; color: Theme.text2; font.pixelSize: Theme.fontSmall }
    RvTextField {
        id: detail
        Layout.fillWidth: true
        maximumLength: 200
        Accessible.name: "Details"
    }
    CheckBox {
        id: includeBox
        Layout.fillWidth: true
        visible: sheet.evidence.length > 0
        text: "Include the words on this story in the report"
        contentItem: Text {
            leftPadding: includeBox.indicator.width + 8
            text: "Include the words on this story in the report"
            wrapMode: Text.Wrap
            color: Theme.text2
            font.pixelSize: Theme.fontSmall
            verticalAlignment: Text.AlignVCenter
        }
    }
    Text {
        Layout.fillWidth: true
        visible: includeBox.visible && includeBox.checked
        text: sheet.evidence
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.text3
        font.pixelSize: Theme.fontSmall
    }
    Text {
        Layout.fillWidth: true
        visible: sheet.evidence.length === 0
        text: "There's no text on this story to include."
        color: Theme.text3
        font.pixelSize: Theme.fontSmall
    }
    Text {
        Layout.fillWidth: true
        visible: sheet.problem.length > 0
        text: sheet.problem
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.danger
        font.pixelSize: Theme.fontSmall
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        RvButton { secondary: true; text: "Cancel"; onClicked: sheet.closed() }
        Item { Layout.fillWidth: true }
        RvButton {
            text: sheet.sending ? "Sending…" : "Send report"
            enabled: !sheet.sending
            onClicked: {
                sheet.problem = ""
                sheet.sending = true
                sheet.stories.reportStory(sheet.story.id, reason.currentText, detail.text, includeBox.visible && includeBox.checked)
            }
        }
    }
}
