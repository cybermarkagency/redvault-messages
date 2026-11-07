pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A new poll for the open chat: a question, 2-10 options, one or several answers.
RvDialog {
    id: dialog
    required property ChatSession chats
    property var options: ["", ""]
    property string error: ""
    property bool busy: false
    title: "New poll"
    width: 460

    function reset() {
        question.text = ""
        dialog.options = ["", ""]
        multiple.checked = false
        dialog.error = ""
        dialog.busy = false
    }
    onOpened: {
        reset()
        question.forceActiveFocus()
    }

    Connections {
        target: dialog.chats
        function onPollDone(error) {
            dialog.busy = false
            if (error.length > 0) dialog.error = error
            else dialog.close()
        }
    }

    contentItem: ColumnLayout {
        spacing: 10

        RvTextField {
            id: question
            Layout.fillWidth: true
            placeholderText: "Ask a question"
            maximumLength: 300
        }
        Text {
            text: "Options"
            color: Theme.text3
            font.pixelSize: Theme.fontSmall
            font.weight: Font.DemiBold
        }
        Repeater {
            model: dialog.options.length
            delegate: RowLayout {
                id: optionRow
                required property int index
                Layout.fillWidth: true
                spacing: 6
                RvTextField {
                    Layout.fillWidth: true
                    placeholderText: "Option " + (optionRow.index + 1)
                    maximumLength: 100
                    text: dialog.options[optionRow.index]
                    onTextEdited: {
                        const next = dialog.options.slice()
                        next[optionRow.index] = text
                        dialog.options = next
                    }
                }
                IconButton {
                    visible: dialog.options.length > 2
                    iconName: "x"
                    tip: "Remove this option"
                    onClicked: {
                        const next = dialog.options.slice()
                        next.splice(optionRow.index, 1)
                        dialog.options = next
                    }
                }
            }
        }
        RvButton {
            visible: dialog.options.length < 10
            primary: false
            iconName: "plus"
            text: "Add an option"
            onClicked: dialog.options = dialog.options.concat([""])
        }
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: "Allow more than one answer"
                color: Theme.text
            }
            RvSwitch {
                id: multiple
            }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "Like your messages, the poll and every vote are end-to-end encrypted. Everyone in the chat can see who voted for what."
            color: Theme.text3
            font.pixelSize: 12
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
                text: dialog.busy ? "Sending…" : "Send poll"
                enabled: !dialog.busy && question.text.trim().length > 0
                         && dialog.options.filter(o => o.trim().length > 0).length >= 2
                onClicked: {
                    dialog.busy = true
                    dialog.error = ""
                    dialog.chats.createPoll(question.text, dialog.options.filter(o => o.trim().length > 0), multiple.checked)
                }
            }
        }
    }
}
