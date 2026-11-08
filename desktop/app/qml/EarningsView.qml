// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// Earnings: the creator dashboard for paid messages, tips and exclusive
// posts (Overview, Paid messages, Tips, Exclusive posts, Transactions,
// Payouts), and Library, the posts you bought. As the web app's
// screens/CreatorEarnings.tsx and Library.tsx. Every amount is the
// server's; payments and payouts happen on Stripe's pages in the browser.
Item {
    id: root
    required property EarningsSession earnings
    property bool active: false
    property string tab: "overview"
    readonly property var dash: earnings ? earnings.dashboard : ({})
    readonly property var settings: earnings ? earnings.settings : ({})
    readonly property var creator: earnings ? earnings.creator : ({})

    onActiveChanged: if (active) refresh()
    onTabChanged: if (active) refresh()
    Component.onCompleted: if (active) refresh()

    function refresh() {
        if (!earnings) return
        earnings.clearMessages()
        earnings.refresh()
        if (tab === "library") {
            earnings.loadLibrary()
            return
        }
        earnings.loadDashboard()
        if (tab === "tips") earnings.loadTips()
        if (tab === "posts") earnings.loadMyPosts()
        if (tab === "payouts" || !creator.loaded) earnings.loadCreator()
    }

    function product(name) {
        const list = dash.products || []
        for (let i = 0; i < list.length; ++i)
            if (list[i].product === name) return list[i]
        return { count: 0, net: "$0.00", percent: 0, gross: "$0.00", fees: "$0.00", refunded: "$0.00" }
    }

    component Stat: Rectangle {
        id: stat
        property string label
        property string value
        property string hint: ""
        Layout.fillWidth: true
        implicitHeight: statColumn.implicitHeight + 24
        radius: Theme.radiusControl
        color: Theme.panel
        border.color: Theme.border
        ColumnLayout {
            id: statColumn
            anchors.fill: parent
            anchors.margins: 12
            spacing: 2
            Text { textFormat: Text.PlainText; text: stat.label; color: Theme.text3; font.pixelSize: Theme.fontSmall }
            Text { textFormat: Text.PlainText; text: stat.value; color: Theme.text; font.pixelSize: 20; font.weight: Font.Bold }
            Text { textFormat: Text.PlainText; visible: stat.hint.length > 0; text: stat.hint; color: Theme.text3; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap; Layout.fillWidth: true }
        }
    }

    component Para: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        color: Theme.text2
        font.pixelSize: 13
    }

    component SectionLabel: Text {
        Layout.topMargin: 6
        color: Theme.text3
        font.pixelSize: Theme.fontSmall
        font.weight: Font.DemiBold
        font.letterSpacing: 0.6
    }

    component Field: ColumnLayout {
        id: field
        property string label
        default property alias content: holder.data
        Layout.fillWidth: true
        spacing: 4
        Text { textFormat: Text.PlainText; text: field.label; color: Theme.text; font.pixelSize: Theme.fontSmall + 1; font.weight: Font.DemiBold }
        ColumnLayout { id: holder; Layout.fillWidth: true; spacing: 4 }
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
            x: Math.max(32, (scroller.width - 760) / 2)
            y: 32
            width: Math.max(0, Math.min(760, scroller.width - 64))
            spacing: 14

            Text {
                text: "Earnings"
                color: Theme.text
                font.family: Theme.displayFamily
                font.pixelSize: 26
                font.weight: Font.Bold
            }
            Para {
                text: "Paid messages, tips and exclusive posts. Paying happens on Stripe's page in your browser; RedVault never sees cards or bank details, and paying never lets anyone read your messages."
            }

            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: [["overview", "Overview"], ["paid-messages", "Paid messages"], ["tips", "Tips"], ["posts", "Exclusive posts"],
                            ["transactions", "Transactions"], ["payouts", "Payouts"], ["library", "Library"]]
                    delegate: Chip {
                        required property var modelData
                        text: modelData[1]
                        checked: root.tab === modelData[0]
                        onClicked: root.tab = modelData[0]
                    }
                }
            }

            Banner {
                Layout.fillWidth: true
                visible: root.earnings !== null && root.settings.loaded === true && !root.earnings.enabled && root.tab !== "library"
                error: false
                text: "Paid messages, tips and exclusive posts aren't switched on on this server yet. Nothing can be charged until they are."
            }
            Banner {
                Layout.fillWidth: true
                visible: root.earnings !== null && root.earnings.enabled && root.settings.loaded === true && !root.settings.creatorReady
                         && ["paid-messages", "tips", "posts", "overview"].indexOf(root.tab) >= 0
                error: false
                text: "Finish payout setup (Payouts tab) before charging for anything. Stripe verifies who you are; RedVault never sees your bank details."
            }
            RowLayout {
                Layout.fillWidth: true
                visible: root.earnings !== null && (root.earnings.error.length > 0 || root.earnings.notice.length > 0)
                Banner {
                    Layout.fillWidth: true
                    error: root.earnings !== null && root.earnings.error.length > 0
                    text: root.earnings ? (root.earnings.error.length > 0 ? root.earnings.error : root.earnings.notice) : ""
                }
                RvButton {
                    primary: false
                    iconName: "x"
                    implicitHeight: 30
                    ToolTip.visible: hovered
                    ToolTip.text: "Dismiss"
                    onClicked: root.earnings.clearMessages()
                }
            }

            // --- Overview ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.tab === "overview"
                spacing: 12
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Stat { label: "Gross"; value: root.dash.gross || "$0.00" }
                    Stat { label: "RedVault fees"; value: root.dash.fees || "$0.00" }
                    Stat { label: "Your earnings"; value: root.dash.net || "$0.00" }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: (root.dash.awaitingKeys || 0) > 0
                    Banner {
                        Layout.fillWidth: true
                        error: false
                        text: root.dash.awaitingKeys + " buyer(s) are waiting for a post key. This app sends them within a minute while it's open."
                    }
                    RvButton {
                        secondary: true
                        text: "Send now"
                        onClicked: root.earnings.deliverKeys()
                    }
                }
                Repeater {
                    model: root.dash.products || []
                    delegate: Rectangle {
                        id: productCard
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: productRow.implicitHeight + 24
                        radius: Theme.radiusControl
                        color: Theme.surface
                        border.color: Theme.border
                        RowLayout {
                            id: productRow
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 12
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { textFormat: Text.PlainText; text: productCard.modelData.name; color: Theme.text; font.weight: Font.DemiBold }
                                Text {
                                    text: productCard.modelData.count + " sold · RedVault keeps " + productCard.modelData.percent + "% · refunded " + productCard.modelData.refunded
                                          + (productCard.modelData.disputes > 0 ? " · " + productCard.modelData.disputes + " disputed" : "")
                                    textFormat: Text.PlainText
                                    color: Theme.text3
                                    font.pixelSize: Theme.fontSmall
                                }
                            }
                            ColumnLayout {
                                Layout.preferredWidth: 120
                                spacing: 2
                                Text { textFormat: Text.PlainText; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; text: productCard.modelData.net; color: Theme.text; font.weight: Font.Bold }
                                Text { textFormat: Text.PlainText; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; text: "of " + productCard.modelData.gross; color: Theme.text3; font.pixelSize: Theme.fontSmall }
                            }
                        }
                    }
                }
                Para {
                    text: "Your earnings are what's left after RedVault's fee; RedVault pays Stripe's processing fees. Refunds and disputes take a sale back out. Money reaches your bank on Stripe's payout schedule (Payouts tab)."
                }
            }

            // --- Paid messages ---
            ColumnLayout {
                id: paidTab
                Layout.fillWidth: true
                visible: root.tab === "paid-messages"
                spacing: 12
                property string mode: root.settings.mode || "off"
                Connections {
                    target: root.earnings
                    function onSettingsChanged() {
                        paidTab.mode = root.settings.mode || "off"
                        priceField.text = root.settings.priceText || "3.00"
                    }
                }
                Para {
                    text: "Charge people who aren't your friends to message you. Friends, people you let in, and chats you started are always free. Blocking and reporting are always free, and nobody can pay their way past a block. Messages stay end-to-end encrypted: paying never lets anyone (RedVault or Stripe) read them."
                }
                Banner {
                    Layout.fillWidth: true
                    visible: root.settings.loaded === true && root.settings.acceptsMessageRequests === false
                    text: "You don't accept message requests, so strangers can't start a chat with you at all. Turn them on in Privacy settings."
                }
                Repeater {
                    model: [["off", "Off", "Anyone who can message you does so for free."],
                            ["first_contact", "First message", "One payment to start a chat; the rest of that chat is free."],
                            ["per_message", "Every message", "Each message from someone who isn't exempt costs the price."]]
                    delegate: AbstractButton {
                        id: modeButton
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: modeColumn.implicitHeight + 20
                        checkable: true
                        checked: paidTab.mode === modelData[0]
                        onClicked: paidTab.mode = modelData[0]
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData[1]
                        background: Rectangle {
                            radius: Theme.radiusControl
                            color: modeButton.checked ? Theme.accentSoft : Theme.surface
                            border.color: modeButton.checked ? Theme.accent : Theme.border
                        }
                        contentItem: ColumnLayout {
                            id: modeColumn
                            spacing: 2
                            Text { textFormat: Text.PlainText; leftPadding: 12; topPadding: 4; text: modeButton.modelData[1]; color: Theme.text; font.weight: Font.DemiBold }
                            Text { textFormat: Text.PlainText; leftPadding: 12; text: modeButton.modelData[2]; color: Theme.text2; font.pixelSize: Theme.fontSmall }
                        }
                    }
                }
                Field {
                    visible: paidTab.mode !== "off"
                    label: "Price (USD)"
                    RvTextField {
                        id: priceField
                        Layout.preferredWidth: 160
                        text: root.settings.priceText || "3.00"
                        Accessible.name: "Price in US dollars"
                    }
                    Text {
                        text: "Between " + (root.earnings ? root.earnings.rates.messageMin : "") + " and " + (root.earnings ? root.earnings.rates.messageMax : "")
                              + ". RedVault keeps " + root.product("paid_message").percent + "% of each paid message."
                        textFormat: Text.PlainText
                        color: Theme.text3
                        font.pixelSize: Theme.fontSmall
                    }
                }
                RvButton {
                    text: "Save"
                    enabled: root.earnings !== null && !root.earnings.busy
                    onClicked: root.earnings.saveMessageSettings(paidTab.mode, priceField.text)
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Stat { label: "Paid messages"; value: String(root.product("paid_message").count) }
                    Stat { label: "Your earnings"; value: root.product("paid_message").net }
                    Stat { label: "Exempt people"; value: String(root.settings.exemptCount || 0); hint: "Add them from a chat's details" }
                }
                Para {
                    text: "Unused paid messages are refunded after 14 days. A safety contact (once every 30 days per person) always gets through for free, so nobody has to pay to warn you about something urgent; misuse can be reported."
                }
            }

            // --- Tips ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.tab === "tips"
                spacing: 12
                Para {
                    text: "Tips are voluntary thank-yous: they don't unlock anything, and nobody has to tip to talk to you. People choose $1, $3, $5, $10 or another amount, and can tip without showing you who they are (RedVault and Stripe still know who paid). RedVault keeps "
                          + root.product("tip").percent + "% of each tip."
                }
                RvButton {
                    secondary: root.settings.tipsEnabled === true
                    text: root.settings.tipsEnabled === true ? "Stop taking tips" : "Take tips"
                    enabled: root.earnings !== null && !root.earnings.busy
                    onClicked: root.earnings.setTipsEnabled(!root.settings.tipsEnabled)
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Stat { label: "Tips"; value: String(root.product("tip").count) }
                    Stat { label: "Your earnings"; value: root.product("tip").net }
                }
                Text {
                    visible: root.earnings !== null && root.earnings.tips.length === 0
                    text: "No tips yet."
                    color: Theme.text3
                }
                Repeater {
                    model: root.earnings ? root.earnings.tips : []
                    delegate: RowLayout {
                        id: tipRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 10
                        Text { textFormat: Text.PlainText; text: tipRow.modelData.amount; color: Theme.text; font.weight: Font.DemiBold }
                        Text {
                            Layout.fillWidth: true
                            text: (tipRow.modelData.from === "A supporter" ? "From a supporter" : "From " + tipRow.modelData.from) + " · " + tipRow.modelData.when
                            textFormat: Text.PlainText
                            color: Theme.text2
                            elide: Text.ElideRight
                        }
                        Chip { text: tipRow.modelData.statusLabel; checked: tipRow.modelData.status === "consumed"; enabled: false }
                    }
                }
            }

            // --- Exclusive posts ---
            ColumnLayout {
                id: postsTab
                Layout.fillWidth: true
                visible: root.tab === "posts"
                spacing: 12
                property url file: ""
                property string fileName: ""
                Connections {
                    target: root.earnings
                    function onPublished() {
                        postText.text = ""
                        teaserField.text = ""
                        postsTab.file = ""
                        postsTab.fileName = ""
                    }
                }
                Para {
                    text: "Sell a post once to each buyer. It's encrypted with its own key on this computer; the server stores only ciphertext. After a payment is confirmed, this app seals the key to the buyer (while it's open: keep it running now and then). A buyer who doesn't get the key within 72 hours is refunded automatically. RedVault keeps "
                          + root.product("exclusive_post").percent + "%. Anyone who can see a post can still photograph their screen."
                }
                Field {
                    label: "Post (encrypted)"
                    PostField {
                        id: postText
                        Layout.fillWidth: true
                        limit: 10000
                        placeholderText: "What buyers get"
                    }
                }
                Field {
                    visible: root.earnings !== null && root.earnings.postMedia
                    label: "Photo or video (optional, encrypted)"
                    RowLayout {
                        spacing: 8
                        RvButton {
                            secondary: true
                            iconName: "paperclip"
                            text: postsTab.fileName.length > 0 ? "Change" : "Add a photo or video"
                            onClicked: postFileDialog.open()
                        }
                        Text { textFormat: Text.PlainText; visible: postsTab.fileName.length > 0; text: postsTab.fileName; color: Theme.text2; elide: Text.ElideMiddle; Layout.maximumWidth: 280 }
                        RvButton {
                            visible: postsTab.fileName.length > 0
                            primary: false
                            text: "Remove"
                            onClicked: {
                                postsTab.file = ""
                                postsTab.fileName = ""
                            }
                        }
                    }
                    Text {
                        text: "Photos are redrawn here, which leaves location and camera details behind. Videos are encrypted as they are."
                        color: Theme.text3
                        font.pixelSize: Theme.fontSmall
                    }
                }
                Field {
                    label: "Public teaser (optional)"
                    RvTextField {
                        id: teaserField
                        Layout.fillWidth: true
                        maximumLength: 280
                        Accessible.name: "Public teaser"
                    }
                    Para {
                        font.pixelSize: Theme.fontSmall
                        color: Theme.text3
                        text: "Not encrypted: anyone who can see your posts can read it, and so can the server. Leave it empty to show only the price."
                    }
                }
                Field {
                    label: "Price (USD)"
                    RvTextField {
                        id: postPrice
                        Layout.preferredWidth: 160
                        text: "5.00"
                        Accessible.name: "Price in US dollars"
                    }
                }
                RvButton {
                    text: root.earnings && root.earnings.publishStage.length > 0 ? "Encrypting and uploading..." : "Publish"
                    enabled: root.earnings !== null && root.earnings.publishStage.length === 0 && root.earnings.enabled && root.settings.creatorReady === true
                             && !postText.tooLong
                    onClicked: root.earnings.publishPost(postText.text, teaserField.text, postPrice.text, postsTab.file)
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Stat { label: "Sold"; value: String(root.product("exclusive_post").count) }
                    Stat { label: "Your earnings"; value: root.product("exclusive_post").net }
                }
                Text {
                    visible: root.earnings !== null && root.earnings.myPosts.length === 0
                    text: "No posts yet."
                    color: Theme.text3
                }
                Repeater {
                    model: root.earnings ? root.earnings.myPosts : []
                    delegate: ExclusivePostCard {
                        required property var modelData
                        earnings: root.earnings
                        post: modelData
                    }
                }
                FileDialog {
                    id: postFileDialog
                    title: "Add a photo or video"
                    fileMode: FileDialog.OpenFile
                    nameFilters: ["Photos and videos (*.jpg *.jpeg *.png *.webp *.gif *.bmp *.mp4 *.webm *.mov)"]
                    onAccepted: {
                        const check = root.earnings.checkPostFile(selectedFile)
                        if (check.error.length > 0) {
                            postsTab.file = ""
                            postsTab.fileName = ""
                            fileProblem.message = check.error
                            fileProblem.open()
                            return
                        }
                        postsTab.file = selectedFile
                        postsTab.fileName = check.name
                    }
                }
                ConfirmDialog {
                    id: fileProblem
                    title: "Can't use that file"
                    confirmText: "OK"
                }
            }

            // --- Transactions ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.tab === "transactions"
                spacing: 12
                property string refundId: ""
                property string refundAmount: ""
                Para {
                    text: "Each payment from Stripe, as confirmed by its webhook. RedVault's fee is taken from each payment; Stripe's processing fees are RedVault's to pay."
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Stat { label: "Gross"; value: root.dash.gross || "$0.00" }
                    Stat { label: "RedVault fees"; value: root.dash.fees || "$0.00" }
                    Stat { label: "Your earnings"; value: root.dash.net || "$0.00" }
                }
                Text {
                    visible: (root.dash.transactions || []).length === 0
                    text: "No payments yet."
                    color: Theme.text3
                }
                Repeater {
                    model: root.dash.transactions || []
                    delegate: Rectangle {
                        id: txRow
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: txLayout.implicitHeight + 20
                        radius: Theme.radiusControl
                        color: Theme.surface
                        border.color: Theme.border
                        RowLayout {
                            id: txLayout
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 10
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { textFormat: Text.PlainText; text: txRow.modelData.productName + " · " + txRow.modelData.amount; color: Theme.text; font.weight: Font.DemiBold }
                                Text {
                                    text: txRow.modelData.when + " · from " + txRow.modelData.from + " · RedVault fee " + txRow.modelData.fee
                                    textFormat: Text.PlainText
                                    color: Theme.text3
                                    font.pixelSize: Theme.fontSmall
                                }
                            }
                            Chip { text: txRow.modelData.statusLabel; checked: txRow.modelData.status === "consumed" || txRow.modelData.status === "paid"; enabled: false }
                            RvButton {
                                visible: txRow.modelData.refundable
                                primary: false
                                text: "Refund"
                                onClicked: {
                                    refundDialog.purchaseId = txRow.modelData.id
                                    refundDialog.message = "Refund " + txRow.modelData.amount + "? The buyer gets it all back and your share is taken back."
                                    refundDialog.open()
                                }
                            }
                        }
                    }
                }
                ConfirmDialog {
                    id: refundDialog
                    property string purchaseId: ""
                    title: "Refund this payment?"
                    confirmText: "Refund"
                    danger: true
                    onConfirmed: root.earnings.refund(purchaseId)
                }
            }

            // --- Payouts (the creator profile shared with paid communities) ---
            ColumnLayout {
                id: payoutsTab
                Layout.fillWidth: true
                visible: root.tab === "payouts"
                spacing: 12

                Text {
                    visible: root.creator.loaded !== true
                    text: "Loading..."
                    color: Theme.text3
                }

                // No creator profile yet.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.creator.loaded === true && root.creator.exists !== true
                    spacing: 10
                    Para {
                        text: "To be paid, create a creator profile, then verify your identity and bank details with Stripe; RedVault never sees them. Your creator handle is public and separate from your private RedVault username."
                    }
                    Field {
                        label: "Creator handle (public)"
                        RvTextField {
                            id: handleField
                            Layout.preferredWidth: 280
                            maximumLength: 32
                            placeholderText: "a-z, 0-9, _"
                            onTextChanged: if (text !== text.toLowerCase()) text = text.toLowerCase()
                        }
                    }
                    Field {
                        label: "Display name"
                        RvTextField {
                            id: displayNameField
                            Layout.preferredWidth: 280
                            maximumLength: 60
                        }
                    }
                    ConsentBox {
                        id: termsBox
                        Layout.fillWidth: true
                        label: "I accept the creator terms (version " + (root.creator.termsVersion || "current")
                               + "): I'm responsible for what I publish; no illegal content, scams, sexual content involving minors, intimate images shared without consent, or other prohibited material; RedVault keeps a commission on each payment as shown when I set a price; payouts need Stripe's identity checks; RedVault can suspend creators who break the rules."
                    }
                    RvButton {
                        text: "Create creator profile"
                        enabled: root.earnings !== null && !root.earnings.busy && termsBox.checked && handleField.text.length >= 3 && displayNameField.text.trim().length > 0
                        onClicked: root.earnings.becomeCreator(handleField.text, displayNameField.text)
                    }
                }

                // A creator: payout setup and balance.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.creator.loaded === true && root.creator.exists === true
                    spacing: 10
                    Text {
                        text: "@" + (root.creator.handle || "") + " · " + (root.creator.displayName || "")
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.weight: Font.DemiBold
                    }
                    Banner {
                        Layout.fillWidth: true
                        visible: root.creator.paymentsEnabled !== true
                        error: false
                        text: "Payouts aren't available on this server yet."
                    }
                    Banner {
                        Layout.fillWidth: true
                        visible: root.creator.paymentsEnabled === true
                        error: root.creator.status === "rejected"
                        text: root.creator.ready === true ? "Payouts are set up. You can charge for messages, take tips and sell posts."
                            : root.creator.status === "not_started" ? "Verify your identity and add a bank account with Stripe. RedVault never sees those details. (This also lets you publish paid communities in the web app.)"
                            : root.creator.status === "rejected" ? "Stripe can't offer payouts to this account."
                            : root.creator.requirementsDue === true ? "Stripe needs more information before you can be paid."
                            : "Stripe is reviewing your details."
                    }
                    RowLayout {
                        visible: root.creator.paymentsEnabled === true && root.creator.ready !== true && root.creator.status !== "rejected"
                        spacing: 8
                        ComboBox {
                            id: countryBox
                            visible: root.creator.status === "not_started"
                            model: root.creator.countries || []
                            textRole: "name"
                            valueRole: "code"
                            implicitWidth: 200
                            implicitHeight: 36
                            Accessible.name: "Country"
                            background: Rectangle {
                                radius: Theme.radiusControl
                                color: Theme.surface
                                border.color: countryBox.activeFocus ? Theme.accent : Theme.border
                            }
                            contentItem: Text {
                                textFormat: Text.PlainText
                                leftPadding: 12
                                text: countryBox.displayText
                                color: Theme.text
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                        }
                        RvButton {
                            text: root.creator.status === "not_started" ? "Set up payouts with Stripe" : "Continue with Stripe"
                            enabled: root.earnings !== null && !root.earnings.busy
                            onClicked: root.earnings.startPayoutSetup(countryBox.currentValue || "us")
                        }
                        RvButton {
                            visible: root.creator.status !== "not_started"
                            secondary: true
                            text: "Check status"
                            enabled: root.earnings !== null && !root.earnings.busy
                            onClicked: root.earnings.refreshPayoutSetup()
                        }
                    }
                    RvButton {
                        visible: root.creator.ready === true
                        secondary: true
                        iconName: "external"
                        text: "Open your Stripe dashboard"
                        onClicked: root.earnings.openStripeDashboard()
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: root.dash.hasBalance === true
                        spacing: 10
                        Stat { label: "Available"; value: root.dash.balanceAvailable || "$0.00" }
                        Stat { label: "On the way"; value: root.dash.balancePending || "$0.00" }
                    }
                    SectionLabel {
                        visible: (root.dash.payouts || []).length > 0
                        text: "RECENT PAYOUTS"
                    }
                    Repeater {
                        model: root.dash.payouts || []
                        delegate: RowLayout {
                            id: payoutRow
                            required property var modelData
                            Layout.fillWidth: true
                            Text { textFormat: Text.PlainText; text: payoutRow.modelData.amount; color: Theme.text; font.weight: Font.DemiBold }
                            Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: payoutRow.modelData.arrival.length > 0 ? "Arrives " + payoutRow.modelData.arrival : ""; color: Theme.text2 }
                            Text { textFormat: Text.PlainText; text: payoutRow.modelData.status; color: Theme.text3 }
                        }
                    }
                    Para {
                        text: "Payouts follow Stripe's schedule. Taxes, refunds and disputes can change what you receive; see the creator terms."
                    }
                }
            }

            // --- Library: posts you bought ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.tab === "library"
                spacing: 12
                Para {
                    text: "Exclusive posts you bought. Each opens with a key the creator's app sealed to you; RedVault can't open them."
                }
                Text {
                    visible: root.earnings !== null && root.earnings.libraryLoaded && root.earnings.library.length === 0
                    text: "Nothing here yet. Exclusive posts you buy show up here."
                    color: Theme.text3
                }
                Repeater {
                    model: root.earnings ? root.earnings.library : []
                    delegate: ColumnLayout {
                        id: libraryItem
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 4
                        Text {
                            text: libraryItem.modelData.creatorName.length > 0
                                  ? libraryItem.modelData.creatorName + (libraryItem.modelData.creatorHandle.length > 0 ? " · @" + libraryItem.modelData.creatorHandle : "") : ""
                            textFormat: Text.PlainText
                            visible: text.length > 0
                            color: Theme.text2
                            font.pixelSize: Theme.fontSmall + 1
                        }
                        ExclusivePostCard {
                            earnings: root.earnings
                            post: libraryItem.modelData
                        }
                    }
                }
            }
        }
    }
}
