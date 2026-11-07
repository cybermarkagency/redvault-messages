pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// In a 1:1 chat's details: what this person offers as a creator (a tip
// button, what messaging them costs, their exclusive posts), and, when you
// charge for messages, letting them message you free. Nothing shows for a
// blocked pair (the server offers nothing). As the web app's
// CreatorOffer.tsx and ExemptionToggle.tsx.
ColumnLayout {
    id: section
    required property EarningsSession earnings
    property string userId: ""
    property string name: ""
    property var offer: ({ available: false, posts: [] })

    spacing: 10
    visible: earnings !== null && earnings.enabled && userId.length > 0 && (offer.available || earnings.chargesForMessages)

    function reload() {
        offer = { available: false, posts: [] }
        if (earnings && userId.length > 0) earnings.loadOffer(userId)
    }
    onUserIdChanged: reload()
    Component.onCompleted: reload()

    Connections {
        target: section.earnings
        function onOfferLoaded(userId, offer) {
            if (userId === section.userId) section.offer = offer
        }
        function onConfigChanged() { section.reload() }
        function onPayChanged() {
            // A post bought here: show it as bought.
            if (section.earnings.paidProduct === "exclusive_post") section.reload()
        }
    }

    Text {
        visible: section.offer.available === true
        text: "CREATOR"
        color: Theme.text3
        font.pixelSize: Theme.fontSmall
        font.weight: Font.DemiBold
        font.letterSpacing: 0.6
    }
    Text {
        Layout.fillWidth: true
        visible: section.offer.available === true && (section.offer.handle ?? "").length > 0
        text: "@" + (section.offer.handle ?? "")
        textFormat: Text.PlainText
        color: Theme.text2
        font.pixelSize: Theme.fontSmall + 1
    }
    Text {
        Layout.fillWidth: true
        visible: section.offer.available === true && (section.offer.paidMessages ?? "").length > 0
        wrapMode: Text.Wrap
        text: section.offer.paidMessages ?? ""
        textFormat: Text.PlainText
        color: Theme.text2
        font.pixelSize: Theme.fontSmall + 1
    }
    RvButton {
        visible: section.offer.available === true && section.offer.tips === true
        text: "Tip " + section.name
        iconName: "heart"
        onClicked: tipDialog.openFor(section.userId, section.name)
    }

    Text {
        visible: section.offer.available === true && (section.offer.posts ?? []).length > 0
        Layout.topMargin: 4
        text: "EXCLUSIVE POSTS"
        color: Theme.text3
        font.pixelSize: Theme.fontSmall
        font.weight: Font.DemiBold
        font.letterSpacing: 0.6
    }
    Repeater {
        model: section.offer.available === true ? section.offer.posts : []
        delegate: ExclusivePostCard {
            required property var modelData
            Layout.fillWidth: true
            earnings: section.earnings
            post: modelData
        }
    }

    // You charge for messages: let this person in for free (friends always are).
    ColumnLayout {
        Layout.fillWidth: true
        visible: section.earnings !== null && section.earnings.chargesForMessages
        spacing: 6
        readonly property bool exempt: section.earnings !== null && section.earnings.settings.exemptCount >= 0 && section.earnings.isExempt(section.userId)
        Text {
            text: "PAID MESSAGES"
            color: Theme.text3
            font.pixelSize: Theme.fontSmall
            font.weight: Font.DemiBold
            font.letterSpacing: 0.6
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: parent.exempt
                ? section.name + " can message you for free."
                : section.name + " pays your message price unless you let them in (friends always can)."
            textFormat: Text.PlainText
            color: Theme.text2
            font.pixelSize: Theme.fontSmall + 1
        }
        RvButton {
            secondary: parent.exempt
            text: parent.exempt ? "Charge them again" : "Let " + section.name + " message you free"
            onClicked: section.earnings.setExempt(section.userId, !parent.exempt)
        }
    }

    TipDialog {
        id: tipDialog
        earnings: section.earnings
    }
}
