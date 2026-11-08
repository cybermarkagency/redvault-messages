// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Add or edit a password-manager item. Only the fields its type has are
// shown; everything is sealed on this computer when it's saved.
RvDialog {
    id: dialog
    required property VaultSession vault
    property var fields: ({})
    readonly property string type: fields.type || "login"
    readonly property bool editing: (fields.id || "").length > 0
    property bool showPassword: false

    title: (editing ? "Edit " : "New ") + ({ login: "login", passkey: "passkey", card: "payment card", note: "secure note", wifi: "Wi-Fi network" })[type]
    width: 520

    function openWith(values) {
        fields = values
        showPassword = false
        title_.text = values.title || ""
        username.text = values.username || ""
        password.text = values.password || ""
        url.text = values.url || ""
        notes.text = values.notes || ""
        totp.text = values.totp || ""
        cardHolder.text = values.cardHolder || ""
        cardNumber.text = values.cardNumber || ""
        cardExpiry.text = values.cardExpiry || ""
        cardCvv.text = values.cardCvv || ""
        wifiSsid.text = values.wifiSsid || ""
        wifiSecurity.text = values.wifiSecurity || "WPA3"
        wifiPassword.text = values.wifiPassword || values.password || ""
        passkeyRpId.text = values.passkeyRpId || ""
        passkeyUser.text = values.passkeyUser || ""
        open()
        title_.forceActiveFocus()
    }

    component Label2: Text {
        color: Theme.text3
        font.pixelSize: 12
    }

    Connections {
        target: dialog.vault
        function onSaved() { dialog.close() }
    }

    contentItem: Flickable {
        implicitHeight: Math.min(form.implicitHeight, 560)
        contentHeight: form.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ColumnLayout {
            id: form
            width: parent.width
            spacing: 8

            Label2 { text: "Name" }
            RvTextField { id: title_; Layout.fillWidth: true; placeholderText: dialog.type === "wifi" ? "Home Wi-Fi" : "Work email" }

            // Login and passkey
            Label2 { visible: dialog.type === "login" || dialog.type === "passkey"; text: "Username or email" }
            RvTextField {
                id: username
                visible: dialog.type === "login" || dialog.type === "passkey"
                Layout.fillWidth: true
                font.family: Theme.monoFamily
            }
            Label2 { visible: dialog.type === "login"; text: "Password" }
            RowLayout {
                visible: dialog.type === "login"
                Layout.fillWidth: true
                RvTextField {
                    id: password
                    Layout.fillWidth: true
                    echoMode: dialog.showPassword ? TextInput.Normal : TextInput.Password
                    font.family: Theme.monoFamily
                }
                IconButton { iconName: dialog.showPassword ? "eye-off" : "eye"; tip: dialog.showPassword ? "Hide password" : "Show password"; toggled: dialog.showPassword; onClicked: dialog.showPassword = !dialog.showPassword }
                IconButton { iconName: "refresh"; tip: "Use a generated password"; onClicked: { dialog.vault.regenerate(); password.text = dialog.vault.generated } }
            }
            StrengthBar {
                visible: dialog.type === "login" && password.text.length > 0
                Layout.fillWidth: true
                strength: dialog.vault.strengthOf(password.text)
            }
            Label2 { visible: dialog.type === "login" || dialog.type === "passkey"; text: "Website" }
            RvTextField {
                id: url
                visible: dialog.type === "login" || dialog.type === "passkey"
                Layout.fillWidth: true
                placeholderText: "example.com"
                font.family: Theme.monoFamily
            }
            Label2 { visible: dialog.type === "login"; text: "One-time code setup key (optional)" }
            RvTextField {
                id: totp
                visible: dialog.type === "login"
                Layout.fillWidth: true
                placeholderText: "The key or otpauth:// link the site shows"
                font.family: Theme.monoFamily
            }
            Label2 { visible: dialog.type === "passkey"; text: "Site (relying party)" }
            RvTextField { id: passkeyRpId; visible: dialog.type === "passkey"; Layout.fillWidth: true; placeholderText: "example.com"; font.family: Theme.monoFamily }
            Label2 { visible: dialog.type === "passkey"; text: "Account name on the passkey" }
            RvTextField { id: passkeyUser; visible: dialog.type === "passkey"; Layout.fillWidth: true; font.family: Theme.monoFamily }

            // Card
            Label2 { visible: dialog.type === "card"; text: "Name on card" }
            RvTextField { id: cardHolder; visible: dialog.type === "card"; Layout.fillWidth: true }
            Label2 { visible: dialog.type === "card"; text: "Card number" }
            RvTextField {
                id: cardNumber
                visible: dialog.type === "card"
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhDigitsOnly
                echoMode: dialog.showPassword ? TextInput.Normal : TextInput.Password
                font.family: Theme.monoFamily
            }
            RowLayout {
                visible: dialog.type === "card"
                Layout.fillWidth: true
                ColumnLayout {
                    Label2 { text: "Expires (MM/YY)" }
                    RvTextField { id: cardExpiry; Layout.fillWidth: true; font.family: Theme.monoFamily; maximumLength: 5 }
                }
                ColumnLayout {
                    Label2 { text: "Security code" }
                    RvTextField {
                        id: cardCvv
                        Layout.fillWidth: true
                        font.family: Theme.monoFamily
                        maximumLength: 4
                        echoMode: dialog.showPassword ? TextInput.Normal : TextInput.Password
                    }
                }
                IconButton { Layout.alignment: Qt.AlignBottom; iconName: dialog.showPassword ? "eye-off" : "eye"; tip: "Show or hide"; onClicked: dialog.showPassword = !dialog.showPassword }
            }

            // Wi-Fi
            Label2 { visible: dialog.type === "wifi"; text: "Network name (SSID)" }
            RvTextField { id: wifiSsid; visible: dialog.type === "wifi"; Layout.fillWidth: true; font.family: Theme.monoFamily }
            Label2 { visible: dialog.type === "wifi"; text: "Security" }
            RvTextField { id: wifiSecurity; visible: dialog.type === "wifi"; Layout.fillWidth: true; placeholderText: "WPA3" }
            Label2 { visible: dialog.type === "wifi"; text: "Password" }
            RowLayout {
                visible: dialog.type === "wifi"
                Layout.fillWidth: true
                RvTextField {
                    id: wifiPassword
                    Layout.fillWidth: true
                    echoMode: dialog.showPassword ? TextInput.Normal : TextInput.Password
                    font.family: Theme.monoFamily
                }
                IconButton { iconName: dialog.showPassword ? "eye-off" : "eye"; tip: "Show or hide"; onClicked: dialog.showPassword = !dialog.showPassword }
            }

            Label2 { text: dialog.type === "note" ? "Note" : "Notes" }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: dialog.type === "note" ? 160 : 80
                TextArea {
                    id: notes
                    wrapMode: TextArea.Wrap
                    color: Theme.text
                    font.pixelSize: 14
                    background: Rectangle { radius: Theme.radiusSmall; color: Theme.surface2; border.color: notes.activeFocus ? Theme.accentBorder : Theme.surface2 }
                }
            }

            Banner { Layout.fillWidth: true; text: dialog.vault.error }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                RvButton {
                    visible: dialog.editing
                    primary: false
                    danger: true
                    iconName: "trash"
                    text: "Delete"
                    onClicked: {
                        dialog.vault.deleteItem(dialog.fields.id)
                        dialog.close()
                    }
                }
                Item { Layout.fillWidth: true }
                RvButton { primary: false; text: "Cancel"; onClicked: dialog.close() }
                RvButton {
                    text: dialog.vault.busy ? "Saving…" : "Save"
                    enabled: !dialog.vault.busy && title_.text.trim().length > 0
                    onClicked: dialog.vault.saveItem({
                        id: dialog.fields.id || "",
                        type: dialog.type,
                        title: title_.text,
                        username: username.text,
                        password: dialog.type === "wifi" ? wifiPassword.text : password.text,
                        url: url.text,
                        notes: notes.text,
                        totp: totp.text,
                        cardHolder: cardHolder.text,
                        cardNumber: cardNumber.text,
                        cardExpiry: cardExpiry.text,
                        cardCvv: cardCvv.text,
                        wifiSsid: wifiSsid.text,
                        wifiSecurity: wifiSecurity.text,
                        wifiPassword: wifiPassword.text,
                        passkeyRpId: passkeyRpId.text,
                        passkeyUser: passkeyUser.text
                    })
                }
            }
        }
    }
    onClosed: {
        // Nothing sensitive lingers in the form.
        password.text = ""
        wifiPassword.text = ""
        cardNumber.text = ""
        cardCvv.text = ""
        showPassword = false
    }
}
