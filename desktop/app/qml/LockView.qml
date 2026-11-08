// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Layouts
import Redvault

// Covers the app while it's locked. The chats underneath aren't even
// rendered (see Main.qml), so nothing shows through or can be read out.
Rectangle {
    id: root
    color: Theme.bg
    property bool confirmForget: false

    function submit() {
        if (!App.lock.busy && pinField.text.length > 0)
            App.lock.unlock(pinField.text)
    }

    Component.onCompleted: pinField.forceActiveFocus()

    Connections {
        target: App.lock
        function onErrorChanged() {
            if (App.lock.error.length > 0) {
                pinField.text = ""
                pinField.forceActiveFocus()
            }
        }
    }

    // Swallow clicks and wheel so nothing reaches the (hidden) app.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    Rectangle {
        anchors.centerIn: parent
        width: 360
        height: column.implicitHeight + 56
        radius: Theme.radiusCard
        color: Theme.surface
        border.color: Theme.border

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: 28
            spacing: 12

            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 52
                Layout.preferredHeight: 52
                radius: 14
                color: Theme.accent
                Icon {
                    anchors.centerIn: parent
                    name: "lock"
                    size: 26
                    color: Theme.accentContrast
                }
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "redvaultmessages is locked"
                color: Theme.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "Signed in as " + App.handle
                textFormat: Text.PlainText
                color: Theme.text2
                font.pixelSize: Theme.fontSmall + 1
            }

            RvTextField {
                id: pinField
                Layout.fillWidth: true
                placeholderText: "PIN"
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhDigitsOnly
                validator: RegularExpressionValidator { regularExpression: /[0-9]{0,12}/ }
                horizontalAlignment: TextInput.AlignHCenter
                font.pixelSize: 20
                font.letterSpacing: 4
                enabled: !App.lock.busy
                onAccepted: root.submit()
                // Submits on the last digit, like the phone.
                onTextChanged: if (text.length === App.lock.pinLength) root.submit()
            }

            Banner {
                Layout.fillWidth: true
                text: App.lock.error
            }

            RvButton {
                Layout.fillWidth: true
                text: App.lock.busy ? "Checking…" : "Unlock"
                enabled: !App.lock.busy && pinField.text.length > 0
                onClicked: root.submit()
            }

            RvButton {
                Layout.fillWidth: true
                visible: !root.confirmForget
                primary: false
                text: "Forgot your PIN?"
                onClicked: root.confirmForget = true
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.confirmForget
                spacing: 8
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Sign out to remove the PIN. You'll sign back in with your password; nothing is lost."
                    color: Theme.text2
                    font.pixelSize: Theme.fontSmall + 1
                }
                RvButton {
                    Layout.fillWidth: true
                    danger: true
                    primary: false
                    text: "Sign out and remove PIN"
                    onClicked: {
                        App.lock.disable()
                        App.signOut()
                    }
                }
            }
        }
    }
}
