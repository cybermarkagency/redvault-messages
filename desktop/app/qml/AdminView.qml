// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The admin tab, for RedVault's super admins: the Communities section of the
// web app's admin panel (screens/AdminCommunities.tsx). Overview with the
// commission rule versions, transactions with refunds and reconciliation,
// creators, communities and the reports RedVault reviews. The server lets
// only super admins in and records every action in its audit log. Money is
// display only: every amount shown is the server's.
Item {
    id: root
    required property CommunityAdminSession admin
    property bool active: false

    onActiveChanged: if (active && admin) admin.open(admin.section)
    Component.onCompleted: if (active && admin) admin.open(admin.section)

    readonly property string section: admin ? admin.section : "overview"
    readonly property var sections: [["overview", "Overview"], ["transactions", "Transactions"], ["creators", "Creators"], ["communities", "Communities"], ["reports", "Reports"]]

    function usd(n) { return App.communities ? App.communities.formatMoney(n, "usd") : "" }
    function money(n, currency) { return App.communities ? App.communities.formatMoney(n, currency) : "" }
    function rate(bps) { return App.communities ? App.communities.formatRate(bps) : "" }
    function date(iso) { return App.communities ? App.communities.formatDate(iso) : "" }

    // One "are you sure?" for every action that needs it: `run` is what happens on yes.
    ConfirmDialog {
        id: confirm
        property var run: null
        danger: true
        function ask(title, text, button, action) {
            confirm.title = title
            confirm.message = text
            confirm.confirmText = button
            confirm.run = action
            confirm.open()
        }
        onConfirmed: if (confirm.run) confirm.run()
    }

    RvDialog {
        id: refundDialog
        property var transaction: null
        title: "Refund this payment?"
        width: 460
        onOpened: {
            refundAmount.text = ""
            refundReason.text = ""
            refundAmount.forceActiveFocus()
        }
        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 10
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.text2
                text: refundDialog.transaction
                      ? "Refund how much? Leave it blank to refund the whole " + root.money(refundDialog.transaction.grossAmountMinor, refundDialog.transaction.currency)
                        + ". The creator's share is taken back from their Stripe balance."
                      : ""
                textFormat: Text.PlainText
            }
            RvTextField {
                id: refundAmount
                Layout.fillWidth: true
                placeholderText: "Amount in USD (blank = all of it)"
                Accessible.name: "Refund amount"
                maximumLength: 12
            }
            RvTextField {
                id: refundReason
                Layout.fillWidth: true
                placeholderText: "Reason (kept in the audit log)"
                Accessible.name: "Refund reason"
                maximumLength: 200
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                RvButton {
                    primary: false
                    text: "Cancel"
                    onClicked: refundDialog.close()
                }
                RvButton {
                    danger: true
                    text: "Refund"
                    enabled: refundReason.text.trim().length > 0 && !root.admin.busy
                    onClicked: {
                        refundDialog.close()
                        root.admin.refund(refundDialog.transaction.id, refundAmount.text, refundReason.text)
                    }
                }
            }
        }
    }

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
            x: Math.max(32, (scroller.width - 900) / 2)
            y: 32
            width: Math.max(0, Math.min(900, scroller.width - 64))
            spacing: 14

            Text {
                text: "Admin"
                color: Theme.text
                font.family: Theme.displayFamily
                font.pixelSize: 26
                font.weight: Font.Bold
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Communities: commission, money, creators, communities and reports. RedVault staff only; the server records every action here in its audit log. Users, other reports and team messages are on the web admin panel."
                color: Theme.text2
                font.pixelSize: 13
            }

            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: root.sections
                    delegate: Chip {
                        required property var modelData
                        text: modelData[1]
                        checked: root.section === modelData[0]
                        onClicked: root.admin.open(modelData[0])
                    }
                }
            }

            Banner {
                Layout.fillWidth: true
                error: false
                text: root.admin ? root.admin.notice : ""
            }
            Banner {
                Layout.fillWidth: true
                text: root.admin ? root.admin.error : ""
            }
            Text {
                visible: root.admin !== null && !root.admin.loaded
                text: "Loading…"
                color: Theme.text3
            }

            Loader {
                Layout.fillWidth: true
                active: root.admin !== null
                sourceComponent: root.section === "overview" ? overviewPage
                               : root.section === "transactions" ? transactionsPage
                               : root.section === "creators" ? creatorsPage
                               : root.section === "communities" ? communitiesPage
                               : reportsPage
            }
        }
    }

    // --- Overview ---
    Component {
        id: overviewPage
        ColumnLayout {
            id: overview
            spacing: 14
            readonly property var o: root.admin.overview
            readonly property bool has: o.rules !== undefined
            readonly property var current: has && o.rules.length > 0 ? o.rules[0] : null

            // The form starts from the current version.
            function fill() {
                if (!overview.current) return
                rateField.text = String(overview.current.rateBps / 100)
                minField.text = (overview.current.minPriceMinor / 100).toFixed(2)
                maxField.text = (overview.current.maxPriceMinor / 100).toFixed(2)
            }
            onCurrentChanged: fill()
            Component.onCompleted: fill()

            Flow {
                Layout.fillWidth: true
                visible: overview.has
                spacing: 10
                Repeater {
                    model: overview.has ? [
                        ["Net commission (ledger)", overview.o.platformCommissionNetMinor],
                        ["Stripe processing fees", overview.o.processingFeesMinor],
                        ["Refund losses", overview.o.refundLossesMinor],
                        ["Dispute losses", overview.o.disputeLossesMinor],
                        ["Platform balance at Stripe (ledger)", overview.o.providerClearingMinor]
                    ] : []
                    delegate: Rectangle {
                        id: tile
                        required property var modelData
                        width: 200
                        height: tileColumn.implicitHeight + 24
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
                                text: tile.modelData[0]
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                                color: Theme.text3
                                font.pixelSize: 12
                            }
                            Text {
                                width: parent.width
                                text: root.usd(tile.modelData[1])
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                color: Theme.text
                                font.pixelSize: 20
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                }
            }
            InfoCard {
                Layout.fillWidth: true
                visible: overview.has && (overview.o.mismatches > 0 || overview.o.unbalancedJournals > 0)
                tone: "warn"
                text: overview.has ? overview.o.mismatches + " transaction(s) don't match Stripe and " + overview.o.unbalancedJournals
                                     + " ledger journal(s) don't balance. See Transactions." : ""
            }

            Caption { text: "COMMISSION AND PRICE LIMITS" }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: 13
                text: "A change is a new version for memberships started from now on. Recorded transactions and existing memberships never change."
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 3
                columnSpacing: 10
                rowSpacing: 6
                FieldLabel { text: "Commission (10–15%)" }
                FieldLabel { text: "Minimum monthly price (USD)" }
                FieldLabel { text: "Maximum monthly price (USD)" }
                RvTextField {
                    id: rateField
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    maximumLength: 6
                    Accessible.name: "Commission percent"
                }
                RvTextField {
                    id: minField
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    maximumLength: 10
                    Accessible.name: "Minimum monthly price"
                }
                RvTextField {
                    id: maxField
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    maximumLength: 10
                    Accessible.name: "Maximum monthly price"
                }
            }
            FieldLabel { text: "Note (why, for the audit log)" }
            RvTextField {
                id: noteField
                Layout.fillWidth: true
                maximumLength: 500
                Accessible.name: "Note"
            }
            RvButton {
                text: "Save new version"
                enabled: !root.admin.busy && overview.has
                onClicked: confirm.ask("New commission version?",
                                       "Set the commission to " + rateField.text.trim() + "% for NEW memberships from now on? Existing memberships keep their rate.",
                                       "Save new version",
                                       () => root.admin.addCommissionRule(rateField.text, minField.text, maxField.text, noteField.text))
            }
            Table {
                Layout.fillWidth: true
                weights: [1, 1, 2.2, 1.6, 3]
                header: ["Version", "Rate", "Price limits", "From", "Note"]
                empty: "No commission rule yet."
                rows: overview.has ? overview.o.rules.map(r => [r.version, root.rate(r.rateBps), root.usd(r.minPriceMinor) + " – " + root.usd(r.maxPriceMinor),
                                                                root.date(r.effectiveFrom), r.note || ""]) : []
            }
        }
    }

    // --- Transactions ---
    Component {
        id: transactionsPage
        ColumnLayout {
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                RvComboBox {
                    implicitWidth: 260
                    Accessible.name: "Reconciliation"
                    options: [{ value: "", label: "All" }, { value: "mismatch", label: "Mismatched with Stripe" },
                              { value: "pending", label: "Awaiting Stripe fees" }, { value: "matched", label: "Matched" }]
                    value: root.admin.transactionFilter
                    onPicked: (value) => root.admin.setTransactionFilter(value)
                }
                Item { Layout.fillWidth: true }
                RvButton {
                    secondary: true
                    iconName: "refresh"
                    text: "Reconcile now"
                    enabled: !root.admin.busy
                    onClicked: root.admin.reconcile()
                }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: transactionColumn.implicitHeight + 16
                radius: Theme.radiusControl
                color: Theme.surface
                border.color: Theme.border
                Column {
                    id: transactionColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
                    readonly property var weights: [1.5, 1, 1.2, 1.7, 1.2, 1, 1.3, 1]
                    readonly property var numeric: [false, false, true, true, true, true, false, false]
                    Cells {
                        width: transactionColumn.width
                        heading: true
                        weights: transactionColumn.weights
                        numeric: transactionColumn.numeric
                        cells: ["Date", "Kind", "Gross", "Commission", "Creator", "Fees", "Reconciliation", ""]
                    }
                    Text {
                        visible: root.admin.loaded && root.admin.transactions.length === 0
                        height: 30
                        leftPadding: 6
                        verticalAlignment: Text.AlignVCenter
                        text: "No transactions."
                        color: Theme.text3
                        font.pixelSize: 13
                    }
                    Repeater {
                        model: root.admin.transactions
                        delegate: Cells {
                            id: transaction
                            required property var modelData
                            width: transactionColumn.width
                            weights: transactionColumn.weights
                            numeric: transactionColumn.numeric
                            cells: [root.date(modelData.occurredAt), modelData.kind,
                                    root.money(modelData.grossAmountMinor, modelData.currency),
                                    root.money(modelData.commissionAmountMinor, modelData.currency) + " (" + root.rate(modelData.commissionBps) + ")",
                                    root.money(modelData.creatorAmountMinor, modelData.currency),
                                    modelData.processingFeeMinor === null || modelData.processingFeeMinor === undefined ? "—" : root.money(modelData.processingFeeMinor, modelData.currency),
                                    modelData.reconciliationStatus + (modelData.reconciliationNote ? " ⓘ" : "")]
                            tip: modelData.reconciliationNote || ""
                            actionText: modelData.kind === "payment" ? "Refund" : ""
                            actionEnabled: !root.admin.busy
                            onAction: {
                                refundDialog.transaction = transaction.modelData
                                refundDialog.open()
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Creators ---
    Component {
        id: creatorsPage
        ColumnLayout {
            spacing: 8
            Text {
                visible: root.admin.loaded && root.admin.creators.length === 0
                text: "No creators yet."
                color: Theme.text3
            }
            Repeater {
                model: root.admin.creators
                delegate: AdminRow {
                    id: creatorRow
                    required property var modelData
                    readonly property var c: modelData
                    Layout.fillWidth: true
                    title: c.displayName + " @" + c.handle + (c.verified ? " · verified" : "")
                    subtitle: c.status + " · since " + root.date(c.createdAt) + " · payouts: "
                              + (c.payments ? c.payments.status + (c.payments.requirementsDue ? " (info due)" : "") + (c.payments.livemode ? "" : " · test mode") : "not set up")
                    RvButton {
                        secondary: true
                        text: creatorRow.c.verified ? "Remove badge" : "Verify"
                        enabled: !root.admin.busy
                        onClicked: root.admin.setCreatorVerified(creatorRow.c.id, !creatorRow.c.verified)
                    }
                    RvButton {
                        visible: creatorRow.c.status === "active"
                        primary: false
                        danger: true
                        text: "Suspend"
                        enabled: !root.admin.busy
                        onClicked: confirm.ask("Suspend this creator?", "Suspend " + creatorRow.c.displayName + "? All their communities are suspended and billing pauses.",
                                               "Suspend", () => root.admin.setCreatorStatus(creatorRow.c.id, "suspended"))
                    }
                    RvButton {
                        visible: creatorRow.c.status !== "active"
                        secondary: true
                        text: "Reinstate"
                        enabled: !root.admin.busy
                        onClicked: root.admin.setCreatorStatus(creatorRow.c.id, "active")
                    }
                }
            }
        }
    }

    // --- Communities ---
    Component {
        id: communitiesPage
        ColumnLayout {
            spacing: 8
            Text {
                visible: root.admin.loaded && root.admin.communityList.length === 0
                text: "No communities yet."
                color: Theme.text3
            }
            Repeater {
                model: root.admin.communityList
                delegate: AdminRow {
                    id: communityRow
                    required property var modelData
                    readonly property var c: modelData
                    Layout.fillWidth: true
                    title: c.name
                    subtitle: c.kind + " · " + c.accessType + " · " + c.status + " · " + c.visibility + " · epoch " + c.keyEpoch
                    RvButton {
                        visible: communityRow.c.status === "suspended"
                        secondary: true
                        text: "Unsuspend"
                        enabled: !root.admin.busy
                        onClicked: confirm.ask("Lift the suspension?", "Lift the suspension of " + communityRow.c.name + "? Billing resumes.",
                                               "Unsuspend", () => root.admin.communityAction(communityRow.c.id, "unsuspend"))
                    }
                    RvButton {
                        visible: communityRow.c.status !== "suspended" && communityRow.c.status !== "closed"
                        primary: false
                        danger: true
                        text: "Suspend"
                        enabled: !root.admin.busy
                        onClicked: confirm.ask("Suspend this community?", "Suspend " + communityRow.c.name + "? Nobody can read or post, and billing pauses.",
                                               "Suspend", () => root.admin.communityAction(communityRow.c.id, "suspend"))
                    }
                    RvButton {
                        visible: communityRow.c.status !== "closed"
                        primary: false
                        danger: true
                        text: "Close now"
                        enabled: !root.admin.busy
                        onClicked: confirm.ask("Close this community now?", "Close " + communityRow.c.name + " now? Subscriptions end immediately and every member loses access. Refund separately if owed.",
                                               "Close now", () => root.admin.communityAction(communityRow.c.id, "close"))
                    }
                }
            }
        }
    }

    // --- Reports ---
    Component {
        id: reportsPage
        ColumnLayout {
            spacing: 10
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: 13
                text: "Reports RedVault reviews: about communities and creators, and every child-safety, non-consensual imagery or other illegal-content report. Message text appears only when the reporter chose to share it."
            }
            Text {
                visible: root.admin.loaded && root.admin.reports.length === 0
                text: "No open reports."
                color: Theme.text3
            }
            Repeater {
                model: root.admin.reports
                delegate: Card {
                    id: reportCard
                    required property var modelData
                    readonly property var r: modelData
                    readonly property bool severe: r.category === "child_safety" || r.category === "nonconsensual_imagery"
                    Layout.fillWidth: true
                    spacing: 8
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Pill {
                            text: reportCard.r.category.replace(/_/g, " ")
                            tone: reportCard.severe ? "danger" : "warn"
                        }
                        Pill { text: reportCard.r.targetType }
                        Text {
                            height: 24
                            verticalAlignment: Text.AlignVCenter
                            text: root.date(reportCard.r.createdAt)
                            textFormat: Text.PlainText
                            color: Theme.text3
                            font.pixelSize: 13
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: !!reportCard.r.details
                        text: reportCard.r.details || ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.text
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        visible: !!reportCard.r.evidencePlaintext
                        implicitHeight: quote.implicitHeight + 16
                        radius: Theme.radiusControl
                        color: Theme.surface2
                        Text {
                            id: quote
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: 10
                            text: reportCard.r.evidencePlaintext || ""
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: Theme.text2
                            font.italic: true
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: !!reportCard.r.evidenceCiphertextSha256
                        text: "Stored ciphertext SHA-256: " + (reportCard.r.evidenceCiphertextSha256 || "")
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        color: Theme.text3
                        font.pixelSize: 12
                        font.family: Theme.monoFamily
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "Community " + (reportCard.r.communityId || "—") + (reportCard.r.targetUserId ? " · user " + reportCard.r.targetUserId : "")
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        color: Theme.text3
                        font.pixelSize: 12
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        RvButton {
                            secondary: true
                            text: "Actioned"
                            enabled: !root.admin.busy
                            onClicked: root.admin.resolveReport(reportCard.r.id, "actioned")
                        }
                        RvButton {
                            secondary: true
                            text: "Dismiss"
                            enabled: !root.admin.busy
                            onClicked: root.admin.resolveReport(reportCard.r.id, "dismissed")
                        }
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

    // A list row: a title, a line under it, and its buttons on the right.
    // Anchors rather than layouts inside, so wrapped text can't feed its height back into its width.
    component AdminRow: Rectangle {
        id: adminRow
        property string title
        property string subtitle
        default property alias actions: actionRow.data
        implicitHeight: Math.max(64, texts.implicitHeight + 24)
        radius: Theme.radiusControl
        color: Theme.surface
        border.color: Theme.border
        Column {
            id: texts
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.right: actionRow.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            Text {
                width: texts.width
                text: adminRow.title
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.text
                font.weight: Font.DemiBold
            }
            Text {
                width: texts.width
                text: adminRow.subtitle
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: Theme.fontSmall + 1
            }
        }
        Row {
            id: actionRow
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
        }
    }

    // A simple table, columns sized by weight, numbers to the right (as the creator dashboard's).
    component Table: Rectangle {
        id: table
        property var header: []
        property var rows: []
        property var weights: []
        property string empty: ""
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
            Cells {
                width: tableColumn.width
                heading: true
                weights: table.weights
                cells: table.header
            }
            Text {
                visible: table.rows.length === 0
                height: 30
                leftPadding: 6
                verticalAlignment: Text.AlignVCenter
                text: table.empty
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: 13
            }
            Repeater {
                model: table.rows
                delegate: Cells {
                    required property var modelData
                    width: tableColumn.width
                    weights: table.weights
                    cells: modelData
                }
            }
        }
    }

    // One table row. With `actionText`, the last column is a small button.
    component Cells: Item {
        id: cellRow
        property var cells: []
        property var weights: []
        property var numeric: []
        property bool heading: false
        property string actionText: ""
        property bool actionEnabled: true
        property string tip: ""
        signal action()
        readonly property real totalWeight: weights.reduce((a, b) => a + b, 0)
        height: 34
        Row {
            anchors.fill: parent
            Repeater {
                model: cellRow.cells
                delegate: Text {
                    required property var modelData
                    required property int index
                    width: cellRow.width * cellRow.weights[index] / cellRow.totalWeight
                    height: cellRow.height
                    leftPadding: 6
                    rightPadding: 6
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: cellRow.numeric[index] ? Text.AlignRight : Text.AlignLeft
                    text: String(modelData)
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: cellRow.heading ? Theme.text3 : Theme.text2
                    font.pixelSize: 13
                    font.weight: cellRow.heading ? Font.DemiBold : Font.Normal
                }
            }
        }
        HoverHandler { id: hover }
        ToolTip.visible: hover.hovered && cellRow.tip.length > 0
        ToolTip.text: cellRow.tip
        RvButton {
            visible: cellRow.actionText.length > 0
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            implicitHeight: 28
            leftPadding: 10
            rightPadding: 10
            primary: false
            danger: true
            text: cellRow.actionText
            enabled: cellRow.actionEnabled
            onClicked: cellRow.action()
        }
    }
}
