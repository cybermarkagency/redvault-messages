// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// The creator dashboard, as the web app's CreatorDashboard: becoming a
// creator, then Overview, Communities, Earnings, Payouts and Settings. Every
// number is the server's (from its ledger and from Stripe); earnings aren't
// payouts. Stripe's onboarding and Express dashboard open in the browser,
// and RedVault never sees identity, tax or bank details.
ColumnLayout {
    id: dash
    required property CommunitySession communities
    spacing: 14

    readonly property var creator: communities.creator
    readonly property var config: communities.config
    readonly property var dashData: communities.dashboard
    readonly property bool hasData: dashData.totals !== undefined
    readonly property string currency: hasData ? dashData.totals.currency : "usd"
    property string tab: "overview"

    function money(n) { return communities.formatMoney(n, currency) }
    readonly property var monthRows: hasData ? dashData.months.slice().reverse().map(m => [m.month, m.newMembers, m.renewals, m.cancellations, m.refunds, money(m.grossMinor), money(m.creatorNetMinor)]) : []

    Text {
        visible: !dash.communities.creatorLoaded
        text: "Loading…"
        color: Theme.text3
    }

    // --- Becoming a creator ---
    ColumnLayout {
        id: become
        Layout.fillWidth: true
        visible: dash.communities.creatorLoaded && !dash.creator
        spacing: 10
        InfoCard {
            Layout.fillWidth: true
            tone: "ok"
            text: "Become a creator"
            detail: "Run free or paid channels and groups from this account. Your creator handle is public and separate from your private RedVault username. To be paid, you'll verify your identity and bank details with Stripe; RedVault never sees them."
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 10
            rowSpacing: 6
            FieldLabel { text: "Creator handle (public)" }
            FieldLabel { text: "Display name" }
            RvTextField {
                id: handle
                Layout.fillWidth: true
                maximumLength: 32
                placeholderText: "a–z, 0–9, _"
                validator: RegularExpressionValidator { regularExpression: /[A-Za-z0-9_]{0,32}/ }
                onTextEdited: text = text.toLowerCase()
                Accessible.name: "Creator handle"
            }
            RvTextField {
                id: displayName
                Layout.fillWidth: true
                maximumLength: 60
                Accessible.name: "Display name"
            }
        }
        FieldLabel { text: "About (public)" }
        PostField {
            id: bio
            Layout.fillWidth: true
            limit: 1000
            Accessible.name: "About"
        }
        FieldLabel { text: "Link (optional, https://)" }
        RvTextField {
            id: link
            Layout.fillWidth: true
            maximumLength: 200
            Accessible.name: "Link"
        }
        ConsentBox {
            id: acceptTerms
            Layout.fillWidth: true
            label: "I accept the creator terms (version " + (dash.config.creatorTermsVersion || "current") + "): I'm responsible for what I publish; no illegal content, scams, sexual content involving minors, intimate images shared without consent, or other prohibited material; RedVault keeps a commission on each payment as shown when I set a price; payouts need Stripe's identity checks; RedVault can suspend communities that break the rules."
        }
        RvButton {
            text: "Create creator profile"
            enabled: !dash.communities.busy && acceptTerms.checked && handle.text.length >= 3 && displayName.text.trim().length > 0 && !bio.tooLong
            onClicked: dash.communities.becomeCreator(handle.text, displayName.text, bio.text, link.text)
        }
    }

    // --- The dashboard ---
    ColumnLayout {
        Layout.fillWidth: true
        visible: dash.communities.creatorLoaded && !!dash.creator
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 14
            Avatar {
                size: 52
                group: true
                initial: dash.creator ? (dash.creator.displayName || "?").charAt(0).toUpperCase() : ""
                tintKey: dash.creator ? dash.creator.handle : ""
                communityImage: dash.creator && dash.creator.hasAvatar ? dash.communities.creatorAvatarPath(dash.creator.id) : ""
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    Layout.fillWidth: true
                    text: dash.creator ? dash.creator.displayName : ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.text
                    font.family: Theme.displayFamily
                    font.pixelSize: 22
                    font.weight: Font.Bold
                }
                Text {
                    text: dash.creator ? "@" + dash.creator.handle + (dash.creator.verified ? " · verified" : "") + (dash.creator.status !== "active" ? " · " + dash.creator.status : "") : ""
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.pixelSize: 13
                }
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: [["overview", "Overview"], ["communities", "Communities"], ["earnings", "Earnings"], ["payouts", "Payouts"], ["settings", "Settings"]]
                delegate: Chip {
                    required property var modelData
                    text: modelData[1]
                    checked: dash.tab === modelData[0]
                    onClicked: {
                        dash.tab = modelData[0]
                        if (modelData[0] !== "settings") dash.communities.loadDashboard()
                    }
                }
            }
        }

        Text {
            visible: dash.tab !== "settings" && !dash.hasData
            text: "Loading…"
            color: Theme.text3
        }

        // Overview
        ColumnLayout {
            Layout.fillWidth: true
            visible: dash.tab === "overview" && dash.hasData
            spacing: 10
            StatGrid {
                Layout.fillWidth: true
                stats: dash.hasData ? [
                    { label: "Paying members", value: String(dash.dashData.totals.payingMembers) },
                    { label: "Communities", value: String(dash.dashData.communities.length) },
                    { label: "Monthly recurring (estimate)", value: dash.money(dash.dashData.totals.monthlyRecurringEstimateMinor), hint: "If every renewing member renews" },
                    { label: "Gross paid", value: dash.money(dash.dashData.totals.grossPaidMinor), hint: "All time, before commission" },
                    { label: "RedVault commission", value: dash.money(dash.dashData.totals.platformCommissionMinor) },
                    { label: "Refunds", value: dash.money(dash.dashData.totals.refundsMinor) },
                    { label: "Disputes", value: dash.money(dash.dashData.totals.disputesMinor), hint: "Borne by RedVault" },
                    { label: "Your earnings", value: dash.money(dash.dashData.totals.creatorEarningsMinor), hint: "Earned, not the same as paid out" },
                    { label: "Still subscribed", value: dash.dashData.retention.stillActive + " of " + dash.dashData.retention.everPaid, hint: "Members who ever paid" },
                    { label: "Renewals failing now", value: String(dash.dashData.failedRenewals), hint: "In their grace period" }
                ] : []
            }
            MonthsTable {
                Layout.fillWidth: true
                rows: dash.monthRows
            }
        }

        // Communities
        ColumnLayout {
            Layout.fillWidth: true
            visible: dash.tab === "communities" && dash.hasData
            spacing: 8
            RvButton {
                iconName: "plus"
                text: "New community"
                onClicked: dash.communities.open("create")
            }
            Text {
                visible: dash.hasData && dash.dashData.communities.length === 0
                text: "No communities yet."
                color: Theme.text3
            }
            Repeater {
                model: dash.hasData ? dash.dashData.communities : []
                delegate: Rectangle {
                    id: communityRow
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 64
                    radius: Theme.radiusControl
                    color: Theme.surface
                    border.color: Theme.border
                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 14
                        anchors.right: rowSide.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            width: parent.width
                            text: communityRow.modelData.name
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.text
                            font.weight: Font.DemiBold
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            color: Theme.text3
                            font.pixelSize: Theme.fontSmall + 1
                            text: (communityRow.modelData.kind === "channel" ? "Channel" : "Group") + " · "
                                  + (communityRow.modelData.accessType === "paid" && communityRow.modelData.price ? dash.money(communityRow.modelData.price.amountMinor) + "/month" : "Free")
                                  + " · " + communityRow.modelData.activeMembers + " members"
                                  + (communityRow.modelData.accessType === "paid" ? " · " + communityRow.modelData.payingMembers + " paying" : "")
                            textFormat: Text.PlainText
                        }
                    }
                    Row {
                        id: rowSide
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8
                        Pill {
                            anchors.verticalCenter: parent.verticalCenter
                            text: communityRow.modelData.status
                            tone: communityRow.modelData.status === "published" ? "accent" : "warn"
                        }
                        RvButton {
                            secondary: true
                            text: "Manage"
                            onClicked: dash.communities.open("manage", communityRow.modelData.id)
                        }
                    }
                }
            }
        }

        // Earnings
        ColumnLayout {
            Layout.fillWidth: true
            visible: dash.tab === "earnings" && dash.hasData
            spacing: 10
            InfoCard {
                Layout.fillWidth: true
                text: "Gross is what members paid (before tax). RedVault keeps its commission from each payment and pays Stripe's processing fees. Your earnings go to your Stripe balance with each payment; refunds take your share back. Earnings aren't payouts: Stripe pays your balance out to your bank on its schedule (see Payouts)."
            }
            StatGrid {
                Layout.fillWidth: true
                stats: dash.hasData ? [
                    { label: "Gross paid", value: dash.money(dash.dashData.totals.grossPaidMinor) },
                    { label: "RedVault commission", value: dash.money(dash.dashData.totals.platformCommissionMinor) },
                    { label: "Refunds", value: dash.money(dash.dashData.totals.refundsMinor) },
                    { label: "Your earnings", value: dash.money(dash.dashData.totals.creatorEarningsMinor) }
                ] : []
            }
            MonthsTable {
                Layout.fillWidth: true
                rows: dash.monthRows
            }
        }

        // Payouts
        ColumnLayout {
            Layout.fillWidth: true
            visible: dash.tab === "payouts" && dash.hasData
            spacing: 10
            InfoCard {
                Layout.fillWidth: true
                visible: dash.hasData && !dash.dashData.payouts.ready
                tone: "warn"
                text: "Payouts aren't set up yet. Finish Stripe onboarding in Settings."
            }
            StatGrid {
                Layout.fillWidth: true
                visible: dash.hasData && !!dash.dashData.payouts.balance
                stats: dash.hasData && dash.dashData.payouts.balance
                       ? dash.dashData.payouts.balance.available.map(b => ({ label: "Available for payout", value: dash.communities.formatMoney(b.amountMinor, b.currency), hint: "In your Stripe balance" }))
                         .concat(dash.dashData.payouts.balance.pending.map(b => ({ label: "Pending", value: dash.communities.formatMoney(b.amountMinor, b.currency), hint: "Not yet available" })))
                       : []
            }
            Table {
                Layout.fillWidth: true
                weights: [2, 2, 2, 2]
                numeric: [false, false, false, true]
                header: ["Created", "Status", "Arrives", "Amount"]
                rows: dash.hasData ? dash.dashData.payouts.recent.map(p => [dash.communities.formatDate(p.createdAt), p.status.replace("_", " "),
                                                                        dash.communities.formatDate(p.arrivalDate), dash.communities.formatMoney(p.amountMinor, p.currency)]) : []
                empty: "No payouts yet."
            }
            RvButton {
                visible: !!dash.creator && dash.creator.payments.status !== "not_started"
                secondary: true
                iconName: "external"
                text: "Open Stripe dashboard"
                enabled: !dash.communities.busy
                onClicked: dash.communities.openStripeDashboard()
            }
        }

        // Settings
        ColumnLayout {
            id: settings
            Layout.fillWidth: true
            visible: dash.tab === "settings"
            spacing: 10
            readonly property var payments: dash.creator ? dash.creator.payments : ({})

            // The form starts from the profile as loaded.
            Connections {
                target: dash.communities
                function onCreatorChanged() { settings.fill() }
            }
            Component.onCompleted: fill()
            function fill() {
                if (!dash.creator) return
                profileName.text = dash.creator.displayName || ""
                profileBio.text = dash.creator.bio || ""
                profileLink.text = dash.creator.profileLink || ""
            }

            Caption { text: "PUBLIC PROFILE" }
            FieldLabel { text: "Display name" }
            RvTextField {
                id: profileName
                Layout.fillWidth: true
                maximumLength: 60
                Accessible.name: "Display name"
            }
            FieldLabel { text: "About" }
            PostField {
                id: profileBio
                Layout.fillWidth: true
                limit: 1000
                Accessible.name: "About"
            }
            FieldLabel { text: "Link (https://)" }
            RvTextField {
                id: profileLink
                Layout.fillWidth: true
                maximumLength: 200
                Accessible.name: "Link"
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                RvButton {
                    text: "Save profile"
                    enabled: !dash.communities.busy && !profileBio.tooLong
                    onClicked: dash.communities.saveCreatorProfile(profileName.text, profileBio.text, profileLink.text)
                }
                RvButton {
                    secondary: true
                    iconName: "image"
                    text: "Choose picture…"
                    enabled: !dash.communities.busy
                    onClicked: pictureDialog.open()
                }
            }
            FileDialog {
                id: pictureDialog
                title: "Choose a picture (PNG, JPEG or WebP, up to 300 KB)"
                nameFilters: ["Images (*.png *.jpg *.jpeg *.webp)"]
                onAccepted: dash.communities.setCreatorPicture(selectedFile)
            }

            Caption {
                Layout.topMargin: 8
                text: "PAID MEMBERSHIPS AND PAYOUTS"
            }
            InfoCard {
                Layout.fillWidth: true
                visible: settings.payments.configured !== true
                text: "Paid memberships aren't available on this server yet. Free communities work."
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: settings.payments.configured === true
                spacing: 10
                RvSwitch {
                    Layout.fillWidth: true
                    label: "Offer paid memberships"
                    description: "Members subscribe monthly through Stripe."
                    checked: !!dash.creator && dash.creator.paidMembershipsEnabled === true
                    enabled: !dash.communities.busy
                    onToggled: (on) => dash.communities.setPaidMemberships(on)
                }
                InfoCard {
                    Layout.fillWidth: true
                    tone: settings.payments.ready ? "ok" : "neutral"
                    text: settings.payments.ready ? "Payouts are set up. You can publish paid communities."
                        : settings.payments.status === "not_started" ? "To publish a paid community, verify your identity and add a bank account with Stripe. RedVault never sees those details."
                        : settings.payments.status === "rejected" ? "Stripe can't offer payouts to this account."
                        : settings.payments.requirementsDue ? "Stripe needs more information before you can be paid."
                        : "Stripe is reviewing your details."
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: !!dash.creator && dash.creator.paidMembershipsEnabled === true && !settings.payments.ready && settings.payments.status !== "rejected"
                    spacing: 8
                    RvComboBox {
                        id: country
                        visible: settings.payments.status === "not_started"
                        Accessible.name: "Country"
                        options: (dash.config.creatorCountries || ["us"]).map(c => ({ value: c, label: ({ us: "United States", gb: "United Kingdom", ca: "Canada", au: "Australia", nz: "New Zealand", ie: "Ireland", de: "Germany", fr: "France" })[c] || c.toUpperCase() }))
                        value: (dash.config.creatorCountries || ["us"])[0]
                    }
                    RvButton {
                        iconName: "external"
                        text: settings.payments.status === "not_started" ? "Set up payouts with Stripe" : "Continue with Stripe"
                        enabled: !dash.communities.busy
                        onClicked: dash.communities.startOnboarding(country.value)
                    }
                    RvButton {
                        visible: settings.payments.status !== "not_started"
                        secondary: true
                        text: "Check status"
                        enabled: !dash.communities.busy
                        onClicked: dash.communities.refreshOnboarding()
                    }
                }
            }
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

    // Number tiles, as many to a row as fit.
    component StatGrid: GridLayout {
        id: grid
        property var stats: []
        columns: Math.max(1, Math.floor(width / 180))
        columnSpacing: 10
        rowSpacing: 10
        Repeater {
            model: grid.stats
            delegate: Rectangle {
                id: tile
                required property var modelData
                Layout.fillWidth: true
                // Every tile in a row as tall as the tallest.
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                implicitHeight: tileColumn.implicitHeight + 24
                radius: Theme.radiusControl
                color: Theme.surface
                border.color: Theme.border
                Column {
                    id: tileColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    spacing: 4
                    Text {
                        width: parent.width
                        text: tile.modelData.label
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.text3
                        font.pixelSize: 12
                    }
                    Text {
                        width: parent.width
                        text: tile.modelData.value
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        visible: !!tile.modelData.hint
                        text: tile.modelData.hint || ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.text3
                        font.pixelSize: 11
                    }
                }
            }
        }
    }

    // A simple table: columns sized by weight, numbers to the right.
    component Table: Rectangle {
        id: table
        property var header: []
        property var rows: []
        property var weights: []
        property var numeric: []
        property string empty: ""
        readonly property real totalWeight: weights.reduce((a, b) => a + b, 0)
        implicitHeight: tableColumn.implicitHeight + 16
        radius: Theme.radiusControl
        color: Theme.surface
        border.color: Theme.border
        Column {
            id: tableColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 8
            Repeater {
                model: [table.header].concat(table.rows.length > 0 ? table.rows : [[table.empty]])
                delegate: Row {
                    id: tableRow
                    required property var modelData
                    required property int index
                    width: tableColumn.width
                    height: 30
                    Repeater {
                        model: tableRow.modelData
                        delegate: Text {
                            required property var modelData
                            required property int index
                            width: tableRow.modelData.length === 1 ? tableRow.width : tableRow.width * table.weights[index] / table.totalWeight
                            height: 30
                            leftPadding: 6
                            rightPadding: 6
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: table.numeric[index] && tableRow.modelData.length > 1 ? Text.AlignRight : Text.AlignLeft
                            text: String(modelData)
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: tableRow.index === 0 ? Theme.text3 : Theme.text2
                            font.pixelSize: 13
                            font.weight: tableRow.index === 0 ? Font.DemiBold : Font.Normal
                        }
                    }
                }
            }
        }
    }

    component MonthsTable: Table {
        weights: [2, 1, 1, 1.4, 1, 1.4, 1.4]
        numeric: [false, true, true, true, true, true, true]
        header: ["Month", "New", "Renewals", "Cancellations", "Refunds", "Gross", "Your net"]
        empty: "No payments yet."
    }
}
