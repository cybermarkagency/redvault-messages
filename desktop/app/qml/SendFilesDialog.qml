pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Files dropped on a chat or pasted into it: a look before they're
// encrypted and sent, one after another.
RvDialog {
    id: dialog
    required property ChatSession chats
    property var files: []   // local file URLs
    // The files still to send once this dialog said yes (sent one at a time), and the chat they're for.
    property var queue: []
    property string queueChatId: ""
    // The chat is in device-key mode: its files go that way, never as an older-protocol message.
    property bool deviceKeys: false
    readonly property string activity: dialog.deviceKeys ? App.deviceKeys.chatFileActivity : dialog.chats.fileActivity
    onActivityChanged: if (dialog.activity.length === 0) Qt.callLater(dialog.sendNext)
    title: files.length > 1 ? "Send " + files.length + " files?" : "Send this file?"
    width: 460

    function openFor(urls) {
        dialog.files = urls
        open()
    }
    function discard() {
        for (const url of dialog.files) dialog.chats.discardPasted(url)
        dialog.files = []
    }
    function sendNext() {
        if (dialog.queue.length === 0 || dialog.activity.length > 0) return
        const next = dialog.queue[0]
        dialog.queue = dialog.queue.slice(1)
        if (dialog.deviceKeys) App.deviceKeys.sendFile(next)
        else dialog.chats.attachFile(next)
    }

    onRejected: discard()
    closePolicy: Popup.CloseOnEscape

    Connections {
        target: dialog.chats
        function onCurrentChatChanged() {
            // Files picked for one chat never go to another.
            if (dialog.queue.length === 0 || dialog.chats.currentChatId === dialog.queueChatId) return
            for (const url of dialog.queue) dialog.chats.discardPasted(url)
            dialog.queue = []
        }
    }

    contentItem: ColumnLayout {
        spacing: 12

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "To " + dialog.chats.currentTitle + ". Each file is encrypted on this computer before it's uploaded."
            textFormat: Text.PlainText
            color: Theme.text3
            font.pixelSize: 13
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(360, fileColumn.implicitHeight)
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                id: fileColumn
                width: parent.width
                spacing: 8
                Repeater {
                    model: dialog.files
                    delegate: RowLayout {
                        id: fileRow
                        required property string modelData
                        readonly property var info: dialog.chats.localFileInfo(modelData)
                        Layout.fillWidth: true
                        spacing: 10
                        Rectangle {
                            implicitWidth: fileRow.info.image ? 96 : 40
                            implicitHeight: fileRow.info.image ? 72 : 40
                            radius: 8
                            color: Theme.surface2
                            clip: true
                            Image {
                                anchors.fill: parent
                                visible: fileRow.info.image
                                // The user's own file on this computer.
                                source: fileRow.info.image ? fileRow.modelData : ""
                                sourceSize.width: 192
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                            }
                            Icon {
                                anchors.centerIn: parent
                                visible: !fileRow.info.image
                                name: "file"
                                size: 20
                                color: Theme.text2
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: fileRow.info.name
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: Theme.text
                            }
                            Text {
                                text: fileRow.info.size
                                textFormat: Text.PlainText
                                color: Theme.text3
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            RvButton {
                primary: false
                text: "Cancel"
                onClicked: dialog.reject()
            }
            RvButton {
                text: dialog.files.length > 1 ? "Send " + dialog.files.length + " files" : "Send"
                onClicked: {
                    dialog.queueChatId = dialog.chats.currentChatId
                    dialog.queue = dialog.queue.concat(dialog.files)
                    dialog.files = []
                    dialog.close()
                    dialog.sendNext()
                }
            }
        }
    }
}
