pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Redvault

// Add a friend by handle, and answer or cancel pending requests. A creator
// who charges for messages can also be messaged without being friends.
RvDialog {
    id: dialog
    required property PeopleSession people
    /// The person found, as a creator: {available, paidMessages, ...} (EarningsSession.loadOffer).
    property var foundOffer: ({ available: false })
    property string startError: ""
    readonly property string foundUserId: people.found.userId ?? ""
    title: "Friends"
    width: 500

    onFoundUserIdChanged: {
        foundOffer = { available: false }
        startError = ""
        if (foundUserId.length > 0 && App.earnings && !people.found.isSelf && !people.found.isFriend && !people.found.deleted)
            App.earnings.loadOffer(foundUserId)
    }
    Connections {
        target: App.earnings
        function onOfferLoaded(userId, offer) {
            if (userId === dialog.foundUserId) dialog.foundOffer = offer
        }
    }
    Connections {
        target: App.chats
        enabled: dialog.visible
        function onGroupActionDone(error) {
            if (dialog.startError === "starting") {
                dialog.startError = error
                if (error.length === 0) dialog.close()
            }
        }
    }

    onOpened: {
        people.clearLookup()
        lookupField.text = ""
        lookupField.forceActiveFocus()
    }

    component SectionTitle: Text {
        Layout.topMargin: 8
        color: Theme.text2
        font.pixelSize: Theme.fontSmall
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
    }

    component PersonRow: RowLayout {
        id: personRow
        property string handle
        property string avatarUrl
        default property alias actions: actionRow.data
        Layout.fillWidth: true
        spacing: 10
        Avatar {
            size: 32
            initial: personRow.handle.length > 0 ? personRow.handle.charAt(0).toUpperCase() : "?"
            source: personRow.avatarUrl
        }
        Text {
            Layout.fillWidth: true
            text: personRow.handle
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.text
            font.weight: Font.DemiBold
        }
        Row {
            id: actionRow
            spacing: 4
        }
    }

    contentItem: ColumnLayout {
        spacing: 10

        // --- Add a friend ---
        SectionTitle { text: "Add a friend" }
        RowLayout {
            Layout.fillWidth: true
            RvTextField {
                id: lookupField
                Layout.fillWidth: true
                placeholderText: "Their handle"
                enabled: !dialog.people.busy
                onAccepted: dialog.people.lookup(text)
            }
            RvButton {
                iconName: "search"
                text: "Find"
                enabled: !dialog.people.busy && lookupField.text.trim().length > 0
                onClicked: dialog.people.lookup(lookupField.text)
            }
        }

        // The person found: a read-only preview until a request is sent.
        Rectangle {
            Layout.fillWidth: true
            visible: dialog.people.found.handle !== undefined
            implicitHeight: foundRow.implicitHeight + 20
            radius: Theme.radiusControl
            color: Theme.bg
            border.color: Theme.border

            PersonRow {
                id: foundRow
                anchors.fill: parent
                anchors.margins: 10
                handle: dialog.people.found.handle ?? ""
                avatarUrl: dialog.people.found.avatarUrl ?? ""

                Text {
                    visible: !!dialog.people.found.isSelf || !!dialog.people.found.isFriend
                             || !!dialog.people.found.pending || !!dialog.people.found.deleted
                    anchors.verticalCenter: parent.verticalCenter
                    text: dialog.people.found.isSelf ? "That's you"
                        : dialog.people.found.deleted ? "Account deleted"
                        : dialog.people.found.isFriend ? "Already friends"
                        : "Request sent"
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall + 1
                }
                RvButton {
                    visible: !dialog.people.found.isSelf && !dialog.people.found.isFriend
                             && !dialog.people.found.pending && !dialog.people.found.deleted
                    iconName: "user-plus"
                    text: "Send request"
                    implicitHeight: 32
                    enabled: !dialog.people.busy
                    onClicked: dialog.people.sendRequest(dialog.people.found.handle)
                }
            }
        }

        // A creator who charges for messages: message them without being friends (paying first, as they ask).
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.foundOffer.available === true && (dialog.foundOffer.paidMessages ?? "").length > 0
            spacing: 8
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Creator. " + (dialog.foundOffer.paidMessages ?? "")
                textFormat: Text.PlainText
                color: Theme.text2
                font.pixelSize: Theme.fontSmall + 1
            }
            RvButton {
                secondary: true
                iconName: "chat"
                text: "Message"
                implicitHeight: 32
                enabled: App.chats !== null && !App.chats.groupBusy
                onClicked: {
                    dialog.startError = "starting"
                    App.chats.startMessageRequest(dialog.foundUserId)
                }
            }
        }
        Banner {
            Layout.fillWidth: true
            text: dialog.startError === "starting" ? "" : dialog.startError
        }

        Banner {
            Layout.fillWidth: true
            text: dialog.people.error
        }
        Banner {
            Layout.fillWidth: true
            error: false
            text: dialog.people.notice
        }

        // --- Requests to you ---
        SectionTitle {
            visible: dialog.people.incoming.length > 0
            text: "Requests to you (" + dialog.people.incoming.length + ")"
        }
        Repeater {
            model: dialog.people.incoming
            delegate: PersonRow {
                id: incomingRow
                required property var modelData
                handle: modelData.handle
                avatarUrl: modelData.avatarUrl
                RvButton {
                    text: "Accept"
                    implicitHeight: 32
                    enabled: !dialog.people.busy
                    onClicked: dialog.people.accept(incomingRow.modelData.id)
                }
                RvButton {
                    primary: false
                    danger: true
                    text: "Deny"
                    implicitHeight: 32
                    enabled: !dialog.people.busy
                    onClicked: dialog.people.deny(incomingRow.modelData.id)
                }
            }
        }

        // --- Your requests ---
        SectionTitle {
            visible: dialog.people.outgoing.length > 0
            text: "Waiting for them (" + dialog.people.outgoing.length + ")"
        }
        Repeater {
            model: dialog.people.outgoing
            delegate: PersonRow {
                id: outgoingRow
                required property var modelData
                handle: modelData.handle
                avatarUrl: modelData.avatarUrl
                RvButton {
                    primary: false
                    text: "Cancel"
                    implicitHeight: 32
                    enabled: !dialog.people.busy
                    onClicked: dialog.people.cancel(outgoingRow.modelData.id)
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.topMargin: 6
            wrapMode: Text.Wrap
            text: "Accepting a request creates an end-to-end encrypted chat. They aren't told if you deny it."
            color: Theme.text3
            font.pixelSize: Theme.fontSmall
        }
    }
}
