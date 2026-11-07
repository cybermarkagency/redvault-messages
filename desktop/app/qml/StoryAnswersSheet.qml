pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The author's view of the answers sealed to them: totals for polls, quizzes
// and sliders, and each written answer (with who wrote it, unless it came
// without a name). Only this device can open them; the server never could.
StorySheet {
    id: sheet
    required property StoriesSession stories
    /// The story map (StoriesSession.storyMap) whose answers these are.
    property var story: null
    /// StoriesSession.interactionsLoaded for this story, or null while it's opening.
    property var interactions: null
    readonly property var answerable: {
        const out = []
        const list = sheet.story ? sheet.story.stickers : []
        for (let i = 0; i < list.length; i++) if (list[i].answerable) out.push(list[i])
        return out
    }
    readonly property int unreadable: sheet.interactions ? sheet.interactions.unreadable : 0

    title: "Answers"
    note: "Only you can open these. Polls, quizzes and sliders show totals, not who chose what. Totals reach your friends while your app is open."

    function percent(n, total) {
        return total > 0 ? Math.round(n / total * 100) : 0
    }
    function plural(n) {
        return n + (n === 1 ? " answer" : " answers")
    }

    Text {
        Layout.fillWidth: true
        visible: sheet.interactions === null
        text: "Opening answers…"
        color: Theme.text3
        font.pixelSize: Theme.fontSmall
    }
    Text {
        Layout.fillWidth: true
        visible: sheet.unreadable > 0
        text: sheet.unreadable + (sheet.unreadable === 1 ? " answer couldn't be opened and isn't" : " answers couldn't be opened and aren't") + " counted."
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.text3
        font.pixelSize: Theme.fontSmall
    }

    ScrollView {
        id: scroll
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(60, Math.min(answersColumn.implicitHeight, sheet.parent ? sheet.parent.height * 0.8 - 150 : 300))
        visible: sheet.interactions !== null
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            id: answersColumn
            width: scroll.availableWidth
            spacing: 14
            Repeater {
                model: sheet.answerable
                delegate: ColumnLayout {
                    id: block
                    required property var modelData
                    readonly property var totals: sheet.interactions && sheet.interactions.totals[block.modelData.id] !== undefined ? sheet.interactions.totals[block.modelData.id] : null
                    readonly property int total: block.totals ? block.totals.total : 0
                    readonly property bool ended: block.modelData.type === "poll" && block.modelData.closesAt > 0 && Date.now() >= block.modelData.closesAt
                    readonly property var entries: {
                        const out = []
                        const list = sheet.interactions ? sheet.interactions.entries : []
                        for (let i = 0; i < list.length; i++) if (list[i].stickerId === block.modelData.id) out.push(list[i])
                        return out
                    }
                    Layout.fillWidth: true
                    spacing: 6
                    Accessible.role: Accessible.Grouping
                    Accessible.name: block.modelData.text

                    Text {
                        Layout.fillWidth: true
                        text: block.modelData.text
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.text
                        font.pixelSize: Theme.fontSize
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: sheet.plural(block.total)
                              + (block.ended ? " · voting has ended" : "")
                              + (block.modelData.type === "quiz" ? " · " + (block.totals && block.totals.hasCorrect ? block.totals.correct : 0) + " right" : "")
                              + (block.modelData.type === "slider" && block.total > 0 && block.totals && block.totals.hasAverage ? " · average " + block.totals.average + "%" : "")
                        textFormat: Text.PlainText
                        color: Theme.text3
                        font.pixelSize: Theme.fontSmall
                    }
                    // Poll and quiz: a bar for each option.
                    Repeater {
                        model: block.modelData.type === "poll" || block.modelData.type === "quiz" ? block.modelData.options : []
                        delegate: RowLayout {
                            id: bar
                            required property string modelData
                            required property int index
                            readonly property int count: block.totals && block.totals.counts.length > bar.index ? block.totals.counts[bar.index] : 0
                            Layout.fillWidth: true
                            spacing: 8
                            Text {
                                Layout.preferredWidth: 110
                                text: bar.modelData + (block.modelData.type === "quiz" && block.modelData.correct === bar.index ? " ✓" : "")
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                color: Theme.text2
                                font.pixelSize: Theme.fontSmall
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                height: 8
                                radius: 4
                                color: Theme.surface3
                                Rectangle {
                                    width: parent.width * sheet.percent(bar.count, block.total) / 100
                                    height: parent.height
                                    radius: 4
                                    color: Theme.accent
                                }
                            }
                            Text {
                                Layout.preferredWidth: 74
                                horizontalAlignment: Text.AlignRight
                                text: bar.count + " · " + sheet.percent(bar.count, block.total) + "%"
                                textFormat: Text.PlainText
                                color: Theme.text2
                                font.pixelSize: Theme.fontSmall
                            }
                        }
                    }
                    // Question: every written answer, with a way to report an abusive one.
                    Text {
                        Layout.fillWidth: true
                        visible: block.modelData.type === "question" && block.entries.length === 0
                        text: "No answers yet."
                        color: Theme.text3
                        font.pixelSize: Theme.fontSmall
                    }
                    Repeater {
                        model: block.modelData.type === "question" ? block.entries : []
                        delegate: ColumnLayout {
                            id: row
                            required property var modelData
                            property bool open: false
                            property bool share: false
                            property bool sending: false
                            property string problem: ""
                            Layout.fillWidth: true
                            spacing: 6

                            Connections {
                                target: sheet.stories
                                function onReportDone(ok, message) {
                                    if (!row.sending) return
                                    row.sending = false
                                    if (ok) row.open = false
                                    else row.problem = message
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        Layout.fillWidth: true
                                        text: row.modelData.sender
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        color: Theme.text3
                                        font.pixelSize: Theme.fontSmall
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: row.modelData.text
                                        textFormat: Text.PlainText
                                        wrapMode: Text.Wrap
                                        color: Theme.text
                                        font.pixelSize: Theme.fontSize
                                    }
                                }
                                RvButton {
                                    secondary: true
                                    text: "Report"
                                    Accessible.name: "Report this answer"
                                    onClicked: row.open = !row.open
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: row.open
                                spacing: 6
                                RvTextField {
                                    id: reason
                                    Layout.fillWidth: true
                                    maximumLength: 300
                                    placeholderText: "What's wrong?"
                                    Accessible.name: "What's wrong with this answer?"
                                }
                                CheckBox {
                                    id: includeBox
                                    Layout.fillWidth: true
                                    checked: row.share
                                    text: "Include the answer's text in the report"
                                    onToggled: row.share = checked
                                    contentItem: Text {
                                        leftPadding: includeBox.indicator.width + 8
                                        text: "Include the answer's text in the report"
                                        color: Theme.text2
                                        font.pixelSize: Theme.fontSmall
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: row.problem.length > 0
                                    text: row.problem
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    color: Theme.danger
                                    font.pixelSize: Theme.fontSmall
                                }
                                RvButton {
                                    text: row.sending ? "Sending…" : "Send report"
                                    enabled: !row.sending
                                    onClicked: {
                                        row.problem = ""
                                        row.sending = true
                                        sheet.stories.reportAnswer(sheet.story.id, row.modelData.stickerId, row.modelData.envelope, reason.text, row.modelData.text, row.share)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
