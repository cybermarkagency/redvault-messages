pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Settings › Linked devices: every sign-in to this account. The server keeps
// only an id and when each expires; names are encrypted by your devices.
ColumnLayout {
    id: root
    required property LinkedDevices devices
    property bool editing: false
    property string confirmId: ""
    spacing: 12

    Text {
        text: "Linked devices"
        color: Theme.text
        font.family: Theme.displayFamily
        font.pixelSize: 26
        font.weight: Font.Bold
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: "Everywhere you're signed in. The server keeps no IP address, device details or sign-in time for any of them: only when each sign-in expires. Device names are encrypted by your own devices."
        color: Theme.text2
        font.pixelSize: 14
    }
    Banner {
        Layout.fillWidth: true
        text: root.devices ? root.devices.error : ""
    }

    Card {
        Layout.fillWidth: true
        spacing: 0

        Text {
            visible: root.devices !== null && !root.devices.loaded && root.devices.error.length === 0
            text: "Loading…"
            color: Theme.text3
        }

        Repeater {
            model: root.devices ? root.devices.devices : []
            delegate: ColumnLayout {
                id: device
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: 0

                Rectangle {
                    visible: device.index > 0
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.bottomMargin: 4
                    implicitHeight: 1
                    color: Theme.border
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumHeight: 56
                    spacing: 12
                    Rectangle {
                        implicitWidth: 40
                        implicitHeight: 40
                        radius: 12
                        color: Theme.surface2
                        Icon {
                            anchors.centerIn: parent
                            name: "monitor"
                            size: 20
                            color: Theme.text2
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 0
                        spacing: 2
                        visible: !(device.modelData.current && root.editing)
                        RowLayout {
                            spacing: 8
                            Text {
                                text: device.modelData.name.length > 0 ? device.modelData.name : "Unnamed device"
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.weight: Font.Medium
                            }
                            Rectangle {
                                visible: device.modelData.current
                                implicitWidth: thisText.implicitWidth + 14
                                implicitHeight: 22
                                radius: 11
                                color: Theme.accentSoft
                                Text {
                                    id: thisText
                                    anchors.centerIn: parent
                                    text: "This device"
                                    color: Theme.accent
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: device.modelData.awaitingTwoFactor
                                  ? "Password entered, two-factor code not yet. If this wasn't you, sign it out and change your password."
                                  : "Signed in until " + device.modelData.expiresOn
                            textFormat: Text.PlainText
                            color: device.modelData.awaitingTwoFactor ? Theme.warn : Theme.text3
                            font.pixelSize: 13
                        }
                    }
                    RvTextField {
                        id: nameField
                        Layout.fillWidth: true
                        visible: device.modelData.current && root.editing
                        placeholderText: "Name this device"
                        maximumLength: 60
                        onAccepted: saveButton.clicked()
                        Keys.onEscapePressed: root.editing = false
                    }
                    RvButton {
                        id: saveButton
                        visible: device.modelData.current && root.editing
                        text: "Save"
                        enabled: nameField.text.trim().length > 0
                        onClicked: {
                            root.devices.rename(nameField.text)
                            root.editing = false
                        }
                    }
                    RvButton {
                        visible: device.modelData.current && !root.editing
                        secondary: true
                        text: "Rename"
                        implicitHeight: 34
                        onClicked: {
                            nameField.text = device.modelData.name
                            root.editing = true
                            nameField.forceActiveFocus()
                        }
                    }
                    RvButton {
                        visible: !device.modelData.current && root.confirmId !== device.modelData.id
                        primary: false
                        danger: true
                        text: "Sign out"
                        implicitHeight: 34
                        onClicked: root.confirmId = device.modelData.id
                    }
                    RvButton {
                        visible: !device.modelData.current && root.confirmId === device.modelData.id
                        danger: true
                        text: "Sign it out"
                        implicitHeight: 34
                        onClicked: {
                            root.devices.signOut(device.modelData.id)
                            root.confirmId = ""
                        }
                    }
                }
            }
        }
    }
    Text {
        visible: root.confirmId.length > 0
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: "That device will need your password (and two-factor code) to sign in again."
        color: Theme.text3
        font.pixelSize: 13
    }
}
