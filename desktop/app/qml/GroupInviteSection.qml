// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Group info > Invite link, as the web app's GroupInviteLink: every member
// can copy the group's link; the owner and admins turn it on or off, reset it,
// choose whether they approve each join, and answer requests to join. The
// link's secret stays on members' devices: the group stays end-to-end encrypted.
ColumnLayout {
    id: section
    required property GroupInviteSession invites
    property bool copied: false

    spacing: 10
    // Nothing while loading, and nothing for a member when there's no link.
    visible: invites !== null && invites.linkState !== "loading" && (invites.linkState === "on" || invites.canManage)

    Connections {
        target: section.invites
        function onCopied() {
            section.copied = true
            copiedTimer.restart()
        }
    }
    Timer {
        id: copiedTimer
        interval: 2000
        onTriggered: section.copied = false
    }

    ConfirmDialog {
        id: resetDialog
        title: "Reset the invite link?"
        message: "The current link stops working."
        confirmText: "Reset link"
        onConfirmed: section.invites.resetLink()
    }
    ConfirmDialog {
        id: turnOffDialog
        title: "Turn off the invite link?"
        message: "It stops working, and pending requests are dropped."
        confirmText: "Turn off link"
        danger: true
        onConfirmed: section.invites.turnOff()
    }

    Text {
        Layout.topMargin: 6
        text: "INVITE LINK"
        color: Theme.text3
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
    }

    Banner {
        Layout.fillWidth: true
        text: section.invites ? section.invites.error : ""
    }

    // No link yet (owners and admins only).
    ColumnLayout {
        Layout.fillWidth: true
        visible: section.invites.linkState === "none"
        spacing: 10
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "Anyone with the link can join, or ask to join if you approve new members. The group stays end-to-end encrypted: the link's secret never reaches the server."
            color: Theme.text2
            font.pixelSize: Theme.fontSmall + 1
        }
        RvButton {
            iconName: "link"
            text: "Create invite link"
            enabled: !section.invites.busy
            onClicked: section.invites.createLink()
        }
    }

    // The link.
    ColumnLayout {
        Layout.fillWidth: true
        visible: section.invites.linkState === "on"
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            visible: section.invites.linkUrl.length > 0
            spacing: 8
            RvTextField {
                Layout.fillWidth: true
                readOnly: true
                text: section.invites.linkUrl
                font.family: Theme.monoFamily
                font.pixelSize: 12
                Accessible.name: "Invite link"
                // Show the start of the address, not its end.
                onTextChanged: cursorPosition = 0
                onActiveFocusChanged: if (activeFocus) selectAll()
            }
            RvButton {
                iconName: "copy"
                text: section.copied ? "Copied" : "Copy"
                onClicked: section.invites.copyLink()
            }
        }
        Text {
            Layout.fillWidth: true
            visible: section.invites.linkUrl.length === 0
            wrapMode: Text.Wrap
            text: "This group's link couldn't be opened on this computer."
            color: Theme.text2
        }

        RvSwitch {
            Layout.fillWidth: true
            visible: section.invites.canManage
            label: "Approve new members"
            description: section.invites.approval ? "An admin lets in each person who opens the link."
                                                  : "Anyone who opens the link joins straight away."
            checked: section.invites.approval
            enabled: !section.invites.busy && section.invites.linkUrl.length > 0
            onToggled: (on) => section.invites.setApproval(on)
        }

        RowLayout {
            visible: section.invites.canManage
            spacing: 4
            RvButton {
                primary: false
                text: "Reset link"
                enabled: !section.invites.busy
                onClicked: resetDialog.open()
            }
            RvButton {
                primary: false
                danger: true
                text: "Turn off link"
                enabled: !section.invites.busy
                onClicked: turnOffDialog.open()
            }
        }
    }

    // Requests to join (owners and admins only).
    Text {
        Layout.topMargin: 6
        visible: section.invites.canManage && section.invites.requests.length > 0
        text: "REQUESTS TO JOIN (" + section.invites.requests.length + ")"
        textFormat: Text.PlainText
        color: Theme.text3
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
    }
    ScrollView {
        Layout.fillWidth: true
        visible: section.invites.canManage && section.invites.requests.length > 0
        Layout.preferredHeight: Math.min(180, requestList.contentHeight)
        clip: true

        ListView {
            id: requestList
            model: section.invites.requests
            spacing: 4
            delegate: RowLayout {
                id: request
                required property var modelData
                width: ListView.view.width
                spacing: 10

                Avatar {
                    size: 30
                    initial: request.modelData.handle.charAt(0).toUpperCase()
                    tintKey: request.modelData.userId
                    source: request.modelData.avatarUrl
                }
                Text {
                    Layout.fillWidth: true
                    text: request.modelData.handle
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.text
                }
                RvButton {
                    implicitHeight: 28
                    text: "Approve"
                    enabled: !section.invites.busy
                    onClicked: section.invites.approve(request.modelData.userId)
                }
                RvButton {
                    primary: false
                    implicitHeight: 28
                    text: "Decline"
                    enabled: !section.invites.busy
                    onClicked: section.invites.decline(request.modelData.userId)
                }
            }
        }
    }
}
