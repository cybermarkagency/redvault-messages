// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Settings > Device keys (preview): each device gets its own encryption keys,
// and a new device is trusted only when one of your trusted devices approves
// it. A development preview behind the server's E2E_V3_ENABLED flag (the web
// app's "Device keys (preview)" panel); your messages still use the current
// encryption. Nothing here is shown unless the server has it on.
ColumnLayout {
    id: root
    required property DeviceKeysSession keys
    property string recoveryInput: ""
    property bool confirmForget: false
    readonly property bool idle: root.keys !== null && !root.keys.busy
    spacing: 12

    component Caption: Text {
        Layout.topMargin: 10
        color: Theme.text2
        font.pixelSize: Theme.fontSmall
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
    }
    component Detail: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.text3
        font.pixelSize: Theme.fontSmall + 1
    }
    component Title: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.text
        font.weight: Font.DemiBold
    }
    component Codes: Text {
        textFormat: Text.PlainText
        required property var codes
        Layout.fillWidth: true
        text: codes.map(c => String(c).padStart(4, "0")).join("   ")
        color: Theme.text
        font.family: Theme.monoFamily
        font.pixelSize: 22
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        Accessible.name: "Pairing codes " + text
    }
    component Divider: Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: Theme.border
    }

    // A trusted device finds out about new pairing requests while this page is open.
    Timer {
        interval: 4000
        repeat: true
        running: root.visible && root.keys !== null && root.keys.status === "trusted" && root.idle && root.keys.answeringDeviceId === ""
        onTriggered: root.keys.refresh()
    }

    Text {
        text: "Device keys (preview)"
        color: Theme.text
        font.family: Theme.displayFamily
        font.pixelSize: 26
        font.weight: Font.Bold
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: "In development: each device gets its own encryption keys, and a new device is trusted only when one of your trusted devices approves it. Your messages still use the current encryption."
        color: Theme.text2
        font.pixelSize: 14
    }
    Banner {
        Layout.fillWidth: true
        text: root.keys ? root.keys.error : ""
    }

    // Where this computer's key lives, and how strong that is.
    Card {
        Layout.fillWidth: true
        visible: root.keys !== null && root.keys.status !== "off"
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Icon {
                Layout.alignment: Qt.AlignTop
                name: root.keys && root.keys.protection.weak ? "triangle-alert" : "shield-check"
                size: 18
                color: root.keys && root.keys.protection.weak ? Theme.warn : Theme.accent
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                spacing: 2
                Title { text: "Key protection on this computer" }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.keys ? root.keys.protection.text : ""
                    textFormat: Text.PlainText
                    color: root.keys && root.keys.protection.weak ? Theme.warn : Theme.text2
                    font.pixelSize: Theme.fontSmall + 1
                }
            }
        }
    }

    // Someone asked to reset this account's keys.
    Card {
        Layout.fillWidth: true
        visible: root.keys !== null && root.keys.pendingReset.effectiveAt !== undefined
        spacing: 8
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.keys && root.keys.pendingReset.effectiveAt !== undefined
                  ? "Someone asked to reset this account's keys. It takes effect on "
                    + new Date(root.keys.pendingReset.effectiveAt).toLocaleString()
                    + " unless a trusted device cancels it. If it wasn't you, cancel it and change your password."
                  : ""
            textFormat: Text.PlainText
            color: Theme.warn
        }
        RvButton {
            visible: root.keys !== null && root.keys.trusted
            enabled: root.idle
            text: "Cancel reset"
            danger: true
            onClicked: root.keys.cancelReset()
        }
    }

    // --- First device ---
    Card {
        Layout.fillWidth: true
        visible: root.keys !== null && root.keys.status === "setup"
        spacing: 8
        Title { text: "Set up device keys on this computer" }
        Detail { text: "This computer becomes your first trusted device. Your current key vouches for the new one. Nothing else about your account changes." }
        RvButton {
            enabled: root.idle
            text: "Set up"
            onClicked: root.keys.setUp()
        }
    }

    // --- Pairing a new computer ---
    Card {
        Layout.fillWidth: true
        visible: root.keys !== null && root.keys.status === "pair"
        spacing: 10
        Title { text: "Pair this computer with a trusted device" }
        Detail { text: "Your password alone can't make this computer trusted. Open Device keys on a trusted device and compare the codes." }

        Detail {
            visible: root.keys !== null && root.keys.interrupted && !root.keys.joining
            color: Theme.warn
            text: "Pairing was interrupted: this computer has keys, but no trusted device has approved them yet. Start again."
        }
        RvButton {
            visible: root.keys !== null && root.keys.interrupted && !root.keys.joining
            enabled: root.idle
            text: "Start over"
            primary: false
            onClicked: root.keys.startOver()
        }

        Detail {
            visible: root.keys !== null && root.keys.joining && root.keys.joinCodes.length === 0
            text: "Waiting for a trusted device to answer…"
        }
        Codes {
            visible: root.keys !== null && root.keys.joinCodes.length > 0
            codes: root.keys ? root.keys.joinCodes : []
        }
        Detail {
            visible: root.keys !== null && root.keys.joinCodes.length > 0
            text: "Do these match the numbers on your trusted device?"
        }
        RowLayout {
            visible: root.keys !== null && root.keys.joining
            spacing: 8
            RvButton {
                visible: root.keys !== null && root.keys.joinCodes.length > 0
                enabled: root.idle
                text: "They match"
                onClicked: root.keys.confirmJoin()
            }
            RvButton {
                text: root.keys && root.keys.joinCodes.length > 0 ? "They don't" : "Cancel"
                danger: true
                primary: false
                onClicked: root.keys.cancelJoin()
            }
        }
        RvButton {
            visible: root.keys !== null && !root.keys.joining && !root.keys.interrupted
            enabled: root.idle
            text: "Start pairing"
            onClicked: root.keys.startPairing()
        }

        Divider {}
        Title { text: "No trusted device left? Use your recovery key" }
        RvTextField {
            Layout.fillWidth: true
            placeholderText: "XXXX-XXXX-…"
            text: root.recoveryInput
            onTextChanged: root.recoveryInput = text
            font.family: Theme.monoFamily
            Accessible.name: "Recovery key"
        }
        RvButton {
            enabled: root.idle && root.recoveryInput.trim().length > 0 && !(root.keys && root.keys.joining)
            text: "Restore"
            primary: false
            onClicked: {
                root.keys.restore(root.recoveryInput)
                root.recoveryInput = ""
            }
        }
    }

    // --- This computer was removed ---
    Card {
        Layout.fillWidth: true
        visible: root.keys !== null && root.keys.status === "removed"
        spacing: 8
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "This computer was removed from your trusted devices."
            color: Theme.warn
        }
        RvButton {
            enabled: root.idle
            text: "Delete its keys"
            danger: true
            onClicked: root.keys.forgetDevice()
        }
    }

    // --- Trusted devices ---
    Card {
        Layout.fillWidth: true
        visible: root.keys !== null && (root.keys.status === "trusted" || root.keys.status === "removed") && root.keys.devices.length > 0
        spacing: 8
        Repeater {
            model: root.keys ? root.keys.devices : []
            delegate: RowLayout {
                id: trustedRow
                required property var modelData
                Layout.fillWidth: true
                spacing: 10
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    spacing: 2
                    RowLayout {
                        spacing: 8
                        Text { text: "Trusted device"; color: Theme.text; font.weight: Font.Medium }
                        Rectangle {
                            visible: trustedRow.modelData.isThis
                            implicitWidth: thisLabel.implicitWidth + 14
                            implicitHeight: 22
                            radius: 11
                            color: Theme.accentSoft
                            Text {
                                id: thisLabel
                                anchors.centerIn: parent
                                text: "This computer"
                                color: Theme.accent
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                        }
                        // rv-signal-v1 (development builds): this device's Signal sessions are declared in the signed list.
                        Rectangle {
                            visible: trustedRow.modelData.signal === true
                            implicitWidth: signalLabel.implicitWidth + 14
                            implicitHeight: 22
                            radius: 11
                            color: Theme.surface
                            border.color: Theme.accent
                            Text {
                                id: signalLabel
                                anchors.centerIn: parent
                                text: "Signal"
                                color: Theme.accent
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                            Accessible.name: "Signal sessions are on for this device"
                        }
                    }
                    Text {
                        text: trustedRow.modelData.fingerprint
                        textFormat: Text.PlainText
                        color: Theme.text3
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontSmall
                    }
                }
                // A signed change: contacts are told, and chat with this device goes back to the device-key protocol.
                RvButton {
                    visible: trustedRow.modelData.signal === true && root.keys.status === "trusted"
                    enabled: root.idle
                    text: "Turn off Signal"
                    primary: false
                    onClicked: root.keys.turnOffSignal(trustedRow.modelData.deviceId)
                }
                RvButton {
                    visible: trustedRow.modelData.canRemove
                    enabled: root.idle
                    text: "Remove"
                    danger: true
                    primary: false
                    onClicked: root.keys.revoke(trustedRow.modelData.deviceId)
                }
            }
        }
    }

    // --- Devices waiting for approval ---
    Card {
        Layout.fillWidth: true
        visible: root.keys !== null && root.keys.status === "trusted" && root.keys.waiting.length > 0
        spacing: 10
        Repeater {
            model: root.keys ? root.keys.waiting : []
            delegate: ColumnLayout {
                id: waitingRow
                required property var modelData
                readonly property bool mine: root.keys !== null && root.keys.answeringDeviceId === waitingRow.modelData.deviceId
                Layout.fillWidth: true
                spacing: 6
                Title { text: "Signed in, not trusted" }
                Detail {
                    text: waitingRow.modelData.asking
                          ? "Asking to pair. Compare the codes on both screens before approving."
                          : "If you don't recognize it, remove it and change your password."
                }
                Codes {
                    visible: waitingRow.mine && root.keys.answerCodes.length > 0
                    codes: root.keys ? root.keys.answerCodes : []
                }
                Detail {
                    visible: waitingRow.mine && root.keys.answerCodes.length > 0
                    text: "Do these match the numbers on the new device?"
                }
                RowLayout {
                    spacing: 8
                    RvButton {
                        visible: !waitingRow.mine && waitingRow.modelData.asking && root.keys.answeringDeviceId === ""
                        enabled: root.idle
                        text: "Compare codes"
                        onClicked: root.keys.compareCodes(waitingRow.modelData.deviceId)
                    }
                    RvButton {
                        visible: waitingRow.mine && root.keys.answerCodes.length > 0
                        enabled: root.idle
                        text: "They match"
                        onClicked: root.keys.approve()
                    }
                    RvButton {
                        visible: waitingRow.mine
                        text: "They don't"
                        danger: true
                        primary: false
                        onClicked: root.keys.rejectAnswer()
                    }
                    RvButton {
                        visible: !waitingRow.mine
                        enabled: root.idle
                        text: "Remove"
                        danger: true
                        primary: false
                        onClicked: root.keys.removeWaiting(waitingRow.modelData.deviceId)
                    }
                }
            }
        }
    }

    // --- Recovery key ---
    Card {
        Layout.fillWidth: true
        visible: root.keys !== null && root.keys.status === "trusted"
        spacing: 8
        Title { text: "Recovery key" }
        Detail { text: "For when you have no trusted device left. Write it down; it's shown once and never stored here or on the server." }
        TextEdit {
            Layout.fillWidth: true
            visible: root.keys !== null && root.keys.recoveryKey.length > 0
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            text: root.keys ? root.keys.recoveryKey : ""
            color: Theme.text
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSize
            Accessible.name: "Recovery key"
        }
        RowLayout {
            spacing: 8
            RvButton {
                enabled: root.idle
                text: root.keys && root.keys.recoveryKey.length > 0 ? "Make a new one" : "Create"
                onClicked: root.keys.createRecoveryKey()
            }
            RvButton {
                visible: root.keys !== null && root.keys.recoveryKey.length > 0
                text: "I wrote it down"
                primary: false
                onClicked: root.keys.dismissRecoveryKey()
            }
        }
    }

    // --- Delete this computer's device ---
    Card {
        Layout.fillWidth: true
        visible: root.keys !== null && root.keys.status === "trusted"
        spacing: 8
        Title { text: "Delete this computer's device keys" }
        Detail { text: "Removes this computer's keys and the device-key messages stored here. Your other devices and the recovery key are unaffected, but this computer would have to be paired again." }
        RvButton {
            visible: !root.confirmForget
            enabled: root.idle
            text: "Delete device keys…"
            danger: true
            primary: false
            onClicked: root.confirmForget = true
        }
        RowLayout {
            visible: root.confirmForget
            spacing: 8
            RvButton {
                enabled: root.idle
                text: "Yes, delete them"
                danger: true
                onClicked: {
                    root.confirmForget = false
                    root.keys.forgetDevice()
                }
            }
            RvButton {
                text: "Keep them"
                primary: false
                onClicked: root.confirmForget = false
            }
        }
    }

    // --- Security events ---
    Caption {
        visible: root.keys !== null && root.keys.events.length > 0
        text: "Recent device-key events"
    }
    Card {
        Layout.fillWidth: true
        visible: root.keys !== null && root.keys.events.length > 0
        spacing: 6
        Repeater {
            model: root.keys ? root.keys.events : []
            delegate: RowLayout {
                id: eventRow
                required property var modelData
                Layout.fillWidth: true
                spacing: 10
                Text {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    wrapMode: Text.Wrap
                    text: eventRow.modelData.text
                    textFormat: Text.PlainText
                    color: Theme.text2
                    font.pixelSize: Theme.fontSmall + 1
                }
                Text {
                    text: eventRow.modelData.when
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall + 1
                }
            }
        }
    }
}
