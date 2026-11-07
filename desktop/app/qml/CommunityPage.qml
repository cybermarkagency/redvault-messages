pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Redvault

// One community, as the web app's CommunityPage: its public listing and
// price, the renewal disclosure a buyer ticks together with 18+, join,
// rejoin, leave, report, and the member and waiting states. Subscribing
// opens Stripe's checkout in the browser; access starts only once Stripe
// confirms the payment to the server.
ColumnLayout {
    id: page
    required property CommunitySession communities
    spacing: 14

    readonly property var c: communities.community
    readonly property bool loaded: c.id !== undefined
    readonly property var viewer: loaded ? c.viewer : ({})
    readonly property var membership: viewer.membership || null
    readonly property var subscription: viewer.subscription || null
    readonly property bool isActiveMember: membership !== null && membership.status === "active"
    readonly property bool isPending: membership !== null && membership.status.startsWith("pending")
    readonly property var config: communities.config
    readonly property bool canBuyHere: config.paymentsEnabled === true && (config.purchasePlatforms || []).includes("desktop")
    readonly property bool closedForJoining: loaded && (c.status !== "published" || c.newMembersPaused === true)

    ConfirmDialog {
        id: leaveDialog
        title: "Leave this community?"
        message: page.loaded ? "Leave " + page.c.name + "?"
                               + (page.subscription ? " Your subscription stops renewing; you won't be charged again. You can come back until the paid period ends." : "")
                             : ""
        confirmText: "Leave"
        danger: true
        onConfirmed: page.communities.leave()
    }
    CommunityReportDialog {
        id: reportDialog
        communities: page.communities
    }
    // Another community opened: its boxes start unticked.
    Connections {
        target: page.communities
        function onPageChanged() {
            termsBox.checked = false
            ageBox.checked = false
        }
    }

    Text {
        visible: !page.loaded && page.communities.communityError.length === 0
        text: "Loading…"
        color: Theme.text3
    }
    Banner {
        Layout.fillWidth: true
        text: page.communities.communityError
    }

    // The banner, when there is one (a card's colour shows until it has loaded).
    Rectangle {
        id: banner
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(200, Math.round(width / 3))
        visible: page.loaded && page.c.hasBanner === true
        radius: Theme.radiusCard
        color: Theme.surface2
        Image {
            anchors.fill: parent
            asynchronous: true
            // Cut to this size by the provider, twice over for high-DPI screens.
            sourceSize: Qt.size(Math.max(1, width) * 2, Math.max(1, height) * 2)
            source: banner.visible
                    ? "image://community/banner/" + page.communities.imageRevision + "/"
                      + encodeURIComponent(page.communities.imagePath("banner", page.c.id))
                    : ""
        }
    }

    // The listing.
    RowLayout {
        Layout.fillWidth: true
        visible: page.loaded
        spacing: 14
        Avatar {
            Layout.alignment: Qt.AlignTop
            size: 56
            group: true
            initial: page.loaded ? ((page.c.name || "").trim().charAt(0) || "#").toUpperCase() : ""
            tintKey: page.loaded ? page.c.name : ""
            communityImage: page.loaded && page.c.hasAvatar ? page.communities.imagePath("avatar", page.c.id) : ""
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Text {
                Layout.fillWidth: true
                text: page.loaded ? page.c.name : ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.text
                font.family: Theme.displayFamily
                font.pixelSize: 24
                font.weight: Font.Bold
            }
            Text {
                Layout.fillWidth: true
                text: page.loaded ? "by " + page.c.creator.displayName + " (@" + page.c.creator.handle + ")" + (page.c.creator.verified ? " · verified creator" : "") : ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: 13
            }
        }
    }
    Flow {
        Layout.fillWidth: true
        visible: page.loaded
        spacing: 6
        Pill { text: page.loaded && page.c.kind === "channel" ? "Channel: the creator posts, members read" : "Group: members talk together" }
        Pill { text: page.loaded ? page.communities.categoryLabel(page.c.category) : "" }
        Pill {
            visible: page.loaded && page.c.memberCount !== null && page.c.memberCount !== undefined
            text: page.loaded ? (page.c.memberCount === 1 ? "1 member" : page.c.memberCount + " members") : ""
        }
        Pill {
            visible: page.loaded && page.c.status !== "published"
            tone: "warn"
            text: page.loaded ? page.c.status : ""
        }
        Pill { text: page.loaded ? "Code " + page.c.code : "" }
    }
    Text {
        Layout.fillWidth: true
        visible: page.loaded && (page.c.description || "").length > 0
        text: page.loaded ? page.c.description : ""
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.text
    }

    // A member.
    Rectangle {
        Layout.fillWidth: true
        visible: page.isActiveMember
        implicitHeight: memberColumn.implicitHeight + 32
        radius: Theme.radiusCard
        color: Theme.accentSoft
        border.color: Theme.accentBorder
        ColumnLayout {
            id: memberColumn
            anchors.fill: parent
            anchors.margins: 16
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: "You're a member" + (page.membership && page.membership.role !== "member" ? " (" + page.membership.role + ")" : "") + "."
                textFormat: Text.PlainText
                color: Theme.text
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.Wrap
                color: Theme.text2
                font.pixelSize: 13
                text: !page.subscription ? ""
                    : page.subscription.entitlementStatus === "cancel_scheduled" ? "Your membership ends on " + page.communities.formatDate(page.subscription.accessUntil) + "."
                    : page.subscription.renewsOn ? "Renews on " + page.communities.formatDate(page.subscription.renewsOn) + "."
                    : ""
                textFormat: Text.PlainText
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                RvButton {
                    iconName: "chat"
                    text: page.loaded && page.c.kind === "channel" ? "Open channel" : "Open chat"
                    onClicked: page.communities.openChat()
                }
                RvButton {
                    visible: page.viewer.isStaff === true
                    secondary: true
                    iconName: "sliders"
                    text: "Manage"
                    onClicked: page.communities.open("manage", page.c.id)
                }
                RvButton {
                    visible: page.membership !== null && page.membership.role !== "owner"
                    primary: false
                    danger: true
                    text: "Leave"
                    enabled: !page.communities.busy
                    onClicked: leaveDialog.open()
                }
            }
        }
    }

    InfoCard {
        Layout.fillWidth: true
        visible: page.membership !== null && ["pending_enrollment", "pending_approval", "banned"].includes(page.membership.status)
        tone: page.membership !== null && page.membership.status === "banned" ? "error" : "neutral"
        text: page.membership === null ? ""
            : page.membership.status === "pending_enrollment" ? "You're in. Your app is waiting for the community's encryption key, which the creator's or another member's app delivers automatically the next time one of them is online. Nothing to do on your side."
            : page.membership.status === "pending_approval" ? "Your request to join was sent. A moderator will review it."
            : page.membership.status === "banned" ? "You can't join this community."
            : ""
    }
    InfoCard {
        Layout.fillWidth: true
        visible: page.subscription !== null && page.subscription.entitlementStatus === "pending"
        text: "Your payment is being confirmed with Stripe. This page updates once it's done."
    }

    // Joining: free, rejoining a paid period, or subscribing.
    Card {
        Layout.fillWidth: true
        visible: page.loaded && !page.isActiveMember && !page.isPending && !(page.membership !== null && page.membership.status === "banned")
        spacing: 10

        // Free.
        ColumnLayout {
            Layout.fillWidth: true
            visible: page.loaded && page.c.accessType === "free"
            spacing: 8
            Text {
                text: "Free to join"
                color: Theme.text
                font.weight: Font.DemiBold
            }
            Text {
                visible: page.closedForJoining && !page.c.staff
                text: "Not accepting new members right now."
                color: Theme.text3
                font.pixelSize: 13
            }
            RvButton {
                text: "Join"
                enabled: !page.communities.busy && !page.closedForJoining
                onClicked: page.communities.join()
            }
        }

        // Paid, with a paid period still running.
        ColumnLayout {
            Layout.fillWidth: true
            visible: page.loaded && page.c.accessType === "paid" && page.subscription !== null
                     && (page.subscription.entitlementStatus === "cancel_scheduled" || page.subscription.entitlementStatus === "active")
            spacing: 8
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: page.subscription ? "You still have a paid membership until " + page.communities.formatDate(page.subscription.accessUntil) + "." : ""
                textFormat: Text.PlainText
                color: Theme.text
                font.weight: Font.DemiBold
            }
            RvButton {
                text: "Rejoin"
                enabled: !page.communities.busy
                onClicked: page.communities.join()
            }
        }

        // Paid: subscribe.
        ColumnLayout {
            id: buy
            Layout.fillWidth: true
            visible: page.loaded && page.c.accessType === "paid" && !!page.c.price
                     && !(page.subscription !== null && (page.subscription.entitlementStatus === "cancel_scheduled" || page.subscription.entitlementStatus === "active"))
            spacing: 10
            readonly property string price: page.loaded && page.c.price ? page.communities.formatMoney(page.c.price.amountMinor, page.c.price.currency) : ""

            Row {
                spacing: 6
                Text {
                    id: priceText
                    text: buy.price
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: 22
                    font.weight: Font.Bold
                }
                Text {
                    anchors.baseline: priceText.baseline
                    text: "/ month"
                    color: Theme.text3
                    font.pixelSize: 14
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: 13
                text: "Billed monthly by RedVault through Stripe. Your card statement shows RedVault; this community's code is " + (page.loaded ? page.c.code : "") + "."
                textFormat: Text.PlainText
            }
            ConsentBox {
                id: termsBox
                Layout.fillWidth: true
                label: "I agree to the community membership terms. This membership <b>renews automatically every month</b> at " + buy.price
                       + " until I cancel. I can cancel any time in Communities → Memberships and keep access until the end of the period I paid for. Refunds follow the terms."
            }
            ConsentBox {
                id: ageBox
                Layout.fillWidth: true
                label: "I'm 18 or older."
            }
            InfoCard {
                Layout.fillWidth: true
                visible: !page.canBuyHere
                tone: "warn"
                text: "Memberships can't be bought here right now."
            }
            RvButton {
                text: page.communities.busy ? "Opening checkout…" : "Subscribe"
                iconName: "external"
                enabled: !page.communities.busy && termsBox.checked && ageBox.checked && page.canBuyHere && !page.closedForJoining
                onClicked: page.communities.subscribe(termsBox.checked, ageBox.checked)
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: Theme.fontSmall + 1
                text: page.closedForJoining ? "Not accepting new members right now."
                                            : "Stripe's checkout opens in your browser. Access starts once Stripe confirms the payment, not before."
                textFormat: Text.PlainText
            }
        }

        Text {
            visible: page.loaded && page.c.accessType === "paid" && !page.c.price
            text: "This community has no price yet."
            color: Theme.text3
        }
    }

    // Preview, rules, privacy.
    Section {
        visible: page.loaded && (page.c.previewText || "").length > 0
        caption: "PREVIEW"
        body: page.loaded ? page.c.previewText : ""
        quoted: true
    }
    Section {
        visible: page.loaded && (page.c.rules || "").length > 0
        caption: "RULES"
        body: page.loaded ? page.c.rules : ""
    }
    Section {
        visible: page.loaded
        caption: "PRIVACY"
        body: "Messages, images, voice notes and files inside are end-to-end encrypted: neither RedVault nor Stripe can read them. RedVault does see this public listing, who is a member, and the state of each membership's billing. When someone leaves, the members' apps switch to a new key they never get."
        muted: true
    }
    RvButton {
        visible: page.loaded
        primary: false
        danger: true
        iconName: "flag"
        text: "Report this community"
        onClicked: reportDialog.reportCommunity(page.c.id)
    }

    component Section: ColumnLayout {
        id: section
        property string caption
        property string body
        property bool quoted: false
        property bool muted: false
        Layout.fillWidth: true
        spacing: 6
        Text {
            text: section.caption
            textFormat: Text.PlainText
            color: Theme.text3
            font.pixelSize: 11
            font.weight: Font.DemiBold
            font.letterSpacing: 0.8
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: bodyText.implicitHeight + (section.quoted ? 24 : 0)
            radius: Theme.radiusSmall
            color: section.quoted ? Theme.surface : "transparent"
            border.color: section.quoted ? Theme.border : "transparent"
            Text {
                id: bodyText
                anchors.fill: parent
                anchors.margins: section.quoted ? 12 : 0
                text: section.body
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: section.muted ? Theme.text3 : Theme.text2
                font.pixelSize: 13
            }
        }
    }
}
