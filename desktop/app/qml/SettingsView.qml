// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// Settings, as a page with a section list (the desktop design): account and
// plan, privacy, security, notifications, calls and audio, and the network
// pages (linked devices, transparency log, relay servers).
Item {
    id: page
    required property AccountSession account
    property string section: "account"

    // Which security form is open: "", "password", "recovery", "mfaOn", "mfaOff", "plan", "pin".
    property string form: ""
    // The last action that finished, so its confirmation shows next to it.
    property string lastDone: ""

    function openForm(name) {
        account.clearMessages()
        account.dismissSecrets()
        form = name
        currentPassword.text = ""
        newPassword.text = ""
        confirmPassword.text = ""
        mfaCode.text = ""
        if (name === "mfaOn")
            account.startMfaSetup()
    }

    onVisibleChanged: {
        if (visible) {
            account.reload()
        } else {
            account.dismissSecrets()
            form = ""
        }
    }
    onSectionChanged: {
        account.dismissSecrets()
        form = ""
        if (section === "devices" && App.devices) App.devices.reload()
        if (section === "devicekeys" && App.deviceKeys) App.deviceKeys.refresh()
        if (section === "relays" && App.relays) App.relays.load()
    }

    FileDialog {
        id: photoDialog
        title: "Choose a profile photo"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Images (*.jpg *.jpeg *.png *.bmp *.gif)"]
        currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        onAccepted: page.account.uploadAvatar(selectedFile)
    }

    Connections {
        target: App.lock
        function onPinSet() {
            pinField.text = ""
            pinConfirm.text = ""
            page.form = ""
        }
    }

    Connections {
        target: page.account
        function onDone(action) {
            // Never leave a password sitting in a field once it's been used.
            currentPassword.text = ""
            newPassword.text = ""
            confirmPassword.text = ""
            mfaCode.text = ""
            deletePassword.text = ""
            page.lastDone = action
            if (action === "password" || action === "mfaConfirm" || action === "mfaDisable" || action === "handle")
                page.form = ""
        }
    }

    component Section: Text {
        Layout.topMargin: 10
        color: Theme.text2
        font.pixelSize: Theme.fontSmall
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
    }

    component Row2: RowLayout {
        id: rowItem
        property string label
        property string status
        property bool statusGood: true
        default property alias actions: actionArea.data
        Layout.fillWidth: true
        spacing: 10
        ColumnLayout {
            Layout.fillWidth: true
            // Take the space that's left rather than asking for the text's full width.
            Layout.preferredWidth: 0
            spacing: 2
            Text {
                Layout.fillWidth: true
                text: rowItem.label
                textFormat: Text.PlainText
                color: Theme.text
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                text: rowItem.status
                textFormat: Text.PlainText
                color: rowItem.statusGood ? Theme.accent : Theme.warn
                font.pixelSize: Theme.fontSmall + 1
                wrapMode: Text.Wrap
            }
        }
        Row {
            id: actionArea
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            spacing: 4
        }
    }

    component NavItem: ItemDelegate {
        id: navItem
        property string key
        property string iconName
        property string current
        readonly property bool active: current === key
        signal picked(string key)
        Layout.fillWidth: true
        implicitHeight: 42
        leftPadding: 14
        onClicked: picked(key)
        background: Rectangle {
            radius: Theme.radiusControl
            color: navItem.active ? Theme.selected : navItem.hovered ? Qt.alpha(Theme.selected, 0.5) : "transparent"
        }
        contentItem: RowLayout {
            spacing: 12
            Icon {
                name: navItem.iconName
                size: 18
                color: navItem.active ? Theme.accent : Theme.text3
            }
            Text {
                Layout.fillWidth: true
                text: navItem.text
                textFormat: Text.PlainText
                color: navItem.active ? Theme.text : Theme.text2
                font.pixelSize: 14
                font.weight: navItem.active ? Font.DemiBold : Font.Normal
            }
        }
    }

    component NavCaption: Text {
        Layout.leftMargin: 14
        Layout.topMargin: 14
        Layout.bottomMargin: 4
        color: Theme.text3
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // --- Sections ---
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 280
            color: Theme.panel
            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: Theme.border
            }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                anchors.rightMargin: 13
                spacing: 2
                Text {
                    Layout.leftMargin: 8
                    Layout.preferredHeight: Theme.headerHeight - 12
                    verticalAlignment: Text.AlignVCenter
                    text: "Settings"
                    color: Theme.text
                    font.family: Theme.displayFamily
                    font.pixelSize: Theme.fontDisplay
                    font.weight: Font.Bold
                }
                NavCaption { text: "ACCOUNT" }
                NavItem { key: "account"; iconName: "user"; text: "Account and plan"; current: page.section; onPicked: (key) => page.section = key }
                NavItem { key: "privacy"; iconName: "lock"; text: "Privacy"; current: page.section; onPicked: (key) => page.section = key }
                NavItem { key: "stories"; iconName: "stories"; text: "Stories"; current: page.section; onPicked: (key) => page.section = key }
                NavItem { key: "security"; iconName: "key"; text: "Security"; current: page.section; onPicked: (key) => page.section = key }
                NavItem { key: "network"; iconName: "shield"; text: "Network routing"; current: page.section; onPicked: (key) => page.section = key }
                NavItem { key: "notifications"; iconName: "bell"; text: "Notifications"; current: page.section; onPicked: (key) => page.section = key }
                NavItem { key: "calls"; iconName: "phone"; text: "Calls and audio"; current: page.section; onPicked: (key) => page.section = key }
                NavCaption { text: "NETWORK" }
                NavItem { key: "devices"; iconName: "monitor"; text: "Linked devices"; current: page.section; onPicked: (key) => page.section = key }
                NavItem {
                    visible: App.deviceKeys !== null && App.deviceKeys.status !== "off"
                    key: "devicekeys"; iconName: "key"; text: "Devices & encryption"; current: page.section; onPicked: (key) => page.section = key
                }
                NavItem { key: "log"; iconName: "log"; text: "Transparency log"; current: page.section; onPicked: (key) => page.section = key }
                NavItem { key: "relays"; iconName: "globe"; text: "Relay servers"; current: page.section; onPicked: (key) => page.section = key }
                NavCaption { text: "APP" }
                NavItem { key: "about"; iconName: "info"; text: "About"; current: page.section; onPicked: (key) => page.section = key }
                Item { Layout.fillHeight: true }
                RvButton {
                    Layout.fillWidth: true
                    primary: false
                    danger: true
                    iconName: "log-out"
                    text: "Sign out"
                    onClicked: App.signOut()
                }
            }
        }

        // --- The transparency log has a page layout of its own ---
        TransparencyLogView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: page.section === "log"
            log: App.transparencyLog
            active: page.visible && page.section === "log"
        }

        // --- Everything else ---
        Flickable {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: page.section !== "log"
            clip: true
            contentWidth: width
            contentHeight: settingsColumn.implicitHeight + 64
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: settingsColumn
                x: Math.max(32, (scroller.width - 680) / 2)
                y: 32
                width: Math.min(680, scroller.width - 64)
                spacing: 12

                Text {
                    visible: page.section !== "devices" && page.section !== "relays" && page.section !== "devicekeys"
                    Layout.bottomMargin: 4
                    text: ({ account: "Account and plan", privacy: "Privacy", stories: "Stories", security: "Security",
                             notifications: "Notifications", calls: "Calls and audio", about: "About" })[page.section] || ""
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.family: Theme.displayFamily
                    font.pixelSize: 26
                    font.weight: Font.Bold
                }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.section === "account"
                        spacing: 12

                        // --- Profile ---
                        Section { text: "Profile" }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 14
                            Avatar {
                                size: 56
                                initial: App.handle.length > 0 ? App.handle.charAt(0).toUpperCase() : "?"
                                source: page.account.avatarUrl
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 0
                                spacing: 2
                                Text {
                                    text: App.handle
                                    textFormat: Text.PlainText
                                    color: Theme.text
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    text: "Everyone can see your profile photo. It's resized to a small square and its location and camera details are removed before upload."
                                    color: Theme.text2
                                    font.pixelSize: Theme.fontSmall
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            RvButton {
                                primary: false
                                iconName: "image"
                                text: page.account.avatarUrl.length > 0 ? "Change photo" : "Add a photo"
                                enabled: !page.account.busy
                                onClicked: {
                                    App.lock.expectExternalReturn()
                                    photoDialog.open()
                                }
                            }
                            RvButton {
                                primary: false
                                danger: true
                                visible: page.account.avatarUrl.length > 0
                                iconName: "trash-2"
                                text: "Remove"
                                enabled: !page.account.busy
                                onClicked: page.account.removeAvatar()
                            }
                            RvButton {
                                primary: false
                                iconName: "pen"
                                text: "Change handle"
                                visible: page.form !== "handle"
                                enabled: !page.account.busy
                                onClicked: {
                                    page.openForm("handle")
                                    handleField.text = App.handle
                                    handleField.forceActiveFocus()
                                    handleField.selectAll()
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }

                        // Changing the handle: keys and chats stay the same.
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: page.form === "handle"
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: "Your handle is how people find and add you. Changing it doesn't change your keys or your chats, and your safety numbers stay the same."
                                color: Theme.text2
                                font.pixelSize: Theme.fontSmall + 1
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                RvTextField {
                                    id: handleField
                                    Layout.fillWidth: true
                                    maximumLength: 64
                                    placeholderText: "New handle"
                                    font.family: Theme.monoFamily
                                    enabled: !page.account.busy
                                    onAccepted: page.account.changeHandle(text)
                                }
                                RvButton {
                                    text: page.account.busy ? "Saving…" : "Save"
                                    enabled: !page.account.busy && handleField.text.trim().length >= 3
                                             && handleField.text.trim() !== App.handle
                                    onClicked: page.account.changeHandle(handleField.text)
                                }
                                RvButton {
                                    primary: false
                                    text: "Cancel"
                                    onClicked: page.form = ""
                                }
                            }
                        }
                        Banner {
                            Layout.fillWidth: true
                            visible: page.form === "handle" && text.length > 0
                            text: page.account.error
                        }
                        Banner {
                            Layout.fillWidth: true
                            visible: page.form === "" && page.lastDone === "handle" && text.length > 0
                            error: false
                            text: page.account.notice
                        }

                        // --- Plan ---
                        Section { text: "Plan" }

                        // Free, Pro and Premium side by side. Every button makes a real change
                        // on the server (core/model/plan_choices): a Stripe checkout, or a change
                        // to the card subscription; the cards follow once it's made.
                        GridLayout {
                            id: planGrid
                            Layout.fillWidth: true
                            columns: width >= 600 ? 3 : 1
                            columnSpacing: 10
                            rowSpacing: 10

                            Repeater {
                                model: page.account.planChoices
                                delegate: PlanCard {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Layout.preferredWidth: 1
                                    choice: modelData
                                    busy: page.account.busy
                                    onChosen: planConfirm.ask(modelData)
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: page.account.planPriceNote
                            textFormat: Text.PlainText
                            color: Theme.text3
                            font.pixelSize: Theme.fontSmall
                        }
                        Banner {
                            Layout.fillWidth: true
                            visible: page.form === "plan" && text.length > 0
                            text: page.account.error
                        }
                        Banner {
                            Layout.fillWidth: true
                            visible: page.form === "plan" && text.length > 0
                            error: false
                            text: page.account.notice
                        }

                        ConfirmDialog {
                            id: planConfirm
                            property string tier: ""
                            property string action: ""

                            // Asks first when the plan says what will happen (a payment, a switch, Free later).
                            function ask(choice) {
                                planConfirm.tier = choice.tier
                                planConfirm.action = choice.action
                                if (!choice.confirm) {
                                    planConfirm.go()
                                    return
                                }
                                planConfirm.title = choice.actionLabel + "?"
                                planConfirm.message = choice.confirm
                                planConfirm.confirmText = choice.action === "checkout" ? "Continue to payment" : choice.actionLabel
                                planConfirm.danger = choice.action === "cancel"
                                planConfirm.open()
                            }
                            function go() {
                                page.account.clearMessages()
                                page.form = "plan"
                                // The browser opens for a checkout: don't lock the app while it's away.
                                if (planConfirm.action === "checkout") App.lock.expectExternalReturn()
                                page.account.choosePlan(planConfirm.tier)
                            }
                            onConfirmed: planConfirm.go()
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: page.account.pro && page.account.storageText.length > 0
                            spacing: 6
                            Text {
                                text: "File storage"
                                color: Theme.text
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: page.account.storageText
                                textFormat: Text.PlainText
                                color: Theme.text2
                                font.pixelSize: Theme.fontSmall
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 6
                                radius: 3
                                color: Theme.surface2
                                Rectangle {
                                    width: parent.width * page.account.storageFraction
                                    height: parent.height
                                    radius: 3
                                    color: page.account.storageFull ? Theme.danger : Theme.accent
                                }
                            }
                        }

                        // --- Delete account ---
                        Section { text: "Delete account" }
                        RvButton {
                            visible: page.form !== "delete"
                            primary: false
                            danger: true
                            iconName: "trash-2"
                            text: "Delete my account…"
                            onClicked: {
                                page.openForm("delete")
                                deletePassword.text = ""
                                deleteConfirm.checked = false
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: page.form === "delete"
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: "This permanently deletes your account and signs you out on every device. Your keys, recovery key, two-factor setup, profile photo and Files are removed. You can't undo this, and your handle can't be used again.\n\nMessages you already sent stay in other people's chats, still end-to-end encrypted. They'll see your account as deleted."
                                      + (page.account.pro ? "\n\nDeleting your account doesn't stop a card subscription: switch to Free above first." : "")
                                color: Theme.text
                                font.pixelSize: Theme.fontSmall + 1
                            }
                            RvTextField {
                                id: deletePassword
                                Layout.fillWidth: true
                                placeholderText: "Your password"
                                echoMode: TextInput.Password
                                enabled: !page.account.busy
                            }
                            Banner {
                                Layout.fillWidth: true
                                visible: text.length > 0
                                text: page.account.error
                            }
                            CheckBox {
                                id: deleteConfirm
                                text: "I understand this can't be undone"
                                palette.windowText: Theme.text
                            }
                            RowLayout {
                                RvButton {
                                    danger: true
                                    text: page.account.busy ? "Deleting…" : "Delete my account"
                                    enabled: !page.account.busy && deletePassword.text.length > 0 && deleteConfirm.checked
                                    onClicked: page.account.deleteAccount(deletePassword.text)
                                }
                                RvButton {
                                    primary: false
                                    text: "Cancel"
                                    enabled: !page.account.busy
                                    onClicked: {
                                        deletePassword.text = ""
                                        page.form = ""
                                    }
                                }
                            }
                        }

                    }
                    Loader {
                        Layout.fillWidth: true
                        visible: page.section === "stories"
                        active: page.section === "stories" && App.stories !== null
                        sourceComponent: StorySettingsSection { stories: App.stories }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.section === "privacy"
                        spacing: 12

                        // --- Privacy ---
                        Section { text: "Privacy" }

                        RvSwitch {
                            Layout.fillWidth: true
                            label: "Read receipts"
                            description: "Let people see when you've read their messages. Off means they only see that it was delivered. Saved on this computer."
                            checked: page.account.readReceipts
                            onToggled: (on) => page.account.setReadReceipts(on)
                        }

                        RvSwitch {
                            Layout.fillWidth: true
                            label: "Accept friend requests"
                            description: "When off, nobody can send you a new friend request."
                            checked: page.account.acceptMessageRequests
                            enabled: page.account.loaded && !page.account.busy
                            onToggled: (on) => page.account.setAcceptMessageRequests(on)
                        }

                        RvSwitch {
                            Layout.fillWidth: true
                            label: "Show when I'm online"
                            description: "Your contacts see a dot when you're online, and you see theirs. Off means neither: it works both ways."
                            checked: page.account.showOnlineStatus
                            enabled: page.account.loaded && !page.account.busy
                            onToggled: (on) => page.account.setShowOnlineStatus(on)
                        }
                        RvSwitch {
                            Layout.fillWidth: true
                            label: "Transparency mode"
                            description: "When on, your outgoing messages' encrypted ciphertext (never the readable content) appears in the public live transparency feed. Off by default: this is entirely your choice."
                            checked: page.account.transparencyMode
                            enabled: page.account.loaded && !page.account.busy
                            onToggled: (on) => page.account.setTransparencyMode(on)
                        }
                        RvSwitch {
                            Layout.fillWidth: true
                            label: "Typing indicators"
                            description: "Show when you're typing, and see when others are. Off means neither."
                            checked: App.live !== null && App.live.typingIndicators
                            onToggled: (on) => { if (App.live) App.live.typingIndicators = on }
                        }

                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.section === "security"
                        spacing: 12

                        // --- Security ---
                        Section { text: "Security" }

                        Banner {
                            Layout.fillWidth: true
                            error: false
                            // Plan changes show their notice under the plan instead.
                            text: page.form === "plan" ? "" : page.account.notice
                        }

                        // Password
                        Row2 {
                            label: "Password"
                            status: "Changing it keeps your messages: your key is re-encrypted on this computer."
                            statusGood: true
                            RvButton {
                                primary: false
                                text: "Change"
                                visible: page.form !== "password"
                                onClicked: page.openForm("password")
                            }
                        }

                        // Recovery key
                        Row2 {
                            label: "Recovery key"
                            status: !page.account.loaded ? "…" : page.account.hasRecoveryKey ? "Set up" : "Not set up: you can't recover this account if you forget your password"
                            statusGood: page.account.hasRecoveryKey
                            RvButton {
                                primary: false
                                text: page.account.hasRecoveryKey ? "Replace" : "Create"
                                visible: page.form !== "recovery"
                                onClicked: page.openForm("recovery")
                            }
                        }

                        // Two-factor
                        Row2 {
                            label: "Two-factor authentication"
                            status: !page.account.loaded ? "…" : page.account.mfaEnabled ? "On" : "Off"
                            statusGood: page.account.mfaEnabled
                            RvButton {
                                primary: false
                                danger: page.account.mfaEnabled
                                text: page.account.mfaEnabled ? "Turn off" : "Turn on"
                                visible: page.account.loaded && page.form !== "mfaOn" && page.form !== "mfaOff"
                                onClicked: page.openForm(page.account.mfaEnabled ? "mfaOff" : "mfaOn")
                            }
                        }

                        // --- The open form ---
                        Rectangle {
                            Layout.fillWidth: true
                            visible: page.form.length > 0
                            implicitHeight: formColumn.implicitHeight + 32
                            radius: Theme.radiusCard
                            color: Theme.bg
                            border.color: Theme.border

                            ColumnLayout {
                                id: formColumn
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 10

                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    font.weight: Font.DemiBold
                                    color: Theme.text
                                    text: page.form === "password" ? "Change your password"
                                        : page.form === "recovery" ? (page.account.newRecoveryKey.length > 0 ? "Your new recovery key" : "Create a new recovery key")
                                        : page.form === "mfaOn" ? "Turn on two-factor authentication"
                                        : "Turn off two-factor authentication"
                                    textFormat: Text.PlainText
                                }

                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    visible: text.length > 0
                                    color: Theme.text2
                                    font.pixelSize: Theme.fontSmall + 1
                                    text: page.form === "password" ? "Your other devices will be signed out."
                                        : page.form === "recovery" && page.account.newRecoveryKey.length === 0
                                          ? "This replaces any recovery key you had before, which stops working." : ""
                                    textFormat: Text.PlainText
                                }

                                // Current password (password change, new recovery key, turning two-factor off)
                                RvTextField {
                                    id: currentPassword
                                    Layout.fillWidth: true
                                    visible: page.form === "password" || page.form === "mfaOff"
                                             || (page.form === "recovery" && page.account.newRecoveryKey.length === 0)
                                    placeholderText: page.form === "password" ? "Current password" : "Your password"
                                    echoMode: TextInput.Password
                                    enabled: !page.account.busy
                                }
                                RvTextField {
                                    id: newPassword
                                    Layout.fillWidth: true
                                    visible: page.form === "password"
                                    placeholderText: "New password (at least 8 characters)"
                                    echoMode: TextInput.Password
                                    enabled: !page.account.busy
                                }
                                RvTextField {
                                    id: confirmPassword
                                    Layout.fillWidth: true
                                    visible: page.form === "password"
                                    placeholderText: "Repeat the new password"
                                    echoMode: TextInput.Password
                                    enabled: !page.account.busy
                                }

                                // Two-factor setup: the key to type into an authenticator app, then a code from it.
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    visible: page.form === "mfaOn"
                                    spacing: 8
                                    Text {
                                        Layout.fillWidth: true
                                        wrapMode: Text.Wrap
                                        text: "Scan this with your authenticator app (Google Authenticator, Authy, 1Password, …), or enter the setup key below by hand. Then type the 6-digit code it shows."
                                        color: Theme.text2
                                        font.pixelSize: Theme.fontSmall + 1
                                    }
                                    Rectangle {
                                        Layout.alignment: Qt.AlignHCenter
                                        visible: page.account.mfaUri.length > 0
                                        implicitWidth: 200
                                        implicitHeight: 200
                                        radius: Theme.radiusControl
                                        color: "white"
                                        Image {
                                            anchors.centerIn: parent
                                            width: 188
                                            height: 188
                                            cache: false
                                            smooth: false
                                            sourceSize: Qt.size(188, 188)
                                            source: page.account.mfaUri.length > 0 ? "image://qr/" + encodeURIComponent(page.account.mfaUri) : ""
                                        }
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        implicitHeight: 48
                                        radius: Theme.radiusControl
                                        color: Theme.surface
                                        border.color: Theme.border
                                        TextEdit {
                                            anchors.centerIn: parent
                                            readOnly: true
                                            selectByMouse: true
                                            textFormat: TextEdit.PlainText
                                            text: page.account.mfaSecret.length > 0 ? page.account.mfaSecret : "…"
                                            color: Theme.text
                                            font.family: "Consolas"
                                            font.pixelSize: 17
                                        }
                                    }
                                    RvButton {
                                        primary: false
                                        iconName: "copy"
                                        text: "Copy setup key"
                                        enabled: page.account.mfaSecret.length > 0
                                        onClicked: App.copySecret(page.account.mfaSecret.replace(/ /g, ""))
                                    }
                                    RvTextField {
                                        id: mfaCode
                                        Layout.fillWidth: true
                                        placeholderText: "6-digit code"
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        maximumLength: 10
                                        enabled: !page.account.busy && page.account.mfaSecret.length > 0
                                        onAccepted: page.account.confirmMfa(text)
                                    }
                                }

                                // A freshly created recovery key, shown once.
                                RecoveryKeyPanel {
                                    Layout.fillWidth: true
                                    visible: page.form === "recovery" && page.account.newRecoveryKey.length > 0
                                    recoveryKey: page.account.newRecoveryKey
                                    onFinished: {
                                        page.account.dismissSecrets()
                                        page.form = ""
                                    }
                                }

                                Banner {
                                    Layout.fillWidth: true
                                    text: page.account.error
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    visible: !(page.form === "recovery" && page.account.newRecoveryKey.length > 0)
                                    Item { Layout.fillWidth: true }
                                    RvButton {
                                        primary: false
                                        text: "Cancel"
                                        onClicked: page.openForm("")
                                    }
                                    RvButton {
                                        enabled: !page.account.busy
                                        danger: page.form === "mfaOff"
                                        text: page.account.busy ? "Working…"
                                            : page.form === "password" ? "Change password"
                                            : page.form === "recovery" ? "Create recovery key"
                                            : page.form === "mfaOn" ? "Turn on"
                                            : "Turn off"
                                        onClicked: {
                                            if (page.form === "password")
                                                page.account.changePassword(currentPassword.text, newPassword.text, confirmPassword.text)
                                            else if (page.form === "recovery")
                                                page.account.createRecoveryKey(currentPassword.text)
                                            else if (page.form === "mfaOn")
                                                page.account.confirmMfa(mfaCode.text)
                                            else
                                                page.account.disableMfa(currentPassword.text)
                                        }
                                    }
                                }
                            }
                        }

                        // --- App lock ---
                        Section { text: "App lock" }

                        Row2 {
                            label: "PIN lock"
                            status: App.lock.enabled ? "On: " + App.lock.timeoutLabels[App.lock.timeoutIndex].toLowerCase() + " away" : "Off"
                            statusGood: App.lock.enabled
                            RvButton {
                                primary: false
                                visible: page.form !== "pin"
                                text: App.lock.enabled ? "Change PIN" : "Set a PIN"
                                onClicked: {
                                    App.lock.clearError()
                                    pinField.text = ""
                                    pinConfirm.text = ""
                                    page.form = "pin"
                                }
                            }
                            RvButton {
                                primary: false
                                danger: true
                                visible: App.lock.enabled && page.form !== "pin"
                                text: "Turn off"
                                onClicked: App.lock.disable()
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: App.lock.enabled
                            Text {
                                Layout.fillWidth: true
                                text: "Lock when I've been away"
                                color: Theme.text
                            }
                            ComboBox {
                                id: timeoutBox
                                model: App.lock.timeoutLabels
                                currentIndex: App.lock.timeoutIndex
                                onActivated: (index) => App.lock.timeoutIndex = index
                                implicitWidth: 190
                                implicitHeight: 36
                                background: Rectangle {
                                    radius: Theme.radiusControl
                                    color: Theme.surface
                                    border.color: timeoutBox.activeFocus ? Theme.accent : Theme.border
                                }
                                contentItem: Text {
                                    textFormat: Text.PlainText
                                    leftPadding: 12
                                    text: timeoutBox.displayText
                                    color: Theme.text
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            visible: page.form === "pin"
                            implicitHeight: pinColumn.implicitHeight + 32
                            radius: Theme.radiusCard
                            color: Theme.bg
                            border.color: Theme.border

                            ColumnLayout {
                                id: pinColumn
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 10

                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    text: "Choose a 4 to 12 digit PIN. It's stored on this computer only as a slow, salted hash. Ten wrong tries sign you out. Press Ctrl+L to lock at any time."
                                    color: Theme.text2
                                    font.pixelSize: Theme.fontSmall + 1
                                }
                                RvTextField {
                                    id: pinField
                                    Layout.fillWidth: true
                                    placeholderText: "New PIN"
                                    echoMode: TextInput.Password
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    validator: RegularExpressionValidator { regularExpression: /[0-9]{0,12}/ }
                                    enabled: !App.lock.busy
                                }
                                RvTextField {
                                    id: pinConfirm
                                    Layout.fillWidth: true
                                    placeholderText: "Repeat the PIN"
                                    echoMode: TextInput.Password
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    validator: RegularExpressionValidator { regularExpression: /[0-9]{0,12}/ }
                                    enabled: !App.lock.busy
                                    onAccepted: App.lock.setPin(pinField.text, pinConfirm.text)
                                }
                                Banner {
                                    Layout.fillWidth: true
                                    text: App.lock.error
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Item { Layout.fillWidth: true }
                                    RvButton {
                                        primary: false
                                        text: "Cancel"
                                        onClicked: page.form = ""
                                    }
                                    RvButton {
                                        text: App.lock.busy ? "Saving…" : "Save PIN"
                                        enabled: !App.lock.busy
                                        onClicked: App.lock.setPin(pinField.text, pinConfirm.text)
                                    }
                                }
                            }
                        }


                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.section === "notifications"
                        spacing: 12

                        // --- This computer ---
                        Section { text: "This computer" }

                        RvSwitch {
                            Layout.fillWidth: true
                            label: "Notifications"
                            description: "Tell me about new messages while redvaultmessages is in the background. Never includes what the message says."
                            checked: Shell.notifications
                            onToggled: (on) => Shell.notifications = on
                        }
                        RvSwitch {
                            Layout.fillWidth: true
                            visible: Shell.notifications
                            label: "Show who it's from"
                            description: "Windows keeps notifications in its history, so this is off by default. Never shown while locked."
                            checked: Shell.notificationShowsSender
                            onToggled: (on) => Shell.notificationShowsSender = on
                        }
                        RvSwitch {
                            Layout.fillWidth: true
                            visible: Shell.trayAvailable
                            label: "Keep running in the tray"
                            description: "Closing the window leaves redvaultmessages running (and signed in) so notifications keep coming. Quit from the tray icon to sign out."
                            checked: Shell.closeToTray
                            onToggled: (on) => Shell.closeToTray = on
                        }
                        RvSwitch {
                            Layout.fillWidth: true
                            // Windows only: Linux has no way for a window to opt out of capture.
                            visible: Qt.platform.os === "windows"
                            label: "Hide from screenshots and screen sharing"
                            description: "Screenshots, recordings and apps that share your screen see an empty window instead of your chats."
                            checked: Shell.captureProtection
                            onToggled: (on) => Shell.captureProtection = on
                        }

                        RvSwitch {
                            Layout.fillWidth: true
                            label: "Ringtone"
                            description: "Ring for incoming calls. Muted contacts never ring."
                            checked: App.calls !== null && App.calls.ringtone
                            onToggled: (on) => { if (App.calls) App.calls.ringtone = on }
                        }

                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.section === "calls"
                        spacing: 12
                        Section { text: "Calls and audio" }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: "Calls are one-to-one and encrypted end to end. They always go through a relay, so nobody you call learns your IP address."
                            color: Theme.text2
                            font.pixelSize: Theme.fontSmall + 1
                        }
                        Text {
                            text: "Microphone"
                            color: Theme.text
                            font.weight: Font.DemiBold
                        }
                        DevicePicker {
                            Layout.fillWidth: true
                            iconName: "mic"
                            defaultLabel: "System default microphone"
                            devices: App.calls ? App.calls.microphones : []
                            currentId: App.calls ? App.calls.microphoneId : ""
                            onPicked: (id) => App.calls.setMicrophone(id)
                        }
                        Text {
                            text: "Speaker"
                            color: Theme.text
                            font.weight: Font.DemiBold
                        }
                        DevicePicker {
                            Layout.fillWidth: true
                            iconName: "speaker"
                            defaultLabel: "System default speaker"
                            devices: App.calls ? App.calls.speakers : []
                            currentId: App.calls ? App.calls.speakerId : ""
                            onPicked: (id) => App.calls.setSpeaker(id)
                        }
                        Text {
                            text: "Camera"
                            color: Theme.text
                            font.weight: Font.DemiBold
                        }
                        DevicePicker {
                            Layout.fillWidth: true
                            iconName: "video"
                            defaultLabel: "System default camera"
                            devices: App.calls ? App.calls.cameras : []
                            currentId: App.calls ? App.calls.cameraId : ""
                            onPicked: (id) => App.calls.setCamera(id)
                        }
                        Row2 {
                            label: "Echo cancellation"
                            status: App.calls && App.calls.echoCancellation ? "On: echo, noise and volume are handled on this computer"
                                                                             : "Not in this build: use headphones to avoid echo"
                            statusGood: App.calls !== null && App.calls.echoCancellation
                        }
                        Row2 {
                            label: "Your plan"
                            status: page.account.premium ? "Premium: one-to-one calls, voice and video call links, screen sharing"
                                  : page.account.pro ? "Pro: one-to-one calls and voice call links (video links and screen sharing are in Premium)"
                                  : "Free: one-to-one voice and video calls. Call links need Pro; screen sharing needs Premium."
                            statusGood: true
                        }
                    }
                    LinkedDevicesSection {
                        Layout.fillWidth: true
                        visible: page.section === "devices"
                        devices: App.devices
                    }
                    DeviceKeysSection {
                        Layout.fillWidth: true
                        visible: page.section === "devicekeys"
                        keys: App.deviceKeys
                    }
                    RelayServersSection {
                        Layout.fillWidth: true
                        visible: page.section === "relays"
                        relays: App.relays
                    }
                    NetworkRoutingSection {
                        Layout.fillWidth: true
                        visible: page.section === "network"
                        tor: App.tor
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.section === "about"
                        spacing: 12

                        // --- About ---
                        Section { text: "About" }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 0
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: "redvaultmessages " + Updater.currentVersion
                                    textFormat: Text.PlainText
                                    color: Theme.text
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    text: Updater.fromStore ? "Updates come from the Microsoft Store."
                                        : Updater.blockedByTor ? "This app checks for updates only through Tor, and its server address isn't Redvault's onion service."
                                        : !Updater.enabled && Qt.platform.os !== "windows" ? "To update, download the newest version and replace this one."
                                        : !Updater.enabled ? "Automatic updates aren't set up in this build."
                                        : Updater.status === Updater.Checking ? "Checking for updates…"
                                        : Updater.status === Updater.UpToDate ? "You're up to date."
                                        : Updater.status === Updater.Available ? "Version " + Updater.availableVersion + " is available (see the top of the window)."
                                        : Updater.status === Updater.Downloading ? "Downloading the update…"
                                        : Updater.status === Updater.Installing ? "Installing the update…"
                                        : Updater.status === Updater.Failed ? "Couldn't check for updates: " + Updater.error
                                        : "Updates are checked automatically and verified before they're installed."
                                    textFormat: Text.PlainText
                                    color: Updater.status === Updater.Failed ? Theme.warn : Theme.text2
                                    font.pixelSize: Theme.fontSmall + 1
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    wrapMode: Text.Wrap
                                    text: Updater.freshnessWarning
                                    textFormat: Text.PlainText
                                    color: Theme.warn
                                    font.pixelSize: Theme.fontSmall + 1
                                }
                            }
                            RvButton {
                                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                visible: Updater.enabled
                                primary: false
                                iconName: "download"
                                text: "Check for updates"
                                enabled: Updater.status !== Updater.Checking && Updater.status !== Updater.Downloading
                                         && Updater.status !== Updater.Installing
                                onClicked: Updater.check()
                            }
                        }

                        OpenSourceSection {
                            Layout.fillWidth: true
                        }


                    }

            }
        }
    }
}
