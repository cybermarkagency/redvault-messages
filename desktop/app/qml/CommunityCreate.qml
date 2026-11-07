pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Creating a community, as the web app's CommunityCreate: a channel or a
// group, free or paid monthly, with the commission shown for the price typed.
// The community's first key (epoch 0) is made on this computer and sealed to
// you; the server never sees it. It starts as a draft.
ColumnLayout {
    id: form
    required property CommunitySession communities
    spacing: 12

    readonly property var creator: communities.creator
    readonly property var config: communities.config
    readonly property bool paidPossible: config.paymentsEnabled === true && !!creator && creator.paidMembershipsEnabled === true
    property string kind: "channel"
    property string accessType: paidPossible ? "paid" : "free"
    readonly property int cents: accessType === "paid" ? communities.parseDollars(price.text) : -1
    readonly property string priceProblem: accessType !== "paid" ? ""
        : cents < 0 ? "Enter a monthly price like 10 or 9.99."
        : (config.minPriceMinor !== undefined && (cents < config.minPriceMinor || cents > config.maxPriceMinor))
          ? "Between " + communities.formatMoney(config.minPriceMinor, config.currency) + " and " + communities.formatMoney(config.maxPriceMinor, config.currency) + "."
        : ""
    readonly property int commission: cents > 0 ? communities.commissionFor(cents) : 0

    Text {
        visible: !form.communities.creatorLoaded
        text: "Loading…"
        color: Theme.text3
    }

    // Not a creator yet.
    Card {
        Layout.fillWidth: true
        visible: form.communities.creatorLoaded && !form.creator
        Text {
            text: "Set up a creator profile first"
            color: Theme.text
            font.weight: Font.DemiBold
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "Communities are published under a public creator handle, separate from your private RedVault username."
            color: Theme.text2
            font.pixelSize: 13
        }
        RvButton {
            text: "Become a creator"
            onClicked: form.communities.open("creator")
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: form.communities.creatorLoaded && !!form.creator
        spacing: 12

        Caption { text: "TYPE" }
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Option {
                Layout.fillWidth: true
                title: "Channel"
                body: "You and your staff post; members read and receive."
                selected: form.kind === "channel"
                onClicked: form.kind = "channel"
            }
            Option {
                Layout.fillWidth: true
                title: "Group"
                body: "Members talk together, with moderators."
                selected: form.kind === "group"
                onClicked: form.kind = "group"
            }
        }

        Caption { text: "ACCESS" }
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Option {
                Layout.fillWidth: true
                title: "Paid, monthly"
                body: form.paidPossible ? "Members subscribe through Stripe." : "Turn on paid memberships and set up payouts in the Creator dashboard first."
                selected: form.accessType === "paid"
                enabled: form.paidPossible
                onClicked: form.accessType = "paid"
            }
            Option {
                Layout.fillWidth: true
                title: "Free"
                body: "Anyone you let in joins at no cost."
                selected: form.accessType === "free"
                onClicked: form.accessType = "free"
            }
        }

        FieldLabel { text: "Name" }
        RvTextField {
            id: name
            Layout.fillWidth: true
            maximumLength: 80
            Accessible.name: "Name"
        }
        FieldLabel { text: "Description (public)" }
        PostField {
            id: description
            Layout.fillWidth: true
            limit: 2000
            Accessible.name: "Description"
        }
        GridLayout {
            Layout.fillWidth: true
            columns: form.accessType === "paid" ? 3 : 2
            columnSpacing: 10
            rowSpacing: 6
            FieldLabel { text: "Category" }
            FieldLabel { text: "Who can find it" }
            FieldLabel { visible: form.accessType === "paid"; text: "Monthly price (USD)" }
            RvComboBox {
                id: category
                Layout.fillWidth: true
                Accessible.name: "Category"
                options: form.communities.categories
                value: "education"
            }
            RvComboBox {
                id: visibility
                Layout.fillWidth: true
                Accessible.name: "Who can find it"
                options: [{ value: "unlisted", label: "Unlisted: only with an invite link" }, { value: "discoverable", label: "Listed in Discover" }]
                value: "unlisted"
            }
            RvTextField {
                id: price
                visible: form.accessType === "paid"
                Layout.fillWidth: true
                text: "10.00"
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                Accessible.name: "Monthly price in US dollars"
            }
        }
        InfoCard {
            Layout.fillWidth: true
            visible: form.accessType === "paid"
            tone: form.priceProblem.length > 0 ? "error" : "neutral"
            text: form.priceProblem.length > 0 ? form.priceProblem
                : form.cents > 0 && form.config.commissionBps !== undefined
                  ? "Of each " + form.communities.formatMoney(form.cents, form.config.currency) + " payment, RedVault keeps "
                    + form.communities.formatRate(form.config.commissionBps) + " (" + form.communities.formatMoney(form.commission, form.config.currency)
                    + ") and you earn " + form.communities.formatMoney(form.cents - form.commission, form.config.currency)
                    + ". RedVault pays Stripe's processing fees. Taxes, refunds and disputes can change what you receive; see the creator terms."
                  : ""
        }
        FieldLabel { text: "Rules" }
        PostField {
            id: rules
            Layout.fillWidth: true
            limit: 4000
            Accessible.name: "Rules"
        }
        FieldLabel { text: "Preview (public: what people see before joining)" }
        PostField {
            id: preview
            Layout.fillWidth: true
            limit: 1000
            Accessible.name: "Preview"
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 10
            rowSpacing: 6
            FieldLabel { text: "Member limit (optional)" }
            FieldLabel { text: "Slow mode" }
            RvTextField {
                id: memberLimit
                Layout.fillWidth: true
                validator: RegularExpressionValidator { regularExpression: /\d{0,6}/ }
                Accessible.name: "Member limit"
            }
            RvComboBox {
                id: slowMode
                Layout.fillWidth: true
                Accessible.name: "Slow mode"
                options: [{ value: 0, label: "Off" }, { value: 10, label: "10 seconds" }, { value: 60, label: "1 minute" }, { value: 300, label: "5 minutes" }]
                value: 0
            }
        }
        ConsentBox {
            id: joinApproval
            Layout.fillWidth: true
            visible: form.accessType === "free"
            label: "A moderator approves each person who asks to join."
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.text3
            font.pixelSize: Theme.fontSmall + 1
            text: "The name, description, rules, preview and price are public listing information RedVault can see. Everything posted inside is end-to-end encrypted."
        }
        RvButton {
            text: form.communities.busy ? "Creating…" : "Create as draft"
            enabled: !form.communities.busy && name.text.trim().length > 0 && form.priceProblem.length === 0
                     && !description.tooLong && !rules.tooLong && !preview.tooLong
            onClicked: form.communities.create({
                kind: form.kind,
                accessType: form.accessType,
                name: name.text,
                description: description.text,
                category: category.value,
                visibility: visibility.value,
                price: price.text,
                rules: rules.text,
                previewText: preview.text,
                memberLimit: memberLimit.text,
                joinApproval: joinApproval.checked,
                slowModeSeconds: slowMode.value
            })
        }
    }

    component Caption: Text {
        color: Theme.text3
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
    }
    component FieldLabel: Text {
        color: Theme.text2
        font.pixelSize: 13
    }
    component Option: AbstractButton {
        id: option
        property string title
        property string body
        property bool selected: false
        hoverEnabled: true
        implicitHeight: optionText.implicitHeight + 24
        Accessible.name: title
        background: Rectangle {
            radius: Theme.radiusControl
            color: option.selected ? Theme.accentSoft : (option.hovered && option.enabled ? Theme.surface2 : Theme.surface)
            border.color: option.selected ? Theme.accent : Theme.border
            border.width: option.selected ? 2 : 1
        }
        contentItem: Item {
            Column {
                id: optionText
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                opacity: option.enabled ? 1 : 0.55
                Text {
                    width: optionText.width
                    text: option.title
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.weight: Font.DemiBold
                }
                Text {
                    width: optionText.width
                    text: option.body
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.text2
                    font.pixelSize: 13
                }
            }
        }
    }
}
