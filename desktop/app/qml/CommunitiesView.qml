// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Paid and free communities, as the web app's Communities screen: Discover,
// My communities, Memberships, Create and the Creator dashboard as tabs, plus
// one community's page and its Manage page. Everything inside a community is
// end-to-end encrypted; the listing, the price and who is a member are not.
Item {
    id: root
    required property CommunitySession communities
    property bool active: false

    onActiveChanged: if (active) root.refresh()
    Component.onCompleted: if (active) root.refresh()

    function refresh() {
        if (!communities) return
        communities.open(communities.page, communities.communityId)
    }

    readonly property string page: communities ? communities.page : "discover"

    function members(count) { return count === 1 ? "1 member" : count + " members" }
    readonly property var tabs: [["discover", "Discover"], ["mine", "My communities"], ["memberships", "Memberships"], ["create", "Create"], ["creator", "Creator dashboard"]]

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Flickable {
        id: scroller
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight + 64
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            x: Math.max(32, (scroller.width - 760) / 2)
            y: 32
            width: Math.max(0, Math.min(760, scroller.width - 64))
            spacing: 14

            Text {
                text: "Communities"
                color: Theme.text
                font.family: Theme.displayFamily
                font.pixelSize: 26
                font.weight: Font.Bold
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Icon {
                    Layout.alignment: Qt.AlignTop
                    name: "lock"
                    size: 16
                    color: Theme.accent
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Free and paid channels and groups run by creators. What's posted inside is end-to-end encrypted; the public listing, its price and who's a member are not."
                    color: Theme.text2
                    font.pixelSize: 13
                }
            }

            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: root.tabs
                    delegate: Chip {
                        required property var modelData
                        text: modelData[1]
                        checked: root.page === modelData[0]
                        onClicked: root.communities.open(modelData[0])
                    }
                }
            }

            Banner {
                Layout.fillWidth: true
                text: root.communities ? root.communities.configError : ""
            }
            InfoCard {
                Layout.fillWidth: true
                visible: root.communities !== null && root.communities.config.enabled === false
                tone: "warn"
                text: "Communities aren't open on this server yet. Memberships you already have keep working."
            }
            Banner {
                Layout.fillWidth: true
                error: false
                text: root.communities ? root.communities.notice : ""
            }
            Banner {
                Layout.fillWidth: true
                text: root.communities ? root.communities.error : ""
            }

            Loader {
                Layout.fillWidth: true
                sourceComponent: root.page === "discover" ? discoverPage
                               : root.page === "mine" ? minePage
                               : root.page === "memberships" ? membershipsPage
                               : root.page === "create" ? createPage
                               : root.page === "creator" ? creatorPage
                               : root.page === "community" ? communityPage
                               : root.page === "manage" ? managePage
                               : null
            }
        }
    }

    Component {
        id: createPage
        CommunityCreate { communities: root.communities }
    }
    Component {
        id: creatorPage
        CreatorDashboard { communities: root.communities }
    }
    Component {
        id: communityPage
        CommunityPage { communities: root.communities }
    }
    Component {
        id: managePage
        CommunityManage { communities: root.communities }
    }

    // --- Discover ---
    Component {
        id: discoverPage
        ColumnLayout {
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                SearchField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: "Search communities"
                    maximumLength: 80
                    onTextChanged: searchTimer.restart()
                }
                RvComboBox {
                    id: categoryBox
                    implicitWidth: 220
                    Accessible.name: "Category"
                    options: [{ value: "", label: "All categories" }].concat(root.communities.categories)
                    value: ""
                    onPicked: searchTimer.restart()
                }
            }
            // Unlisted communities open only from an invite link.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                RvTextField {
                    id: inviteField
                    Layout.fillWidth: true
                    placeholderText: "Have an invite link? Paste it here"
                    Accessible.name: "Invite link"
                    onAccepted: if (openInvite.enabled) openInvite.clicked()
                }
                RvButton {
                    id: openInvite
                    secondary: true
                    text: "Open"
                    // The web app's invite link, <site>/?community=<id>&invite=<token>.
                    enabled: root.communities.isCommunityLink(inviteField.text)
                    onClicked: {
                        const link = inviteField.text
                        inviteField.text = ""
                        root.communities.openLink(link)
                    }
                }
            }
            Timer {
                id: searchTimer
                interval: 250
                onTriggered: root.communities.search(searchField.text, categoryBox.value)
            }
            Text {
                visible: !root.communities.discoverLoaded
                text: "Loading…"
                color: Theme.text3
            }
            Text {
                Layout.fillWidth: true
                visible: root.communities.discoverLoaded && root.communities.discover.length === 0
                wrapMode: Text.Wrap
                text: "No communities match. Unlisted communities only open from an invite link."
                color: Theme.text3
            }
            Repeater {
                model: root.communities.discover
                delegate: CommunityRow {
                    required property var modelData
                    Layout.fillWidth: true
                    title: modelData.name
                    subtitle: "@" + modelData.creator.handle + (modelData.creator.verified ? " · verified" : "")
                    body: modelData.description || modelData.previewText || ""
                    letter: modelData.name
                    image: modelData.hasAvatar ? root.communities.imagePath("avatar", modelData.id) : ""
                    pills: [
                        { text: modelData.kind === "channel" ? "Channel" : "Group", tone: "plain" },
                        modelData.price ? { text: root.communities.formatMoney(modelData.price.amountMinor, modelData.price.currency) + "/month", tone: "accent" } : { text: "Free", tone: "plain" },
                        modelData.memberCount !== null && modelData.memberCount !== undefined ? { text: root.members(modelData.memberCount), tone: "plain" } : null,
                        { text: root.communities.categoryLabel(modelData.category), tone: "plain" }
                    ].filter(p => p !== null)
                    onClicked: root.communities.open("community", modelData.id)
                }
            }
        }
    }

    // --- My communities ---
    Component {
        id: minePage
        ColumnLayout {
            spacing: 10
            Text {
                visible: !root.communities.mineLoaded
                text: "Loading…"
                color: Theme.text3
            }
            Text {
                visible: root.communities.mineLoaded && root.communities.mine.length === 0
                text: "You haven't joined any communities yet."
                color: Theme.text3
            }
            Repeater {
                model: root.communities.mine
                delegate: CommunityRow {
                    required property var modelData
                    readonly property string status: modelData.membership.status
                    Layout.fillWidth: true
                    title: modelData.name
                    letter: modelData.name
                    image: modelData.hasAvatar ? root.communities.imagePath("avatar", modelData.id) : ""
                    subtitle: (modelData.kind === "channel" ? "Channel" : "Group")
                              + (modelData.membership.role !== "member" ? " · " + modelData.membership.role : "")
                              + (modelData.status !== "published" ? " · " + modelData.status : "")
                    pills: [status === "active" ? { text: "Member", tone: "accent" }
                          : status === "pending_enrollment" ? { text: "Waiting for key", tone: "warn" }
                          : status === "pending_approval" ? { text: "Requested", tone: "warn" }
                          : status === "banned" ? { text: "Banned", tone: "danger" }
                          : { text: status, tone: "plain" }]
                    onClicked: root.communities.open("community", modelData.id)
                }
            }
        }
    }

    // --- Memberships ---
    Component {
        id: membershipsPage
        ColumnLayout {
            id: membershipsColumn
            spacing: 10
            property var cancelling: null

            ConfirmDialog {
                id: cancelDialog
                title: "Cancel membership?"
                message: membershipsColumn.cancelling
                         ? "Cancel your " + membershipsColumn.cancelling.community.name + " membership? You won't be charged again and keep access until "
                           + root.communities.formatDate(membershipsColumn.cancelling.accessUntil) + "."
                         : ""
                confirmText: "Cancel membership"
                danger: true
                onConfirmed: root.communities.cancelMembership(membershipsColumn.cancelling.id)
            }

            InfoCard {
                Layout.fillWidth: true
                visible: root.communities.confirming.length > 0
                tone: "neutral"
                text: "Confirming your payment with Stripe… Access starts as soon as Stripe confirms it, not before."
                RvButton {
                    secondary: true
                    text: "Check now"
                    enabled: !root.communities.busy
                    onClicked: root.communities.checkConfirmation()
                }
            }
            Text {
                visible: !root.communities.membershipsLoaded
                text: "Loading…"
                color: Theme.text3
            }
            Text {
                visible: root.communities.membershipsLoaded && root.communities.memberships.length === 0
                text: "No paid memberships."
                color: Theme.text3
            }
            Repeater {
                model: root.communities.memberships
                delegate: Card {
                    id: membershipCard
                    required property var modelData
                    readonly property var m: modelData
                    readonly property string status: m.entitlementStatus
                    readonly property var receiptRows: root.communities.receipts[m.id]
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: membershipCard.m.community.name
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text
                        font.weight: Font.DemiBold
                        font.pixelSize: 15
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "@" + (membershipCard.m.creatorHandle || "creator") + " · code " + membershipCard.m.community.code
                        textFormat: Text.PlainText
                        color: Theme.text3
                        font.pixelSize: Theme.fontSmall + 1
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Pill {
                            text: ({ pending: "Processing payment", active: "Active", trialing: "Trial", grace_period: "Payment failed",
                                     cancel_scheduled: "Cancelled — access until period end", expired: "Ended", canceled: "Not completed",
                                     refunded: "Refunded", disputed: "Payment under review", suspended: "Suspended" })[membershipCard.status] || membershipCard.status
                            tone: membershipCard.status === "active" ? "accent"
                                : ["grace_period", "pending", "cancel_scheduled"].includes(membershipCard.status) ? "warn"
                                : ["refunded", "disputed", "suspended"].includes(membershipCard.status) ? "danger" : "plain"
                        }
                        Pill {
                            visible: !!membershipCard.m.price
                            text: membershipCard.m.price ? root.communities.formatMoney(membershipCard.m.price.amountMinor, membershipCard.m.price.currency) + "/month" : ""
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        visible: text.length > 0
                        color: Theme.text2
                        font.pixelSize: 13
                        text: (membershipCard.m.renewsOn ? "Renews on " + root.communities.formatDate(membershipCard.m.renewsOn) + ". " : "")
                              + (membershipCard.status === "cancel_scheduled" ? "Access until " + root.communities.formatDate(membershipCard.m.accessUntil) + "; you won't be charged again. " : "")
                              + (membershipCard.status === "grace_period" ? "Update your card by " + root.communities.formatDate(membershipCard.m.graceUntil) + " to keep access. " : "")
                              + (membershipCard.status === "expired" && membershipCard.m.endedAt ? "Ended on " + root.communities.formatDate(membershipCard.m.endedAt) + ". " : "")
                        textFormat: Text.PlainText
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        RvButton {
                            secondary: true
                            text: "View community"
                            onClicked: root.communities.open("community", membershipCard.m.community.id)
                        }
                        RvButton {
                            visible: (membershipCard.status === "active" || membershipCard.status === "grace_period") && !membershipCard.m.cancelAtPeriodEnd
                            primary: false
                            danger: true
                            text: "Cancel membership"
                            enabled: !root.communities.busy
                            onClicked: {
                                membershipsColumn.cancelling = membershipCard.m
                                cancelDialog.open()
                            }
                        }
                        RvButton {
                            visible: membershipCard.status === "cancel_scheduled" && membershipCard.m.billingStatus !== "canceled"
                            secondary: true
                            text: "Keep membership"
                            enabled: !root.communities.busy
                            onClicked: root.communities.resumeMembership(membershipCard.m.id)
                        }
                        RvButton {
                            visible: membershipCard.m.billingStatus !== "checkout_pending" && membershipCard.m.billingStatus !== "checkout_expired"
                            secondary: true
                            iconName: "external"
                            text: "Manage payment method"
                            enabled: !root.communities.busy
                            onClicked: root.communities.openPortal(membershipCard.m.id)
                        }
                        RvButton {
                            primary: false
                            text: membershipCard.receiptRows ? "Hide receipts" : "Receipts"
                            onClicked: root.communities.toggleReceipts(membershipCard.m.id)
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: !!membershipCard.receiptRows
                        spacing: 4
                        Text {
                            visible: !!membershipCard.receiptRows && membershipCard.receiptRows.length === 0
                            text: "No payments yet."
                            color: Theme.text3
                        }
                        Repeater {
                            model: membershipCard.receiptRows || []
                            delegate: Item {
                                id: receiptRow
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: 24
                                Text {
                                    anchors.left: parent.left
                                    anchors.right: amount.left
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    elide: Text.ElideRight
                                    color: Theme.text2
                                    font.pixelSize: 13
                                    text: root.communities.formatDate(receiptRow.modelData.occurredAt) + " · "
                                          + (receiptRow.modelData.kind === "payment" ? "Payment" : receiptRow.modelData.kind === "refund" ? "Refund" : receiptRow.modelData.kind)
                                          + (receiptRow.modelData.periodStart ? " · " + root.communities.formatDate(receiptRow.modelData.periodStart) + " – " + root.communities.formatDate(receiptRow.modelData.periodEnd) : "")
                                    textFormat: Text.PlainText
                                }
                                Text {
                                    id: amount
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: Theme.text
                                    font.pixelSize: 13
                                    text: (receiptRow.modelData.kind === "refund" ? "−" : "") + root.communities.formatMoney(receiptRow.modelData.amountMinor, receiptRow.modelData.currency)
                                    textFormat: Text.PlainText
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // A community in a list: its picture (or letter), name, who runs it, what it's about, and pills.
    // Anchors rather than layouts inside, so a wrapped description can't feed its height back into its width.
    component CommunityRow: ItemDelegate {
        id: row
        property string title
        property string subtitle
        property string body
        property string letter
        /// Its picture's path (CommunitySession.imagePath), or empty for the letter.
        property string image
        property var pills: []
        implicitHeight: Math.max(72, texts.implicitHeight + 24)
        leftPadding: 14
        rightPadding: 14
        background: Rectangle {
            radius: Theme.radiusControl
            color: row.hovered ? Theme.surface2 : Theme.surface
            border.color: Theme.border
        }
        contentItem: Item {
            Avatar {
                id: thumb
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                size: 40
                group: true
                initial: (row.letter.trim().charAt(0) || "#").toUpperCase()
                tintKey: row.title
                communityImage: row.image
            }
            Column {
                id: texts
                anchors.left: thumb.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                Text {
                    width: texts.width
                    text: row.title
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.text
                    font.weight: Font.DemiBold
                }
                Text {
                    width: texts.width
                    visible: row.subtitle.length > 0
                    text: row.subtitle
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall + 1
                }
                Text {
                    width: texts.width
                    visible: row.body.length > 0
                    text: row.body
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    maximumLineCount: 2
                    wrapMode: Text.Wrap
                    color: Theme.text2
                    font.pixelSize: Theme.fontSmall + 1
                }
                Flow {
                    width: texts.width
                    spacing: 6
                    Repeater {
                        model: row.pills
                        delegate: Pill {
                            required property var modelData
                            text: modelData.text
                            tone: modelData.tone
                        }
                    }
                }
            }
        }
    }
}
