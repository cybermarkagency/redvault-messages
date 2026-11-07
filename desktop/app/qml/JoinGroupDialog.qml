pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Redvault

// Someone opened a group invite link (pasted, or clicked in a chat): the
// group's name and size, and Join group, or Ask to join when an admin approves
// new members, as the web app's JoinGroup. Also asks for a link to paste.
RvDialog {
    id: dialog
    required property GroupInviteSession invites
    readonly property string joinState: invites ? invites.joinState : ""
    property string pasteError: ""

    title: joinState === "paste" ? "Join a group with a link" : "Group invite"
    width: 460

    onOpened: {
        pasteError = ""
        pasteField.text = ""
        if (joinState === "paste") pasteField.forceActiveFocus()
    }
    onClosed: if (invites) invites.closeJoin()

    Connections {
        target: dialog.invites
        function onShowJoin() {
            if (!dialog.visible) dialog.open()
        }
        function onJoined() {
            dialog.close()
        }
    }

    contentItem: ColumnLayout {
        spacing: 14

        // --- Asking for a link ---
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.joinState === "paste"
            spacing: 10
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Paste an invite link someone shared with you."
                color: Theme.text2
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                RvTextField {
                    id: pasteField
                    Layout.fillWidth: true
                    placeholderText: "Paste the invite link"
                    font.family: Theme.monoFamily
                    Accessible.name: "Group invite link"
                    onTextChanged: dialog.pasteError = ""
                    onAccepted: openButton.clicked()
                }
                RvButton {
                    id: openButton
                    text: "Open"
                    enabled: pasteField.text.trim().length > 0
                    onClicked: dialog.pasteError = dialog.invites.openInvite(pasteField.text)
                }
            }
            Banner {
                Layout.fillWidth: true
                text: dialog.pasteError
            }
        }

        // --- Opening, or a link that doesn't work ---
        Text {
            visible: dialog.joinState === "opening"
            text: "Opening the link…"
            color: Theme.text2
        }
        Banner {
            Layout.fillWidth: true
            visible: dialog.joinState === "error"
            text: dialog.invites ? dialog.invites.joinError : ""
        }

        // --- The group ---
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.joinState === "ready"
            spacing: 10

            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: 64
                implicitHeight: 64
                radius: 32
                color: Theme.accentSoft
                Icon {
                    anchors.centerIn: parent
                    name: "users"
                    size: 30
                    color: Theme.accent
                }
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: dialog.invites ? dialog.invites.joinName : ""
                textFormat: Text.PlainText
                color: Theme.text
                font.family: Theme.displayFamily
                font.pixelSize: 22
                font.weight: Font.Bold
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: dialog.invites ? "Group · " + dialog.invites.joinMemberCount + (dialog.invites.joinMemberCount === 1 ? " member" : " members") : ""
                textFormat: Text.PlainText
                color: Theme.text3
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Icon {
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 2
                    name: "lock"
                    size: 14
                    color: Theme.accent
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Messages in this group are end-to-end encrypted. You'll only see messages sent after you join."
                          + (dialog.invites && dialog.invites.joinApproval ? " An admin of the group has to approve you first." : "")
                    color: Theme.text2
                    font.pixelSize: Theme.fontSmall + 1
                }
            }
            Banner {
                Layout.fillWidth: true
                text: dialog.invites ? dialog.invites.joinError : ""
            }

            RvButton {
                Layout.fillWidth: true
                visible: dialog.invites.joinMember
                text: "Open group"
                onClicked: dialog.invites.openGroup()
            }
            Text {
                Layout.fillWidth: true
                visible: !dialog.invites.joinMember && dialog.invites.joinRequested
                wrapMode: Text.Wrap
                text: "Request sent. The group will appear in your chats once an admin approves it."
                color: Theme.text2
            }
            RvButton {
                Layout.fillWidth: true
                visible: !dialog.invites.joinMember && dialog.invites.joinRequested
                secondary: true
                text: "Cancel request"
                enabled: !dialog.invites.joinBusy
                onClicked: dialog.invites.cancelRequest()
            }
            RvButton {
                Layout.fillWidth: true
                visible: !dialog.invites.joinMember && !dialog.invites.joinRequested
                text: dialog.invites.joinBusy ? "Joining…" : dialog.invites.joinApproval ? "Ask to join" : "Join group"
                enabled: !dialog.invites.joinBusy
                onClicked: dialog.invites.join()
            }
        }
    }
}
