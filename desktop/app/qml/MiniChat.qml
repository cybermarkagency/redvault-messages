// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The in-call chat panel: the call's chat (already the open chat) with a
// compact composer.
Item {
    id: root
    required property ChatSession session
    signal closeRequested()

    // This panel shows and sends through the older protocol only. A chat on device keys can't use it:
    // sending from here would drop to that protocol without anyone asking for it. ChatSession refuses too.
    readonly property bool deviceKeyChat: App.deviceKeys !== null
        && (App.deviceKeys.chatActive || App.deviceKeys.chatPinned(root.session.currentChatId))

    function send() {
        const text = input.text.trim()
        if (text.length === 0 || root.session.sending || root.deviceKeyChat)
            return
        root.session.sendMessage(text, "")
    }

    Connections {
        target: root.session
        function onSent(ok) {
            if (ok) input.text = ""
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            Layout.leftMargin: 18
            Layout.rightMargin: 10
            Text {
                Layout.fillWidth: true
                text: "Chat"
                color: Theme.text
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            IconButton {
                iconName: "close"
                tip: "Hide chat"
                onClicked: root.closeRequested()
            }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.session.messages
            spacing: 4
            topMargin: 10
            bottomMargin: 10
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            onCountChanged: Qt.callLater(positionViewAtEnd)
            Component.onCompleted: positionViewAtEnd()
            delegate: MessageDelegate {
                width: list.width
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            spacing: 8
            RvTextField {
                id: input
                objectName: "miniChatInput"
                Layout.fillWidth: true
                enabled: !root.deviceKeyChat
                placeholderText: root.deviceKeyChat ? "Device-key chat: send from the chat itself" : "Message"
                Accessible.name: "Message"
                onAccepted: root.send()
                onTextChanged: if (text.length > 0 && App.live) App.live.notifyTyping(root.session.currentChatId)
            }
            IconButton {
                iconName: "send"
                tip: "Send"
                iconColor: Theme.accent
                enabled: input.text.trim().length > 0 && !root.session.sending && !root.deviceKeyChat
                onClicked: root.send()
            }
        }
    }
}
