// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// One sticker on a story, drawn as plain text and controls (never as markup
// from the story): a poll, quiz, question or slider to answer, or a countdown,
// link, mention, date or place. On a photo it is always a solid card, never
// text straight on the picture. Sizes are fractions of the story's width, like
// the web app's, so a sticker looks the same at any size. Where it sits is the
// parent's job (x and y are fractions of the frame, see StoryViewer).
Item {
    id: root
    required property var sticker
    /// The story frame's width.
    property real frameWidth: 360
    /// "view", or "edit": a preview in the composer with nothing to press.
    property string mode: "view"
    /// Your own story: you see the totals but don't answer.
    property bool own: false
    /// What you already answered: {answered, readable, anonymous, kind, choice, value, text}, or null.
    property var mine: null
    /// What the author published (yours: what you counted): {total, counts, hasCorrect, correct, hasAverage, average}, or null.
    property var totals: null
    /// Answered just now, before the server confirms.
    property bool busy: false
    /// Whether this person may answer: stickers are on, the story is still up, and it isn't theirs.
    property bool canAnswer: false
    readonly property bool hovered: hover.hovered
    /// The viewer's clock stops while someone is typing or dragging a slider.
    property bool typing: false
    property real now: Date.now()
    signal answer(var answer)
    signal linkRequested(string url)

    readonly property string kind: root.sticker.type
    readonly property bool isCard: root.kind === "poll" || root.kind === "quiz" || root.kind === "question" || root.kind === "slider" || root.kind === "countdown"
    readonly property real fs: Math.max(12, root.frameWidth * 0.036)
    readonly property real pad: Math.max(10, root.frameWidth * 0.026)
    readonly property real cardRadius: Math.max(14, root.frameWidth * 0.034)
    readonly property bool interactive: root.mode === "view" && !root.own && root.canAnswer
    readonly property bool hasMine: root.mine !== null && root.mine !== undefined && root.mine.answered === true
    readonly property color paper: "#F2FFFFFF"
    readonly property color ink: "#111111"
    readonly property color quiet: "#52525B"

    width: loader.item ? loader.item.width : 0
    height: loader.item ? loader.item.height : 0
    Accessible.role: Accessible.Grouping
    Accessible.name: root.label()

    function label() {
        const s = root.sticker
        switch (s.type) {
        case "poll": return "Poll: " + s.text
        case "quiz": return "Quiz: " + s.text
        case "question": return "Question: " + s.text
        case "slider": return "Slider: " + s.text
        case "countdown": return "Countdown: " + s.text
        case "link": return "Link: " + s.text
        case "mention": return "Mention"
        case "date": return "Date"
        case "location": return "Place: " + s.text
        }
        return "Sticker"
    }
    function percent(n, total) {
        return total > 0 ? Math.round(n / total * 100) : 0
    }
    function plural(n) {
        return n + (n === 1 ? " answer" : " answers")
    }
    /// "2d 4h", "3h 12m", "45s": time left until `endsAt`, or "" once it has passed.
    function timeLeft(endsAt) {
        const ms = endsAt - root.now
        if (ms <= 0) return ""
        const s = Math.floor(ms / 1000)
        const d = Math.floor(s / 86400)
        const h = Math.floor((s % 86400) / 3600)
        const m = Math.floor((s % 3600) / 60)
        if (d > 0) return d + "d " + h + "h"
        if (h > 0) return h + "h " + m + "m"
        if (m > 0) return m + "m " + (s % 60) + "s"
        return s + "s"
    }
    function whenText() {
        const d = new Date(root.sticker.when)
        const date = Qt.formatDate(d, "ddd, d MMM yyyy")
        const time = d.toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
        return root.sticker.style === "date" ? date : root.sticker.style === "time" ? time : date + " · " + time
    }

    HoverHandler { id: hover }
    // A press on the card, but not on one of its controls, doesn't reach the story behind (which would go to the next one).
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

    // The clock for countdowns and poll deadlines.
    Timer {
        interval: 1000
        repeat: true
        running: root.visible && (root.kind === "countdown" || (root.kind === "poll" && root.sticker.closesAt > 0))
        onTriggered: root.now = Date.now()
    }

    Loader {
        id: loader
        sourceComponent: root.kind === "poll" || root.kind === "quiz" ? choiceCard
                       : root.kind === "question" ? questionCard
                       : root.kind === "slider" ? sliderCard
                       : root.kind === "countdown" ? countdownCard
                       : root.kind === "link" ? linkPill
                       : textPill
    }

    // --- Poll and quiz ---
    Component {
        id: choiceCard
        Rectangle {
            id: card
            readonly property bool quiz: root.kind === "quiz"
            readonly property bool closed: root.kind === "poll" && root.sticker.closesAt > 0 && root.now >= root.sticker.closesAt
            readonly property int choice: root.hasMine && root.mine.readable ? root.mine.choice : -1
            readonly property var counts: root.totals && root.totals.counts ? root.totals.counts : []
            readonly property int total: root.totals ? root.totals.total : 0
            // Totals go to the author, and to those who answered once the author chose to show them: nobody is nudged before voting.
            readonly property bool showTotals: card.counts.length > 0 && card.total > 0 && (root.own || (root.sticker.resultsEveryone && root.hasMine))
            readonly property bool reveal: card.quiz && (card.choice >= 0 || root.own)
            readonly property bool canPress: root.interactive && !root.busy && !card.closed && !(card.quiz && root.hasMine)
            readonly property string note: card.closed ? "Voting has ended"
                : root.own ? (root.totals ? root.plural(card.total) + (card.quiz && root.totals.hasCorrect ? " · " + root.totals.correct + " right" : "") : "No answers yet")
                : root.hasMine ? (card.quiz ? (card.choice === root.sticker.correct ? "Correct!" : "Not quite") : "Your vote is in. You can change it.")
                : !root.sticker.resultsEveryone ? "Only the author sees the results" : ""

            width: Math.round(root.frameWidth * 0.64)
            height: choiceColumn.implicitHeight + root.pad * 2
            radius: root.cardRadius
            color: root.paper

            ColumnLayout {
                id: choiceColumn
                x: root.pad
                y: root.pad
                width: parent.width - root.pad * 2
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: root.sticker.text
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.ink
                    font.pixelSize: root.fs
                    font.weight: Font.Bold
                }
                Repeater {
                    model: root.sticker.options
                    delegate: AbstractButton {
                        id: option
                        required property string modelData
                        required property int index
                        readonly property bool isPicked: card.choice === option.index
                        readonly property bool isRight: card.reveal && card.quiz && root.sticker.correct === option.index
                        readonly property bool isWrong: card.reveal && option.isPicked && card.quiz && root.sticker.correct !== option.index
                        readonly property int share: card.showTotals ? root.percent(card.counts[option.index], card.total) : 0
                        Layout.fillWidth: true
                        implicitHeight: Math.max(Math.round(root.fs * 2.6), optionText.implicitHeight + 16)
                        enabled: card.canPress
                        Accessible.name: option.modelData
                        Accessible.checked: option.isPicked
                        background: Rectangle {
                            radius: 12
                            clip: true
                            color: option.isRight ? "#DCFCE7" : option.isWrong ? "#FEE2E2" : "#F4F4F5"
                            border.width: 2
                            border.color: option.isRight ? "#16A34A" : option.isWrong ? "#DC2626" : option.isPicked ? "#2563EB" : "#D4D4D8"
                            Rectangle {
                                visible: card.showTotals || (card.reveal && root.totals !== null)
                                height: parent.height
                                width: parent.width * option.share / 100
                                color: option.isPicked || option.isRight ? "#93C5FD" : "#D4D4D8"
                                Behavior on width { NumberAnimation { duration: 300 } }
                            }
                        }
                        contentItem: RowLayout {
                            spacing: 8
                            Text {
                                id: optionText
                                Layout.fillWidth: true
                                Layout.leftMargin: 10
                                text: option.modelData
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                                color: root.ink
                                font.pixelSize: root.fs
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.rightMargin: 10
                                visible: card.showTotals
                                text: option.share + "%"
                                textFormat: Text.PlainText
                                color: root.ink
                                font.pixelSize: root.fs
                                font.weight: Font.DemiBold
                            }
                        }
                        onClicked: root.answer({ choice: option.index })
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: card.quiz && card.reveal && root.sticker.explanation.length > 0
                    text: root.sticker.explanation
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.quiet
                    font.pixelSize: root.fs * 0.85
                }
                Text {
                    Layout.fillWidth: true
                    visible: card.note.length > 0
                    text: card.note
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.quiet
                    font.pixelSize: root.fs * 0.85
                }
            }
        }
    }

    // --- Question ---
    Component {
        id: questionCard
        Rectangle {
            id: card
            width: Math.round(root.frameWidth * 0.64)
            height: questionColumn.implicitHeight + root.pad * 2
            radius: root.cardRadius
            color: root.paper

            function send() {
                const t = field.text.trim()
                if (t.length > 0 && root.interactive && !root.busy) root.answer({ text: t })
            }

            ColumnLayout {
                id: questionColumn
                x: root.pad
                y: root.pad
                width: parent.width - root.pad * 2
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: root.sticker.text
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.ink
                    font.pixelSize: root.fs
                    font.weight: Font.Bold
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.hasMine
                    text: "Sent ✓" + (root.sticker.anonymous ? " · your name isn't shown to the author" : "")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.quiet
                    font.pixelSize: root.fs * 0.85
                }
                Text {
                    Layout.fillWidth: true
                    visible: !root.hasMine && root.own
                    text: root.totals ? root.plural(root.totals.total) : "Answers appear in Activity"
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.quiet
                    font.pixelSize: root.fs * 0.85
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: !root.hasMine && !root.own
                    spacing: 6
                    TextField {
                        id: field
                        Layout.fillWidth: true
                        implicitHeight: Math.round(root.fs * 2.6)
                        enabled: root.interactive
                        maximumLength: 500
                        placeholderText: "Type your answer"
                        placeholderTextColor: "#71717A"
                        color: root.ink
                        font.pixelSize: root.fs
                        leftPadding: 10
                        rightPadding: 10
                        selectByMouse: true
                        Accessible.name: "Your answer to: " + root.sticker.text
                        onActiveFocusChanged: root.typing = field.activeFocus
                        onAccepted: card.send()
                        background: Rectangle {
                            radius: 12
                            color: "#FFFFFF"
                            border.width: field.activeFocus ? 2 : 1
                            border.color: field.activeFocus ? "#2563EB" : "#D4D4D8"
                        }
                    }
                    Button {
                        id: sendButton
                        implicitHeight: Math.round(root.fs * 2.6)
                        enabled: root.interactive && !root.busy && field.text.trim().length > 0
                        text: "Send"
                        Accessible.name: "Send answer"
                        onClicked: card.send()
                        background: Rectangle { radius: 12; color: root.ink; opacity: sendButton.enabled ? 1 : 0.45 }
                        contentItem: Text {
                            textFormat: Text.PlainText
                            text: sendButton.text
                            color: "#FFFFFF"
                            font.pixelSize: root.fs
                            font.weight: Font.Bold
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: !root.hasMine && !root.own && root.sticker.anonymous
                    text: "Sent without your name. RedVault's server can still tell it was you."
                    wrapMode: Text.Wrap
                    color: root.quiet
                    font.pixelSize: root.fs * 0.85
                }
            }
        }
    }

    // --- Emoji slider ---
    Component {
        id: sliderCard
        Rectangle {
            id: card
            readonly property bool shows: root.totals !== null && root.totals.hasAverage && root.totals.total > 0 && (root.own || (root.sticker.resultsEveryone && root.hasMine))
            readonly property int sentValue: root.hasMine && root.mine.readable ? root.mine.value : -1
            property int shown: 50
            readonly property string note: root.own ? (root.totals ? root.plural(root.totals.total) + (card.shows ? " · average " + root.totals.average + "%" : "") : "No answers yet")
                : card.shows ? "Average " + root.totals.average + "% from " + root.totals.total
                : root.hasMine ? "Sent ✓"
                : !root.sticker.resultsEveryone ? "Only the author sees the results" : ""

            width: Math.round(root.frameWidth * 0.64)
            height: sliderColumn.implicitHeight + root.pad * 2
            radius: root.cardRadius
            color: root.paper

            function send() {
                debounce.stop()
                root.typing = false
                if (root.interactive && !root.busy) root.answer({ value: Math.round(slider.value) })
            }
            onSentValueChanged: if (card.sentValue >= 0) card.shown = card.sentValue

            Timer { id: debounce; interval: 600; onTriggered: card.send() }

            ColumnLayout {
                id: sliderColumn
                x: root.pad
                y: root.pad
                width: parent.width - root.pad * 2
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: root.sticker.text
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.ink
                    font.pixelSize: root.fs
                    font.weight: Font.Bold
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Slider {
                        id: slider
                        Layout.fillWidth: true
                        from: 0
                        to: 100
                        stepSize: 1
                        value: card.shown
                        enabled: root.interactive && !root.busy
                        Accessible.name: root.sticker.text + " " + Math.round(slider.value) + "%"
                        onMoved: {
                            card.shown = Math.round(slider.value)
                            root.typing = true
                            if (!slider.pressed) debounce.restart()
                        }
                        onPressedChanged: {
                            if (slider.pressed) root.typing = true
                            else card.send()
                        }
                        background: Rectangle {
                            x: slider.leftPadding
                            y: slider.topPadding + slider.availableHeight / 2 - height / 2
                            width: slider.availableWidth
                            height: 6
                            radius: 3
                            color: "#D4D4D8"
                            Rectangle { width: slider.visualPosition * parent.width; height: parent.height; radius: 3; color: "#2563EB" }
                        }
                        handle: Rectangle {
                            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
                            y: slider.topPadding + slider.availableHeight / 2 - height / 2
                            width: 22
                            height: 22
                            radius: 11
                            color: "#FFFFFF"
                            border.width: 2
                            border.color: slider.activeFocus ? "#111111" : "#2563EB"
                        }
                    }
                    Text {
                        text: root.sticker.emoji
                        textFormat: Text.PlainText
                        font.pixelSize: root.fs * 1.8
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: card.note.length > 0
                    text: card.note
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.quiet
                    font.pixelSize: root.fs * 0.85
                }
            }
        }
    }

    // --- Countdown ---
    Component {
        id: countdownCard
        Rectangle {
            width: Math.round(root.frameWidth * 0.64)
            height: countdownColumn.implicitHeight + root.pad * 2
            radius: root.cardRadius
            color: root.paper
            ColumnLayout {
                id: countdownColumn
                x: root.pad
                y: root.pad
                width: parent.width - root.pad * 2
                spacing: 4
                Text {
                    Layout.fillWidth: true
                    text: root.sticker.text
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.ink
                    font.pixelSize: root.fs
                    font.weight: Font.Bold
                }
                Text {
                    Layout.fillWidth: true
                    readonly property string remaining: root.timeLeft(root.sticker.when)
                    text: remaining.length > 0 ? remaining : "It's time!"
                    textFormat: Text.PlainText
                    color: root.ink
                    font.pixelSize: root.fs * 1.6
                    font.weight: Font.ExtraBold
                }
            }
        }
    }

    // --- A link: the viewer asks before leaving ---
    Component {
        id: linkPill
        AbstractButton {
            id: pill
            implicitWidth: Math.min(Math.round(root.frameWidth * 0.86), pillText.implicitWidth + 28)
            implicitHeight: Math.max(Math.round(root.fs * 2.6), pillText.implicitHeight + 16)
            width: implicitWidth
            height: implicitHeight
            enabled: root.mode === "view"
            Accessible.name: "Link: " + root.sticker.text
            background: Rectangle { radius: height / 2; color: root.paper }
            contentItem: Text {
                id: pillText
                text: "🔗 " + root.sticker.text
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: root.ink
                font.pixelSize: root.fs
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: root.linkRequested(root.sticker.url)
        }
    }

    // --- A mention, a date or a place ---
    Component {
        id: textPill
        Rectangle {
            implicitWidth: Math.min(Math.round(root.frameWidth * 0.86), plainText.implicitWidth + 28)
            implicitHeight: Math.max(Math.round(root.fs * 2.6), plainText.implicitHeight + 16)
            width: implicitWidth
            height: implicitHeight
            radius: height / 2
            color: root.paper
            Text {
                id: plainText
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - 28)
                text: root.kind === "mention" ? "@" + (root.sticker.handle.length > 0 ? root.sticker.handle : "someone")
                    : root.kind === "date" ? root.whenText()
                    : "📍 " + root.sticker.text
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: root.ink
                font.pixelSize: root.fs
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
