// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Compare the 60-digit safety number with your contact (in person or on a
// call you trust). It's the same number on every app, for both of you.
RvDialog {
    id: dialog
    required property ChatSession chats
    title: "Verify " + chats.currentTitle
    width: 520

    // Device keys: a second number over both accounts' device keys (Olm and Signal sessions), when
    // both of you have them. `device` is what DeviceKeysSession.safetyNumberReady last sent.
    property var device: ({})
    property bool deviceBusy: false
    readonly property var deviceGroups: device.groups || []
    readonly property bool deviceKeyChanged: device.keyChanged === true
    readonly property bool deviceVerified: device.verified === true
    readonly property string deviceProblem: device.problem || ""
    function deviceAction(call) {
        dialog.deviceBusy = true
        call()
    }
    onAboutToShow: {
        device = ({})
        deviceBusy = false
        if (App.deviceKeys && App.deviceKeys.trusted && chats.currentUserId.length > 0)
            App.deviceKeys.loadSafetyNumber(chats.currentUserId, chats.currentChatId)
    }

    // Tall with device keys: it scrolls inside the window instead of running off it.
    height: Math.min(implicitHeight, Overlay.overlay ? Overlay.overlay.height - 48 : implicitHeight)

    contentItem: Flickable {
        id: scroller
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight
        implicitWidth: column.implicitWidth
        implicitHeight: column.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {
            policy: scroller.contentHeight > scroller.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
        }

        ColumnLayout {
            id: column
            width: scroller.width - (scroller.contentHeight > scroller.height ? 12 : 0)
            spacing: 14

            Connections {
                target: App.deviceKeys
                function onSafetyNumberReady(contactUserId, info) {
                    if (contactUserId !== dialog.chats.currentUserId) return
                    dialog.device = info
                    dialog.deviceBusy = false
                }
            }

            // Device keys changed: say so at the top, since the details are further down.
            Rectangle {
                Layout.fillWidth: true
                visible: dialog.deviceKeyChanged
                implicitHeight: topDeviceText.implicitHeight + 24
                radius: Theme.radiusControl
                color: Theme.warnSoft
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10
                    Icon {
                        Layout.alignment: Qt.AlignTop
                        name: "triangle-alert"
                        size: 18
                        color: Theme.warn
                    }
                    Text {
                        id: topDeviceText
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: dialog.chats.currentTitle + "'s device keys changed. Scroll down to Device keys and compare the new number before you trust them."
                        textFormat: Text.PlainText
                        color: Theme.warn
                        font.pixelSize: 13
                    }
                }
            }

            // Their key is different from the one this computer first saw.
            Rectangle {
                Layout.fillWidth: true
                visible: dialog.chats.currentKeyChanged
                implicitHeight: changedText.implicitHeight + 24
                radius: Theme.radiusControl
                color: Theme.warnSoft
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10
                    Icon {
                        Layout.alignment: Qt.AlignTop
                        name: "triangle-alert"
                        size: 18
                        color: Theme.warn
                    }
                    Text {
                        id: changedText
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: "Safety number changed. " + dialog.chats.currentTitle
                              + " has a new key: a new device, a reinstall, or someone in the middle. Compare the new number before you trust this chat."
                        textFormat: Text.PlainText
                        color: Theme.warn
                        font.pixelSize: 13
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Compare these numbers with " + dialog.chats.currentTitle
                      + ", in person or on a call you trust. If they match on both screens, nobody is intercepting this chat."
                textFormat: Text.PlainText
                color: Theme.text2
                font.pixelSize: Theme.fontSmall + 1
            }

            // Their phone can scan this from the Verify screen instead of comparing digits.
            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                visible: dialog.chats.currentSafetyQr.length > 0
                implicitWidth: 188
                implicitHeight: 188
                radius: Theme.radiusControl
                color: "white"
                Image {
                    anchors.centerIn: parent
                    width: 176
                    height: 176
                    cache: false
                    smooth: false
                    sourceSize: Qt.size(176, 176)
                    source: dialog.chats.currentSafetyQr.length > 0 ? "image://qr/" + encodeURIComponent(dialog.chats.currentSafetyQr) : ""
                }
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: "Scan this with the app on their phone (Verify screen), or compare the numbers below."
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: digits.implicitHeight + 28
                radius: Theme.radiusCard
                color: Theme.bg
                border.color: dialog.chats.currentVerified ? Theme.accent : Theme.borderStrong
                border.width: dialog.chats.currentVerified ? 2 : 1

                Text {
                    id: digits
                    anchors.centerIn: parent
                    horizontalAlignment: Text.AlignHCenter
                    text: dialog.chats.currentSafetyNumber
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.family: Theme.monoFamily
                    font.pixelSize: 20
                    font.weight: Font.Medium
                    lineHeight: 1.3
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: dialog.chats.currentVerified
                Icon {
                    name: "shield-check"
                    size: 20
                    color: Theme.accent
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "You've verified " + dialog.chats.currentTitle + " on this computer."
                    textFormat: Text.PlainText
                    color: Theme.accent
                    font.weight: Font.DemiBold
                }
            }

            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "If they don't match, someone may be intercepting this chat, or one of you has a new key. Don't mark it as verified."
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }

            RvButton {
                Layout.fillWidth: true
                primary: false
                iconName: "copy"
                text: "Copy number"
                onClicked: App.copySecret(dialog.chats.currentSafetyNumber.split("\n").join(" "))
            }
            RvButton {
                Layout.fillWidth: true
                visible: !dialog.chats.currentVerified
                text: "The numbers match, mark as verified"
                onClicked: dialog.chats.setCurrentVerified(true)
            }
            RvButton {
                Layout.fillWidth: true
                visible: dialog.chats.currentVerified
                primary: false
                text: "Mark as not verified"
                onClicked: dialog.chats.setCurrentVerified(false)
            }

            Text {
                Layout.fillWidth: true
                visible: dialog.deviceProblem.length > 0
                wrapMode: Text.Wrap
                text: dialog.deviceProblem
                textFormat: Text.PlainText
                color: Theme.warn
                font.pixelSize: Theme.fontSmall + 1
            }

            // --- Device keys: the number over both accounts' device keys ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: dialog.deviceGroups.length > 0
                spacing: 10

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Theme.border
                }
                Text {
                    Layout.fillWidth: true
                    text: "Device keys"
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.weight: Font.DemiBold
                    font.pixelSize: Theme.fontSmall + 2
                }

                // Their account's device keys aren't the ones this computer trusted: compare before accepting.
                Rectangle {
                    Layout.fillWidth: true
                    visible: dialog.deviceKeyChanged
                    implicitHeight: deviceChangedText.implicitHeight + 24
                    radius: Theme.radiusControl
                    color: Theme.warnSoft
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 10
                        Icon {
                            Layout.alignment: Qt.AlignTop
                            name: "triangle-alert"
                            size: 18
                            color: Theme.warn
                        }
                        Text {
                            id: deviceChangedText
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: dialog.chats.currentTitle + "'s device keys changed: they started over on a new device, or someone is in the middle. "
                                  + "Until you accept the new keys, messages with them stay on hold. Compare this new number first."
                            textFormat: Text.PlainText
                            color: Theme.warn
                            font.pixelSize: 13
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "This number covers the keys on every device you both use. Their phone can scan this code, or compare the digits."
                    textFormat: Text.PlainText
                    color: Theme.text2
                    font.pixelSize: Theme.fontSmall + 1
                }

                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    visible: (dialog.device.qrText || "").length > 0
                    implicitWidth: 188
                    implicitHeight: 188
                    radius: Theme.radiusControl
                    color: "white"
                    Image {
                        anchors.centerIn: parent
                        width: 176
                        height: 176
                        cache: false
                        smooth: false
                        sourceSize: Qt.size(176, 176)
                        source: (dialog.device.qrText || "").length > 0 ? "image://qr/" + encodeURIComponent(dialog.device.qrText) : ""
                        Accessible.name: "Device-key safety number code for their phone to scan"
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: deviceDigits.implicitHeight + 24
                    radius: Theme.radiusCard
                    color: Theme.bg
                    border.color: dialog.deviceVerified ? Theme.accent : (dialog.deviceKeyChanged ? Theme.warn : Theme.borderStrong)
                    border.width: dialog.deviceVerified || dialog.deviceKeyChanged ? 2 : 1
                    Text {
                        id: deviceDigits
                        anchors.centerIn: parent
                        width: parent.width - 24
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        // 3 rows of 4 groups, like the account number above.
                        text: [0, 4, 8].map(i => dialog.deviceGroups.slice(i, i + 4).join(" ")).join("
")
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.family: Theme.monoFamily
                        font.pixelSize: 17
                        lineHeight: 1.3
                        Accessible.name: "Device-key safety number " + text
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: dialog.deviceVerified
                    Icon {
                        name: "shield-check"
                        size: 20
                        color: Theme.accent
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: "You've verified " + dialog.chats.currentTitle + "'s device keys on this computer."
                        textFormat: Text.PlainText
                        color: Theme.accent
                        font.weight: Font.DemiBold
                    }
                }

                RvButton {
                    Layout.fillWidth: true
                    primary: false
                    iconName: "copy"
                    text: "Copy device-key number"
                    onClicked: App.copySecret(dialog.deviceGroups.join(" "))
                }
                // Same keys as before.
                RvButton {
                    Layout.fillWidth: true
                    visible: !dialog.deviceKeyChanged && !dialog.deviceVerified
                    enabled: !dialog.deviceBusy
                    text: "The numbers match, mark device keys as verified"
                    onClicked: dialog.deviceAction(() => App.deviceKeys.setDeviceKeysVerified(dialog.chats.currentUserId, dialog.chats.currentChatId,
                                                                                              dialog.device.accountKey, true))
                }
                RvButton {
                    Layout.fillWidth: true
                    visible: !dialog.deviceKeyChanged && dialog.deviceVerified
                    enabled: !dialog.deviceBusy
                    primary: false
                    text: "Mark device keys as not verified"
                    onClicked: dialog.deviceAction(() => App.deviceKeys.setDeviceKeysVerified(dialog.chats.currentUserId, dialog.chats.currentChatId,
                                                                                              dialog.device.accountKey, false))
                }
                // New keys.
                RvButton {
                    Layout.fillWidth: true
                    visible: dialog.deviceKeyChanged
                    enabled: !dialog.deviceBusy
                    text: "The new numbers match, trust the new keys"
                    onClicked: dialog.deviceAction(() => App.deviceKeys.acceptNewDeviceKeys(dialog.chats.currentUserId, dialog.chats.currentChatId,
                                                                                            dialog.device.accountKey, true))
                }
                RvButton {
                    Layout.fillWidth: true
                    visible: dialog.deviceKeyChanged
                    enabled: !dialog.deviceBusy
                    primary: false
                    text: "Trust the new keys without comparing"
                    onClicked: dialog.deviceAction(() => App.deviceKeys.acceptNewDeviceKeys(dialog.chats.currentUserId, dialog.chats.currentChatId,
                                                                                            dialog.device.accountKey, false))
                }
            }
        }
    }
}
