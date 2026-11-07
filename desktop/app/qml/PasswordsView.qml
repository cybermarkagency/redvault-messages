pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Passwords: the password manager. Category sidebar (240px), item list
// (380px) and the item. Locked, the list and item are replaced by the lock
// screen and nothing from the vault is loaded at all.
Item {
    id: root
    required property VaultSession vault
    required property ChatSession chats
    property bool revealed: false
    property string revealedText: ""
    property bool passwordUnlock: false

    // Passwords are hidden again when you switch items or the vault locks.
    function hide() {
        revealed = false
        revealedText = ""
    }
    Connections {
        target: root.vault
        function onSelectedChanged() { root.hide() }
        function onLockedChanged() {
            root.hide()
            root.passwordUnlock = false
        }
    }
    onVisibleChanged: {
        if (!visible) hide()
        else if (!vault.locked) vault.refresh()
    }

    VaultItemDialog {
        id: editor
        vault: root.vault
    }

    ContactPickerDialog {
        id: sharePicker
        title: "Share securely with"
        chats: root.chats
        onPicked: (userId) => root.vault.share(root.vault.selectedId, userId)
    }

    Menu {
        id: newMenu
        MenuItem { text: "Login"; onTriggered: editor.openWith(root.vault.blankItem("login")) }
        MenuItem { text: "Passkey"; onTriggered: editor.openWith(root.vault.blankItem("passkey")) }
        MenuItem { text: "Payment card"; onTriggered: editor.openWith(root.vault.blankItem("card")) }
        MenuItem { text: "Secure note"; onTriggered: editor.openWith(root.vault.blankItem("note")) }
        MenuItem { text: "Wi-Fi network"; onTriggered: editor.openWith(root.vault.blankItem("wifi")) }
    }

    component Category: ItemDelegate {
        id: cat
        property string key
        property string iconName
        property color ink: Theme.text
        property int count: 0
        property string current
        signal picked(string key)
        Layout.fillWidth: true
        implicitHeight: 40
        leftPadding: 12
        rightPadding: 12
        onClicked: picked(key)
        background: Rectangle {
            radius: Theme.radiusControl
            color: cat.current === cat.key ? Theme.selected : cat.hovered ? Qt.alpha(Theme.selected, 0.5) : "transparent"
        }
        contentItem: RowLayout {
            spacing: 10
            Icon { visible: cat.iconName.length > 0; name: cat.iconName; size: 16; color: cat.ink }
            Text {
                Layout.fillWidth: true
                text: cat.text
                textFormat: Text.PlainText
                color: cat.current === cat.key ? Theme.accent : cat.ink
                font.pixelSize: 14
                font.weight: cat.current === cat.key ? Font.DemiBold : Font.Normal
            }
            Text {
                text: cat.count
                textFormat: Text.PlainText
                color: cat.current === cat.key ? Theme.accent : Theme.text3
                font.family: Theme.monoFamily
                font.pixelSize: 12
            }
        }
    }

    component Badge: Rectangle {
        id: badge
        property string status
        visible: status.length > 0
        implicitWidth: badgeText.implicitWidth + 16
        implicitHeight: 22
        radius: 11
        color: badge.status === "exposed" ? Theme.dangerSoft : "#33291A"
        Text {
            id: badgeText
            anchors.centerIn: parent
            text: badge.status === "exposed" ? "Exposed" : badge.status === "reused" ? "Reused" : "Weak"
            textFormat: Text.PlainText
            color: badge.status === "exposed" ? Theme.danger : Theme.warn
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
    }

    component FieldRow: Item {
        id: field
        property string label
        property string value
        property bool mono: true
        default property alias actions: actionRow.data
        Layout.fillWidth: true
        implicitHeight: 64
        ColumnLayout {
            anchors.left: parent.left
            anchors.right: actionRow.left
            anchors.leftMargin: 16
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3
            Text { textFormat: Text.PlainText; text: field.label; color: Theme.text3; font.pixelSize: 12 }
            Text {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: field.value
                textFormat: Text.PlainText
                color: Theme.text
                font.family: field.mono ? Theme.monoFamily : Theme.fontFamily
                font.pixelSize: 14
            }
        }
        Row {
            id: actionRow
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // --- Categories ---
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 240
            color: Theme.panel
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                anchors.rightMargin: 13
                spacing: 2
                Text {
                    Layout.leftMargin: 8
                    Layout.preferredHeight: Theme.headerHeight - 12
                    verticalAlignment: Text.AlignVCenter
                    text: "Passwords"
                    color: Theme.text
                    font.family: Theme.displayFamily
                    font.pixelSize: 28
                    font.weight: Font.Bold
                }

                // Lock status
                Rectangle {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 10
                    implicitHeight: 70
                    radius: Theme.radiusCard
                    color: root.vault.locked ? Theme.surface : Theme.accentSoft
                    border.color: root.vault.locked ? Theme.borderStrong : Theme.accentBorder
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 10
                        Icon { name: root.vault.locked ? "lock" : "lock-open"; size: 18; color: root.vault.locked ? Theme.text2 : Theme.accent }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                text: root.vault.locked ? "Locked" : "Unlocked"
                                textFormat: Text.PlainText
                                color: root.vault.locked ? Theme.text : Theme.accent
                                font.weight: Font.DemiBold
                                font.pixelSize: 13
                            }
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: root.vault.locked ? "Unlock to view items" : "Locks after " + root.vault.lockAfterMinutes + " min idle"
                                textFormat: Text.PlainText
                                color: Theme.text2
                                font.pixelSize: 12
                            }
                        }
                        RvButton {
                            secondary: true
                            implicitHeight: 34
                            leftPadding: 10
                            rightPadding: 10
                            text: root.vault.locked ? "Unlock" : "Lock now"
                            onClicked: root.vault.locked ? (root.vault.helloAvailable ? root.vault.unlockWithHello(Window.window) : (root.passwordUnlock = true))
                                                         : root.vault.lockNow()
                        }
                    }
                }

                Category { key: "all"; text: "All items"; count: root.vault.counts.all || 0; current: root.vault.category; onPicked: (k) => root.vault.category = k }
                Category { key: "login"; text: "Logins"; count: root.vault.counts.login || 0; current: root.vault.category; onPicked: (k) => root.vault.category = k }
                Category { key: "passkey"; text: "Passkeys"; count: root.vault.counts.passkey || 0; current: root.vault.category; onPicked: (k) => root.vault.category = k }
                Category { key: "card"; text: "Payment cards"; count: root.vault.counts.card || 0; current: root.vault.category; onPicked: (k) => root.vault.category = k }
                Category { key: "note"; text: "Secure notes"; count: root.vault.counts.note || 0; current: root.vault.category; onPicked: (k) => root.vault.category = k }
                Category { key: "wifi"; text: "Wi-Fi"; count: root.vault.counts.wifi || 0; current: root.vault.category; onPicked: (k) => root.vault.category = k }
                Category { key: "shared"; text: "Shared with me"; count: root.vault.counts.shared || 0; current: root.vault.category; onPicked: (k) => root.vault.category = k }

                Text {
                    Layout.leftMargin: 12
                    Layout.topMargin: 14
                    Layout.bottomMargin: 4
                    text: "SECURITY CHECK"
                    color: Theme.text3
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.8
                }
                Category { key: "exposed"; iconName: "triangle-alert"; ink: Theme.danger; text: "Exposed in a breach"; count: root.vault.counts.exposed || 0; current: root.vault.category; onPicked: (k) => root.vault.category = k }
                Category { key: "reused"; iconName: "repeat"; ink: Theme.warn; text: "Reused"; count: root.vault.counts.reused || 0; current: root.vault.category; onPicked: (k) => root.vault.category = k }
                Category { key: "weak"; iconName: "alert-circle"; ink: Theme.warn; text: "Weak"; count: root.vault.counts.weak || 0; current: root.vault.category; onPicked: (k) => root.vault.category = k }

                Item { Layout.fillHeight: true }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: syncColumn.implicitHeight + 28
                    radius: Theme.radiusCard
                    color: Theme.accentSoft
                    border.color: Theme.accentBorder
                    ColumnLayout {
                        id: syncColumn
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 4
                        Text { text: "Unlimited items and devices"; color: Theme.accent; font.weight: Font.DemiBold }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            readonly property int devices: App.devices ? App.devices.devices.length : 0
                            text: (devices > 0 ? "Synced on " + devices + (devices === 1 ? " device. " : " devices. ") : "Synced across your devices. ")
                                  + "Your vault is encrypted with a key only you hold."
                            textFormat: Text.PlainText
                            color: Theme.text2
                            font.pixelSize: 12
                        }
                    }
                }
            }
        }

        // --- Locked ---
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.vault.locked
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(420, parent.width - 48)
                spacing: 14
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: 88
                    implicitHeight: 88
                    radius: 44
                    color: Theme.surface
                    border.color: Theme.borderStrong
                    Icon { anchors.centerIn: parent; name: "lock"; size: 34; color: Theme.accent }
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Your vault is locked"
                    color: Theme.text
                    font.family: Theme.displayFamily
                    font.pixelSize: 30
                    font.weight: Font.Bold
                }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: "Unlock to see your passwords. Nothing in the vault can be read until you do — not even by us."
                    color: Theme.text2
                    font.pixelSize: 15
                }
                RvButton {
                    Layout.alignment: Qt.AlignHCenter
                    visible: root.vault.helloAvailable && !root.passwordUnlock
                    implicitHeight: 48
                    iconName: "fingerprint"
                    text: "Unlock with Windows Hello"
                    onClicked: root.vault.unlockWithHello(Window.window)
                }
                RvButton {
                    Layout.alignment: Qt.AlignHCenter
                    visible: root.vault.helloAvailable && !root.passwordUnlock
                    primary: false
                    text: "Use master password instead"
                    onClicked: root.passwordUnlock = true
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: !root.vault.helloAvailable || root.passwordUnlock
                    spacing: 10
                    RvTextField {
                        id: masterPassword
                        Layout.fillWidth: true
                        placeholderText: "Your account password"
                        echoMode: TextInput.Password
                        enabled: !root.vault.busy
                        onAccepted: unlockButton.clicked()
                        onVisibleChanged: if (visible) forceActiveFocus()
                    }
                    RvButton {
                        id: unlockButton
                        Layout.fillWidth: true
                        implicitHeight: 44
                        text: root.vault.busy ? "Checking…" : "Unlock"
                        enabled: !root.vault.busy && masterPassword.text.length > 0
                        onClicked: {
                            root.vault.unlockWithPassword(masterPassword.text)
                            masterPassword.text = ""
                        }
                    }
                    RvButton {
                        Layout.alignment: Qt.AlignHCenter
                        visible: root.vault.helloAvailable
                        primary: false
                        text: "Use Windows Hello instead"
                        onClicked: root.passwordUnlock = false
                    }
                }
                Banner { Layout.fillWidth: true; text: root.vault.error }
                Banner {
                    Layout.fillWidth: true
                    error: true
                    text: root.vault.refused === 0 ? ""
                        : (root.vault.refused === 1 ? "One item isn't shown" : root.vault.refused + " items aren't shown")
                          + ": they don't match what this computer last saw, so they may have been changed on the server."
                }
            }
        }

        // --- Items ---
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 380
            visible: !root.vault.locked
            color: Theme.bg
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                anchors.rightMargin: 17
                spacing: 12
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    SearchField {
                        Layout.fillWidth: true
                        placeholderText: "Search vault"
                        text: root.vault.search
                        onTextChanged: root.vault.search = text
                    }
                    IconButton {
                        implicitWidth: 40
                        implicitHeight: 40
                        iconName: "plus"
                        iconColor: Theme.accentContrast
                        tip: "New item"
                        onClicked: newMenu.popup()
                        background: Rectangle { radius: Theme.radiusSmall; color: Theme.accent }
                    }
                }
                ListView {
                    id: itemList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    model: root.vault.items
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    delegate: ItemDelegate {
                        id: itemRow
                        required property var modelData
                        width: ListView.view.width
                        height: 60
                        leftPadding: 10
                        rightPadding: 10
                        onClicked: root.vault.select(modelData.id)
                        background: Rectangle {
                            radius: Theme.radiusControl
                            color: root.vault.selectedId === itemRow.modelData.id ? Theme.selected
                                 : itemRow.hovered ? Qt.alpha(Theme.selected, 0.5) : "transparent"
                        }
                        contentItem: RowLayout {
                            spacing: 12
                            Avatar {
                                size: 38
                                group: true
                                initial: itemRow.modelData.initial
                                tintKey: itemRow.modelData.title
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Text {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: itemRow.modelData.title
                                    textFormat: Text.PlainText
                                    color: Theme.text
                                    font.pixelSize: 15
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: itemRow.modelData.subtitle
                                    textFormat: Text.PlainText
                                    color: Theme.text2
                                    font.pixelSize: 13
                                }
                            }
                            Badge { status: itemRow.modelData.status }
                            Icon { visible: itemRow.modelData.incoming; name: "users"; size: 14; color: Theme.text3 }
                        }
                    }
                    Column {
                        anchors.centerIn: parent
                        width: parent.width - 40
                        visible: itemList.count === 0
                        spacing: 10
                        BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: !root.vault.loaded; visible: running }
                        Text {
                            width: parent.width
                            visible: root.vault.loaded
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            text: root.vault.search.length > 0 ? "Nothing matches that." : root.vault.category === "all"
                                  ? "Your vault is empty. Add your first login with +." : "Nothing here."
                            textFormat: Text.PlainText
                            color: Theme.text3
                        }
                    }
                }
            }
        }

        // --- Item ---
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.vault.locked
            readonly property var s: root.vault.selected

            Text {
                anchors.centerIn: parent
                visible: root.vault.selected.id === undefined
                text: "Select an item"
                color: Theme.text3
            }

            Flickable {
                id: itemPane
                anchors.fill: parent
                visible: root.vault.selected.id !== undefined
                clip: true
                contentWidth: width
                contentHeight: itemColumn.implicitHeight + 48
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                ColumnLayout {
                    id: itemColumn
                    x: 32
                    y: 24
                    width: itemPane.width - 64
                    spacing: 16
                    readonly property var s: root.vault.selected

                    // Header
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        Avatar {
                            size: 56
                            group: true
                            initial: (itemColumn.s.title || "?").charAt(0).toUpperCase()
                            tintKey: itemColumn.s.title || ""
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: itemColumn.s.title || ""
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.family: Theme.displayFamily
                                font.pixelSize: 28
                                font.weight: Font.Bold
                            }
                            Text {
                                visible: text.length > 0
                                text: itemColumn.s.incoming ? "Shared with you by " + itemColumn.s.owner : (itemColumn.s.host || "")
                                textFormat: Text.PlainText
                                color: Theme.text2
                                font.family: itemColumn.s.incoming ? Theme.fontFamily : Theme.monoFamily
                                font.pixelSize: 13
                            }
                        }
                        RvButton {
                            visible: !itemColumn.s.incoming
                            secondary: true
                            text: "Edit"
                            onClicked: editor.openWith(root.vault.editableItem(itemColumn.s.id))
                        }
                        RvButton {
                            visible: !itemColumn.s.incoming
                            primary: false
                            iconName: "share"
                            text: "Share securely"
                            onClicked: sharePicker.open()
                        }
                    }

                    Banner {
                        Layout.fillWidth: true
                        error: root.vault.error.length > 0
                        text: root.vault.error.length > 0 ? root.vault.error : root.vault.notice
                    }

                    // Found in a known data breach
                    Rectangle {
                        Layout.fillWidth: true
                        visible: itemColumn.s.status === "exposed"
                        implicitHeight: exposedRow.implicitHeight + 28
                        radius: Theme.radiusCard
                        color: Theme.dangerSoft
                        border.color: Qt.alpha(Theme.danger, 0.35)
                        RowLayout {
                            id: exposedRow
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 12
                            Icon {
                                name: "triangle-alert"
                                size: 20
                                color: Theme.danger
                            }
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: "This password appears in known data breaches (" + (itemColumn.s.breach || "").toLowerCase()
                                      + "), so attackers try it first. Change it on the site, then update it here."
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: 14
                            }
                            RvButton {
                                secondary: true
                                text: "Generate new"
                                onClicked: root.vault.useGenerated(itemColumn.s.id)
                            }
                        }
                    }

                    // Reused-password warning
                    Rectangle {
                        Layout.fillWidth: true
                        visible: itemColumn.s.status === "reused"
                        implicitHeight: warnRow.implicitHeight + 28
                        radius: Theme.radiusCard
                        color: "#2A2418"
                        border.color: "#4A3F28"
                        RowLayout {
                            id: warnRow
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 12
                            Icon {
                                name: "repeat"
                                size: 20
                                color: Theme.warn
                            }
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: "This password is also used for " + itemColumn.s.reusedWith + (itemColumn.s.reusedWith === 1 ? " other item" : " other items")
                                        + ". Give each account its own password."
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: 14
                            }
                            RvButton {
                                secondary: true
                                text: "Generate new"
                                onClicked: root.vault.useGenerated(itemColumn.s.id)
                            }
                        }
                    }

                    // Side by side when there's room, the generator below when there isn't.
                    GridLayout {
                        Layout.fillWidth: true
                        columns: itemColumn.width >= 680 ? 2 : 1
                        columnSpacing: 16
                        rowSpacing: 16

                        // Fields
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            implicitHeight: fields.implicitHeight
                            radius: Theme.radiusCard
                            color: Theme.surface
                            clip: true
                            ColumnLayout {
                                id: fields
                                width: parent.width
                                spacing: 0

                                FieldRow {
                                    visible: (itemColumn.s.username || "").length > 0
                                    label: "Username"
                                    value: itemColumn.s.username || ""
                                    IconButton { filled: true; iconName: "copy"; tip: "Copy username"; onClicked: root.vault.copyField(itemColumn.s.id, "username") }
                                }
                                FieldRow {
                                    visible: itemColumn.s.type === "wifi"
                                    label: "Network"
                                    value: (itemColumn.s.wifiSsid || "") + (itemColumn.s.wifiSecurity ? "  ·  " + itemColumn.s.wifiSecurity : "")
                                }
                                FieldRow {
                                    visible: itemColumn.s.type === "card"
                                    label: "Card"
                                    value: (itemColumn.s.cardHolder || "") + "  ·  •••• " + (itemColumn.s.cardLast4 || "") + "  ·  " + (itemColumn.s.cardExpiry || "")
                                    IconButton { filled: true; iconName: "copy"; tip: "Copy card number"; onClicked: root.vault.copyField(itemColumn.s.id, "cardNumber") }
                                }

                                // Password
                                Item {
                                    Layout.fillWidth: true
                                    visible: itemColumn.s.hasPassword === true
                                    implicitHeight: pwColumn.implicitHeight + 28
                                    ColumnLayout {
                                        id: pwColumn
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.top: parent.top
                                        anchors.margins: 14
                                        anchors.leftMargin: 16
                                        spacing: 8
                                        RowLayout {
                                            Layout.fillWidth: true
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 3
                                                Text { text: "Password"; color: Theme.text3; font.pixelSize: 12 }
                                                Text {
                                                    Layout.fillWidth: true
                                                    elide: Text.ElideRight
                                                    text: root.revealed ? root.revealedText : "•".repeat(Math.min(itemColumn.s.passwordLength || 0, 32))
                                                    textFormat: Text.PlainText
                                                    color: Theme.text
                                                    font.family: Theme.monoFamily
                                                    font.pixelSize: root.revealed ? 15 : 18
                                                    font.letterSpacing: root.revealed ? 0 : 1
                                                }
                                            }
                                            IconButton {
                                                filled: !root.revealed
                                                toggled: root.revealed
                                                iconName: root.revealed ? "eye-off" : "eye"
                                                tip: root.revealed ? "Hide password" : "Show password"
                                                onClicked: {
                                                    if (root.revealed) {
                                                        root.hide()
                                                    } else {
                                                        root.revealedText = root.vault.revealPassword(itemColumn.s.id)
                                                        root.revealed = true
                                                    }
                                                }
                                            }
                                            IconButton { filled: true; iconName: "copy"; tip: "Copy password"; onClicked: root.vault.copyField(itemColumn.s.id, "password") }
                                        }
                                        StrengthBar { Layout.fillWidth: true; strength: itemColumn.s.strength || ({}) }
                                        // The private breach check: only 5 characters of the password's hash leave this computer.
                                        RowLayout {
                                            spacing: 6
                                            Icon {
                                                name: itemColumn.s.exposed ? "triangle-alert" : "shield-check"
                                                size: 14
                                                color: itemColumn.s.exposed ? Theme.danger : Theme.text3
                                            }
                                            Text {
                                                text: "Breach check: " + (itemColumn.s.breach || "")
                                                textFormat: Text.PlainText
                                                color: itemColumn.s.exposed ? Theme.danger : Theme.text3
                                                font.pixelSize: 12
                                                ToolTip.visible: breachHover.hovered
                                                ToolTip.text: "Checked without revealing the password: only the first 5 characters of its SHA-1 hash are sent, through our server, to Have I Been Pwned."
                                                HoverHandler { id: breachHover }
                                            }
                                        }
                                        Text {
                                            text: "Copied passwords clear from the clipboard after 30 seconds."
                                            color: Theme.text3
                                            font.pixelSize: 12
                                        }
                                    }
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
                                }

                                // One-time code
                                Item {
                                    Layout.fillWidth: true
                                    visible: itemColumn.s.hasTotp === true && root.vault.totpCode.length > 0
                                    implicitHeight: 74
                                    ColumnLayout {
                                        anchors.left: parent.left
                                        anchors.leftMargin: 16
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 3
                                        Text { text: "One-time code"; color: Theme.text3; font.pixelSize: 12 }
                                        Text {
                                            text: root.vault.totpCode
                                            textFormat: Text.PlainText
                                            color: Theme.accent
                                            font.family: Theme.monoFamily
                                            font.pixelSize: 22
                                            font.weight: Font.Medium
                                            font.letterSpacing: 1.5
                                        }
                                    }
                                    Row {
                                        anchors.right: parent.right
                                        anchors.rightMargin: 14
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 10
                                        CountdownRing {
                                            anchors.verticalCenter: parent.verticalCenter
                                            seconds: root.vault.totpSecondsLeft
                                            period: root.vault.totpPeriod
                                        }
                                        IconButton { filled: true; iconName: "copy"; tip: "Copy code"; onClicked: root.vault.copyField(itemColumn.s.id, "totp") }
                                    }
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
                                }

                                // Passkey
                                Item {
                                    Layout.fillWidth: true
                                    visible: (itemColumn.s.passkeyRpId || "").length > 0
                                    implicitHeight: 66
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 16
                                        anchors.rightMargin: 14
                                        spacing: 12
                                        Icon { name: "user-check"; size: 20; color: Theme.accent }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Text { text: "Passkey saved"; color: Theme.text; font.weight: Font.DemiBold }
                                            Text {
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                                text: "Sign in without a password on " + itemColumn.s.passkeyRpId
                                                textFormat: Text.PlainText
                                                color: Theme.text2
                                                font.pixelSize: 13
                                            }
                                        }
                                    }
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
                                }

                                FieldRow {
                                    visible: (itemColumn.s.website || "").length > 0
                                    label: "Website"
                                    value: itemColumn.s.website || ""
                                    IconButton {
                                        // Only web addresses: a shared item could carry file:// or another
                                        // scheme that would start a program on this PC.
                                        readonly property string target: {
                                            const w = (itemColumn.s.website || "").trim()
                                            if (/^https?:\/\//i.test(w)) return w
                                            return /^[A-Za-z][A-Za-z0-9+.-]*:(?!\d)/.test(w) ? "" : "https://" + w
                                        }
                                        visible: target.length > 0
                                        filled: true
                                        iconName: "external"
                                        tip: "Open website"
                                        onClicked: {
                                            App.lock.expectExternalReturn()
                                            Qt.openUrlExternally(target)
                                        }
                                    }
                                }
                                Item {
                                    Layout.fillWidth: true
                                    visible: (itemColumn.s.notes || "").length > 0
                                    implicitHeight: notesColumn.implicitHeight + 28
                                    ColumnLayout {
                                        id: notesColumn
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.top: parent.top
                                        anchors.margins: 14
                                        anchors.leftMargin: 16
                                        spacing: 3
                                        Text { text: "Notes"; color: Theme.text3; font.pixelSize: 12 }
                                        TextEdit {
                                            Layout.fillWidth: true
                                            readOnly: true
                                            selectByMouse: true
                                            wrapMode: TextEdit.Wrap
                                            textFormat: TextEdit.PlainText
                                            text: itemColumn.s.notes || ""
                                            color: Theme.text
                                            font.pixelSize: 14
                                        }
                                    }
                                }
                            }
                        }

                        // Generator and facts
                        ColumnLayout {
                            Layout.preferredWidth: 300
                            Layout.fillWidth: itemColumn.width < 680
                            Layout.alignment: Qt.AlignTop
                            spacing: 16

                            Rectangle {
                                Layout.fillWidth: true
                                visible: !itemColumn.s.incoming
                                implicitHeight: genColumn.implicitHeight + 32
                                radius: Theme.radiusCard
                                color: Theme.surface
                                ColumnLayout {
                                    id: genColumn
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 12
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text { Layout.fillWidth: true; text: "Password generator"; color: Theme.text; font.weight: Font.DemiBold }
                                        IconButton { filled: true; implicitWidth: 34; implicitHeight: 34; iconSize: 16; iconName: "refresh"; tip: "Generate another"; onClicked: root.vault.regenerate() }
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        implicitHeight: genText.implicitHeight + 20
                                        radius: Theme.radiusSmall
                                        color: Theme.surface2
                                        TextEdit {
                                            id: genText
                                            anchors.fill: parent
                                            anchors.margins: 10
                                            readOnly: true
                                            selectByMouse: true
                                            wrapMode: TextEdit.WrapAnywhere
                                            text: root.vault.generated
                                            color: Theme.text
                                            font.family: Theme.monoFamily
                                            font.pixelSize: 14
                                        }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: root.vault.genWords ? "Words" : "Length"; color: Theme.text2; font.pixelSize: 13 }
                                        Text { textFormat: Text.PlainText; text: root.vault.genLength; color: Theme.text; font.family: Theme.monoFamily; font.pixelSize: 13 }
                                    }
                                    Slider {
                                        id: lengthSlider
                                        Layout.fillWidth: true
                                        from: root.vault.genWords ? 4 : 8
                                        to: root.vault.genWords ? 12 : 48
                                        stepSize: 1
                                        value: root.vault.genLength
                                        onMoved: root.vault.genLength = value
                                        background: Rectangle {
                                            x: lengthSlider.leftPadding
                                            y: lengthSlider.topPadding + lengthSlider.availableHeight / 2 - height / 2
                                            width: lengthSlider.availableWidth
                                            height: 4
                                            radius: 2
                                            color: Qt.alpha(Theme.text, 0.14)
                                            Rectangle { width: lengthSlider.visualPosition * parent.width; height: parent.height; radius: 2; color: Theme.accent }
                                        }
                                        handle: Rectangle {
                                            x: lengthSlider.leftPadding + lengthSlider.visualPosition * (lengthSlider.availableWidth - width)
                                            y: lengthSlider.topPadding + lengthSlider.availableHeight / 2 - height / 2
                                            width: 16
                                            height: 16
                                            radius: 8
                                            color: Theme.text
                                        }
                                    }
                                    Row {
                                        spacing: 6
                                        Chip { text: "A–Z"; checked: root.vault.genUpper; onClicked: root.vault.genUpper = !root.vault.genUpper }
                                        Chip { text: "0–9"; checked: root.vault.genDigits; onClicked: root.vault.genDigits = !root.vault.genDigits }
                                        Chip { text: "!@#"; checked: root.vault.genSymbols; enabled: !root.vault.genWords; onClicked: root.vault.genSymbols = !root.vault.genSymbols }
                                        Chip { text: "Words"; checked: root.vault.genWords; onClicked: root.vault.genWords = !root.vault.genWords }
                                    }
                                    RvButton {
                                        Layout.fillWidth: true
                                        text: "Use this password"
                                        enabled: itemColumn.s.hasPassword === true || itemColumn.s.type === "login" || itemColumn.s.type === "wifi"
                                        onClicked: root.vault.useGenerated(itemColumn.s.id)
                                    }
                                    RvButton {
                                        Layout.fillWidth: true
                                        primary: false
                                        iconName: "copy"
                                        text: "Copy"
                                        onClicked: root.vault.copy(root.vault.generated, true)
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: facts.implicitHeight + 28
                                radius: Theme.radiusCard
                                color: Theme.surface
                                GridLayout {
                                    id: facts
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    columns: 2
                                    rowSpacing: 8
                                    columnSpacing: 12
                                    Text { text: "Shared with"; color: Theme.text2; font.pixelSize: 13 }
                                    Text {
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignRight
                                        elide: Text.ElideRight
                                        text: itemColumn.s.incoming ? "You (from " + itemColumn.s.owner + ")"
                                              : (itemColumn.s.sharedWith || []).length === 0 ? "Only you"
                                              : (itemColumn.s.sharedWith || []).map(p => p.handle).join(", ")
                                        textFormat: Text.PlainText
                                        color: Theme.text
                                        font.pixelSize: 13
                                    }
                                    Text { text: "Last changed"; color: Theme.text2; font.pixelSize: 13 }
                                    Text { textFormat: Text.PlainText; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; text: itemColumn.s.lastChanged || "—"; color: Theme.text; font.pixelSize: 13 }
                                }
                            }

                            // Who it's shared with, and stopping.
                            Repeater {
                                model: itemColumn.s.incoming ? [] : (itemColumn.s.sharedWith || [])
                                delegate: RowLayout {
                                    id: sharedPerson
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 10
                                    Avatar { size: 28; initial: sharedPerson.modelData.handle.charAt(0).toUpperCase(); tintKey: sharedPerson.modelData.userId }
                                    Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: sharedPerson.modelData.handle; color: Theme.text; elide: Text.ElideRight }
                                    RvButton {
                                        primary: false
                                        danger: true
                                        implicitHeight: 28
                                        text: "Stop sharing"
                                        onClicked: root.vault.unshare(itemColumn.s.id, sharedPerson.modelData.userId)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
