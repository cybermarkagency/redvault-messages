// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The form for one sticker of a story (a poll, quiz, question, emoji slider,
// countdown, link, mention, date or place). What it builds is checked by the
// same parser every receiving app uses (StoriesSession.checkSticker), so a
// sticker that gets this far will show.
Rectangle {
    id: editor
    required property StoriesSession stories
    /// "poll", "quiz", "question", "slider", "countdown", "link", "mention", "date" or "location".
    property string type: "poll"
    /// The sticker map being edited, or null for a new one.
    property var initial: null
    property string error: ""
    property int correct: 0
    property bool resultsEveryone: true
    /// A poll's voting deadline, in hours from now; 0 is when the story ends.
    property int closesInHours: 0
    property bool anonymous: false
    property string emoji: "😍"
    property string dateStyle: "datetime"
    property string mentionId: ""
    signal saved(var sticker)
    signal canceled()

    readonly property var titles: ({ poll: "Poll", quiz: "Quiz", question: "Question", slider: "Emoji slider", countdown: "Countdown",
                                     link: "Link", mention: "Mention", date: "Date or time", location: "Place" })
    readonly property var emojis: ["😍", "🔥", "😂", "👍", "❤️", "😮"]
    readonly property var deadlines: [{ label: "When the story ends", hours: 0 }, { label: "In 1 hour", hours: 1 }, { label: "In 6 hours", hours: 6 },
                                      { label: "In 12 hours", hours: 12 }, { label: "In 24 hours", hours: 24 }]
    readonly property var dateStyles: [{ label: "Date and time", value: "datetime" }, { label: "Date only", value: "date" }, { label: "Time only", value: "time" }]
    readonly property var friendChoices: {
        const out = [{ userId: "", handle: "Choose…" }]
        const list = editor.stories ? editor.stories.friends : []
        for (let i = 0; i < list.length; i++) if (list[i].isFriend) out.push(list[i])
        return out
    }
    readonly property bool asksQuestion: editor.type === "poll" || editor.type === "quiz" || editor.type === "question" || editor.type === "slider"
    readonly property bool hasOptions: editor.type === "poll" || editor.type === "quiz"
    readonly property bool hasResults: editor.type === "poll" || editor.type === "quiz" || editor.type === "slider"
    readonly property string anonymousText: "Answers come without a name. You won't be shown who wrote them. RedVault's server can still tell who sent one, and you may be able to guess from when it arrives."

    color: Theme.surface
    radius: Theme.radiusCard
    Accessible.role: Accessible.Dialog
    Accessible.name: (editor.titles[editor.type] || "Sticker") + " sticker"

    function pad2(n) {
        return (n < 10 ? "0" : "") + n
    }
    function localText(ms) {
        const d = new Date(ms)
        return d.getFullYear() + "-" + editor.pad2(d.getMonth() + 1) + "-" + editor.pad2(d.getDate()) + " " + editor.pad2(d.getHours()) + ":" + editor.pad2(d.getMinutes())
    }
    /// "2026-12-25 18:30" in this computer's time zone, as milliseconds; NaN if it isn't a date.
    function whenMs() {
        const m = /^\s*(\d{4})-(\d{1,2})-(\d{1,2})[ T](\d{1,2}):(\d{2})\s*$/.exec(whenField.text)
        if (!m) return NaN
        const d = new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3]), Number(m[4]), Number(m[5]))
        // new Date rolls 31 February into March: the typed day must be the day it came out as.
        return d.getMonth() === Number(m[2]) - 1 && d.getDate() === Number(m[3]) ? d.getTime() : NaN
    }

    /// Fills the form for a new sticker of `kind`, or for editing `sticker`.
    function open(kind, sticker) {
        const s = sticker || ({})
        editor.type = kind
        editor.initial = sticker || null
        editor.error = ""
        questionField.text = editor.asksQuestion && s.text ? s.text : ""
        titleField.text = kind === "countdown" && s.text ? s.text : ""
        labelField.text = (kind === "link" || kind === "location") && s.text ? s.text : ""
        urlField.text = kind === "link" && s.url ? s.url : "https://"
        explanationField.text = kind === "quiz" && s.explanation ? s.explanation : ""
        optionsModel.clear()
        const options = (kind === "poll" || kind === "quiz") && s.options && s.options.length >= 2 ? s.options : ["", ""]
        for (let i = 0; i < options.length; i++) optionsModel.append({ text: options[i] })
        editor.correct = kind === "quiz" && s.correct ? s.correct : 0
        editor.resultsEveryone = s.resultsEveryone !== false
        editor.closesInHours = 0
        editor.anonymous = kind === "question" && s.anonymous === true
        editor.emoji = kind === "slider" && s.emoji ? s.emoji : editor.emojis[0]
        whenField.text = editor.localText(s.when ? s.when : Date.now() + 24 * 3600 * 1000)
        editor.dateStyle = kind === "date" && s.style ? s.style : "datetime"
        editor.mentionId = kind === "mention" && s.userId ? s.userId : ""
    }

    function save() {
        const c = { type: editor.type }
        const s = editor.initial
        c.id = s ? s.id : ""
        c.x = s ? s.x : 0.5
        c.y = s ? s.y : 0.5
        c.scale = s ? s.scale : 1
        c.rotation = s ? s.rotation : 0
        switch (editor.type) {
        case "poll":
        case "quiz": {
            c.text = questionField.text.trim()
            const options = []
            for (let i = 0; i < optionsModel.count; i++) options.push(optionsModel.get(i).text.trim())
            c.options = options
            c.resultsEveryone = editor.resultsEveryone
            if (editor.type === "quiz") {
                c.correct = editor.correct
                c.explanation = explanationField.text.trim()
            } else if (editor.closesInHours > 0) {
                c.closesAt = Date.now() + editor.closesInHours * 3600 * 1000
            } else if (s && s.closesAt > 0) {
                c.closesAt = s.closesAt
            }
            break
        }
        case "question":
            c.text = questionField.text.trim()
            c.anonymous = editor.anonymous
            break
        case "slider":
            c.text = questionField.text.trim()
            c.emoji = editor.emoji
            c.resultsEveryone = editor.resultsEveryone
            break
        case "countdown":
            c.text = titleField.text.trim()
            c.when = editor.whenMs()
            if (isNaN(c.when)) { editor.error = "Type the time like 2026-12-25 18:30 (year-month-day hour:minute)."; return }
            break
        case "date":
            c.when = editor.whenMs()
            if (isNaN(c.when)) { editor.error = "Type the time like 2026-12-25 18:30 (year-month-day hour:minute)."; return }
            c.style = editor.dateStyle
            break
        case "link":
            c.url = urlField.text.trim()
            c.text = labelField.text.trim()
            break
        case "location":
            c.text = labelField.text.trim()
            break
        case "mention":
            c.userId = editor.mentionId
            break
        }
        const result = editor.stories.checkSticker(c)
        if (!result.ok) {
            editor.error = result.error
            return
        }
        editor.error = ""
        editor.saved(result.sticker)
    }

    ListModel { id: optionsModel }

    MouseArea { anchors.fill: parent }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        Text {
            Layout.fillWidth: true
            text: (editor.initial ? "Edit " : "New ") + (editor.titles[editor.type] || "sticker").toLowerCase()
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
        }

        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: scroll.availableWidth
                spacing: 10

                // The question (poll, quiz, question, slider)
                Field {
                    visible: editor.asksQuestion
                    label: editor.type === "question" ? "Your question" : editor.type === "slider" ? "Your prompt" : "Question"
                    RvTextField {
                        id: questionField
                        objectName: "stickerQuestion"
                        Layout.fillWidth: true
                        maximumLength: 120
                        Accessible.name: "Question"
                    }
                }

                // Options (poll, quiz)
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: editor.hasOptions
                    spacing: 6
                    Text { text: "Options"; color: Theme.text2; font.pixelSize: Theme.fontSmall }
                    Repeater {
                        model: optionsModel
                        delegate: RowLayout {
                            id: optionRow
                            required property int index
                            required property string text
                            Layout.fillWidth: true
                            spacing: 6
                            RadioButton {
                                objectName: "stickerCorrect" + optionRow.index
                                visible: editor.type === "quiz"
                                checked: editor.correct === optionRow.index
                                onClicked: editor.correct = optionRow.index
                                Accessible.name: "Option " + (optionRow.index + 1) + " is the right answer"
                            }
                            RvTextField {
                                objectName: "stickerOption" + optionRow.index
                                Layout.fillWidth: true
                                text: optionRow.text
                                maximumLength: 40
                                placeholderText: "Option " + (optionRow.index + 1)
                                Accessible.name: "Option " + (optionRow.index + 1)
                                onTextEdited: optionsModel.setProperty(optionRow.index, "text", text)
                            }
                            IconButton {
                                visible: optionsModel.count > 2
                                iconName: "close"
                                tip: "Remove option " + (optionRow.index + 1)
                                onClicked: {
                                    optionsModel.remove(optionRow.index)
                                    if (editor.correct >= optionsModel.count - 1 || editor.correct === optionRow.index) editor.correct = 0
                                }
                            }
                        }
                    }
                    RvButton {
                        visible: optionsModel.count < 4
                        secondary: true
                        text: "Add an option"
                        onClicked: optionsModel.append({ text: "" })
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: editor.type === "quiz"
                        text: "The right answer is revealed after someone answers. It isn't secret from their app, which holds the whole story."
                        wrapMode: Text.Wrap
                        color: Theme.text3
                        font.pixelSize: Theme.fontSmall
                    }
                }

                Field {
                    visible: editor.type === "quiz"
                    label: "Explanation (optional)"
                    RvTextField {
                        id: explanationField
                        objectName: "stickerExplanation"
                        Layout.fillWidth: true
                        maximumLength: 120
                    }
                }

                Field {
                    visible: editor.type === "poll"
                    label: "Voting closes"
                    note: "The deadline is exact in the app; the count uses times to the minute."
                    StoryCombo {
                        objectName: "stickerDeadline"
                        Layout.fillWidth: true
                        model: editor.deadlines
                        textRole: "label"
                        currentIndex: 0
                        onActivated: (index) => editor.closesInHours = editor.deadlines[index].hours
                        Accessible.name: "Voting closes"
                    }
                }

                Field {
                    visible: editor.hasResults
                    label: "Who sees the results"
                    note: "Totals only, never who chose what. They update while your app is open."
                    StoryCombo {
                        Layout.fillWidth: true
                        model: ["Everyone who can see the story", "Only me"]
                        currentIndex: editor.resultsEveryone ? 0 : 1
                        onActivated: (index) => editor.resultsEveryone = index === 0
                        Accessible.name: "Who sees the results"
                    }
                }

                CheckBox {
                    id: anonymousBox
                    Layout.fillWidth: true
                    visible: editor.type === "question"
                    checked: editor.anonymous
                    text: editor.anonymousText
                    onToggled: editor.anonymous = checked
                    contentItem: Text {
                        textFormat: Text.PlainText
                        leftPadding: anonymousBox.indicator.width + 8
                        text: editor.anonymousText
                        wrapMode: Text.Wrap
                        color: Theme.text2
                        font.pixelSize: Theme.fontSmall
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: editor.type === "slider"
                    spacing: 6
                    Text { text: "Emoji"; color: Theme.text2; font.pixelSize: Theme.fontSmall }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: editor.emojis
                            delegate: AbstractButton {
                                id: emojiChoice
                                required property string modelData
                                width: 44
                                height: 40
                                Accessible.name: "Emoji " + emojiChoice.modelData
                                Accessible.checked: editor.emoji === emojiChoice.modelData
                                background: Rectangle {
                                    radius: Theme.radiusSmall
                                    color: Theme.surface2
                                    border.width: editor.emoji === emojiChoice.modelData ? 2 : 1
                                    border.color: editor.emoji === emojiChoice.modelData ? Theme.accent : Theme.borderStrong
                                }
                                contentItem: Text { textFormat: Text.PlainText; text: emojiChoice.modelData; font.pixelSize: 22; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                onClicked: editor.emoji = emojiChoice.modelData
                            }
                        }
                    }
                }

                Field {
                    visible: editor.type === "countdown"
                    label: "What is it counting down to?"
                    RvTextField {
                        id: titleField
                        objectName: "stickerTitle"
                        Layout.fillWidth: true
                        maximumLength: 60
                    }
                }

                Field {
                    visible: editor.type === "countdown" || editor.type === "date"
                    label: "When"
                    note: "Year-month-day hour:minute, in this computer's time zone. Everyone sees it in theirs."
                    RvTextField {
                        id: whenField
                        objectName: "stickerWhen"
                        Layout.fillWidth: true
                        maximumLength: 16
                        placeholderText: "2026-12-25 18:30"
                        Accessible.name: "When"
                    }
                }

                Field {
                    visible: editor.type === "date"
                    label: "Show"
                    StoryCombo {
                        Layout.fillWidth: true
                        model: editor.dateStyles
                        textRole: "label"
                        currentIndex: editor.dateStyle === "date" ? 1 : editor.dateStyle === "time" ? 2 : 0
                        onActivated: (index) => editor.dateStyle = editor.dateStyles[index].value
                        Accessible.name: "Show"
                    }
                }

                Field {
                    visible: editor.type === "link"
                    label: "Web address"
                    RvTextField {
                        id: urlField
                        objectName: "stickerUrl"
                        Layout.fillWidth: true
                        maximumLength: 300
                        Accessible.name: "Web address"
                    }
                }

                Field {
                    visible: editor.type === "link" || editor.type === "location"
                    label: editor.type === "link" ? "Text on the sticker" : "Place name"
                    note: editor.type === "link" ? "Friends are asked before the link opens, and see the site it goes to."
                                                  : "Only what you type is shared. Your device's location is never read."
                    RvTextField {
                        id: labelField
                        objectName: "stickerLabel"
                        Layout.fillWidth: true
                        maximumLength: editor.type === "link" ? 40 : 60
                        placeholderText: editor.type === "link" ? "Defaults to the site name" : ""
                        Accessible.name: editor.type === "link" ? "Text on the sticker" : "Place name"
                    }
                }

                Field {
                    visible: editor.type === "mention"
                    label: "Friend"
                    note: "They only see it if they can see this story. Nobody is notified."
                    StoryCombo {
                        Layout.fillWidth: true
                        model: editor.friendChoices
                        textRole: "handle"
                        currentIndex: {
                            for (let i = 0; i < editor.friendChoices.length; i++) if (editor.friendChoices[i].userId === editor.mentionId) return i
                            return 0
                        }
                        onActivated: (index) => editor.mentionId = editor.friendChoices[index].userId
                        Accessible.name: "Friend"
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: editor.error.length > 0
            text: editor.error
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.danger
            font.pixelSize: Theme.fontSmall
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            RvButton { secondary: true; text: "Cancel"; onClicked: editor.canceled() }
            Item { Layout.fillWidth: true }
            RvButton { objectName: "stickerSave"; text: editor.initial ? "Save" : "Add"; onClicked: editor.save() }
        }
    }

    /// A labelled form field: a small caption, the control(s), and an optional note.
    component Field: ColumnLayout {
        property string label
        property string note
        default property alias controls: slot.data
        Layout.fillWidth: true
        spacing: 4
        Text { textFormat: Text.PlainText; text: label; color: Theme.text2; font.pixelSize: Theme.fontSmall }
        ColumnLayout { id: slot; Layout.fillWidth: true; spacing: 4 }
        Text {
            Layout.fillWidth: true
            visible: note.length > 0
            text: note
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.text3
            font.pixelSize: Theme.fontSmall
        }
    }
}
