pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Share: a public link to a file or folder that anyone can open in a
// browser, without an account (docs/public-share-links.md). The links already
// made to this item, then a new one: what recipients can do, a password,
// an expiry and limits. Then the link itself, to copy.
RvDialog {
    id: dialog
    required property SharesSession shares
    property string itemId
    property string itemName
    property bool isFolder: false
    property string madeUrl
    property bool madeHasPassword: false
    property string madePermission: "viewer"
    property string permission: "viewer"

    readonly property string viewerText: "Anyone with this link can view and download files. Uploading and editing are disabled."
    readonly property string contributorText: "Anyone with this link can view and download files, and upload new files. They cannot delete or replace your files."
    readonly property string passwordNote: "The password isn't part of the link. Send it separately (another app, or in person) so one leaked message isn't enough."

    title: madeUrl.length > 0 ? "Link ready" : "Share “" + itemName + "”"
    width: 560

    function openFor(id, name, folder) {
        itemId = id
        itemName = name
        isFolder = folder
        madeUrl = ""
        permission = "viewer"
        labelField.text = ""
        passwordSwitch.checked = false
        passwordField.text = ""
        expiry.currentIndex = 0
        downloadsField.text = ""
        sizeField.text = ""
        allowanceField.text = ""
        filesField.text = ""
        shares.clearMessages()
        open()
    }

    function expiresAtMs() {
        const hour = 3600 * 1000
        switch (expiry.currentIndex) {
        case 1: return Date.now() + hour
        case 2: return Date.now() + 24 * hour
        case 3: return Date.now() + 7 * 24 * hour
        case 4: return Date.now() + 30 * 24 * hour
        case 5: return customDate.iso.length > 0 ? Date.parse(customDate.iso) : -1
        default: return 0
        }
    }

    Connections {
        target: dialog.shares
        function onLinkReady(url, permission, hasPassword) {
            if (!dialog.opened) return
            dialog.madeUrl = url
            dialog.madePermission = permission
            dialog.madeHasPassword = hasPassword
            dialog.shares.clearMessages()
        }
    }

    contentItem: ScrollView {
        implicitHeight: Math.min(body.implicitHeight, 620)
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            id: body
            width: parent.width
            spacing: 12

            // --- The link that was just made ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: dialog.madeUrl.length > 0
                spacing: 12
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: dialog.madePermission === "contributor" ? dialog.contributorText : dialog.viewerText
                    textFormat: Text.PlainText
                    color: Theme.text2
                }
                RvTextField {
                    id: urlField
                    objectName: "shareLinkUrl"
                    Layout.fillWidth: true
                    readOnly: true
                    text: dialog.madeUrl
                    Accessible.name: "Share link"
                    onActiveFocusChanged: if (activeFocus) selectAll()
                }
                InfoCard { Layout.fillWidth: true; visible: dialog.madeHasPassword; tone: "neutral"; iconName: "info"; text: dialog.passwordNote }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    RvButton {
                        secondary: true
                        iconName: "copy"
                        text: copiedTimer.running ? "Copied" : "Copy link"
                        onClicked: {
                            urlField.selectAll()
                            urlField.copy()
                            copiedTimer.restart()
                        }
                    }
                    RvButton { text: "Done"; onClicked: dialog.close() }
                }
                Timer { id: copiedTimer; interval: 2000 }
            }

            // --- Existing links, then a new one ---
            ColumnLayout {
                Layout.fillWidth: true
                visible: dialog.madeUrl.length === 0
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    readonly property var existing: dialog.opened ? dialog.shares.linksFor(dialog.itemId) : []
                    id: existingBox
                    visible: existing.length > 0
                    spacing: 6
                    Text { text: "Links you made"; color: Theme.text2; font.weight: Font.DemiBold }
                    Repeater {
                        model: existingBox.existing
                        delegate: RowLayout {
                            id: existingRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 8
                            Icon { name: "link"; size: 14; color: Theme.accent }
                            Text { textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight; text: existingRow.modelData.label; color: Theme.text }
                            Pill { text: existingRow.modelData.permission === "contributor" ? "Contributor" : "Viewer" }
                            Pill { text: existingRow.modelData.statusText; tone: existingRow.modelData.active ? "accent" : "danger" }
                            IconButton {
                                visible: existingRow.modelData.active && existingRow.modelData.url.length > 0
                                implicitWidth: 30; implicitHeight: 30; iconSize: 14
                                iconName: "copy"
                                tip: "Copy link"
                                onClicked: dialog.shares.copyLink(existingRow.modelData.id)
                            }
                        }
                    }
                    Rectangle { Layout.fillWidth: true; Layout.topMargin: 6; implicitHeight: 1; color: Theme.border }
                }

                Text { text: "New link"; color: Theme.text2; font.weight: Font.DemiBold }
                RvTextField {
                    id: labelField
                    Layout.fillWidth: true
                    placeholderText: "Name (only you see it): " + dialog.itemName
                    maximumLength: 120
                }

                Text { text: "Recipients can"; color: Theme.text2; font.pixelSize: 13 }
                Repeater {
                    model: [
                        { key: "viewer", title: "View and download", text: dialog.viewerText },
                        { key: "contributor", title: "View, download and upload", text: dialog.isFolder ? dialog.contributorText : "Folder links only: a file link can't take uploads." }
                    ]
                    delegate: Rectangle {
                        id: permissionCard
                        required property var modelData
                        readonly property bool on: dialog.permission === modelData.key
                        readonly property bool allowed: modelData.key === "viewer" || dialog.isFolder
                        Layout.fillWidth: true
                        implicitHeight: permissionText.implicitHeight + 22
                        radius: Theme.radiusControl
                        color: on ? Theme.accentSoft : Theme.surface2
                        border.color: on ? Theme.accentBorder : Theme.borderStrong
                        opacity: allowed ? 1 : 0.55
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData.title
                        Accessible.checked: on
                        RowLayout {
                            id: permissionText
                            anchors.fill: parent
                            anchors.margins: 11
                            spacing: 10
                            Rectangle {
                                Layout.alignment: Qt.AlignTop
                                Layout.topMargin: 2
                                implicitWidth: 16; implicitHeight: 16; radius: 8
                                color: "transparent"
                                border.color: permissionCard.on ? Theme.accent : Theme.text3
                                border.width: 2
                                Rectangle { anchors.centerIn: parent; width: 8; height: 8; radius: 4; color: Theme.accent; visible: permissionCard.on }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { textFormat: Text.PlainText; text: permissionCard.modelData.title; color: Theme.text; font.weight: Font.DemiBold }
                                Text { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; text: permissionCard.modelData.text; color: Theme.text2; font.pixelSize: 13 }
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: permissionCard.allowed
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dialog.permission = permissionCard.modelData.key
                        }
                    }
                }
                InfoCard {
                    Layout.fillWidth: true
                    visible: dialog.permission === "contributor"
                    tone: "warn"
                    text: "Anyone who receives or is forwarded this link can upload files until you change its permissions, revoke it, or it expires."
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: dialog.permission === "contributor"
                    spacing: 8
                    RvTextField { id: sizeField; Layout.fillWidth: true; placeholderText: "Largest file (MB)"; inputMethodHints: Qt.ImhFormattedNumbersOnly }
                    RvTextField { id: allowanceField; Layout.fillWidth: true; placeholderText: "Total uploads (MB)"; inputMethodHints: Qt.ImhFormattedNumbersOnly }
                    RvTextField { id: filesField; Layout.fillWidth: true; placeholderText: "Most files"; inputMethodHints: Qt.ImhDigitsOnly }
                }

                RvSwitch {
                    id: passwordSwitch
                    Layout.fillWidth: true
                    label: "Password"
                    description: "Needed to open the link, on top of the link itself."
                }
                RvTextField {
                    id: passwordField
                    objectName: "shareLinkPassword"
                    Layout.fillWidth: true
                    visible: passwordSwitch.checked
                    placeholderText: "At least 8 characters"
                    echoMode: TextInput.Password
                }
                InfoCard { Layout.fillWidth: true; visible: passwordSwitch.checked; tone: "neutral"; text: dialog.passwordNote }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text { text: "Expires"; color: Theme.text2 }
                    RvChoice {
                        id: expiry
                        Layout.fillWidth: true
                        model: ["Never", "In 1 hour", "In 1 day", "In 7 days", "In 30 days", "On a date…"]
                        Accessible.name: "Expires"
                    }
                }
                ChannelScheduleField {
                    id: customDate
                    Layout.fillWidth: true
                    visible: expiry.currentIndex === 5
                    label: "Expires at"
                    Component.onCompleted: reset()
                }
                RvTextField {
                    id: downloadsField
                    Layout.fillWidth: true
                    placeholderText: "Download limit (empty: no limit)"
                    inputMethodHints: Qt.ImhDigitsOnly
                }

                Banner { Layout.fillWidth: true; visible: dialog.shares.error.length > 0; text: dialog.shares.error }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    RvButton { primary: false; text: "Cancel"; onClicked: dialog.close() }
                    RvButton {
                        objectName: "createShareLink"
                        iconName: "link"
                        text: dialog.shares.busy ? "Encrypting…" : "Create link"
                        enabled: !dialog.shares.busy && (!passwordSwitch.checked || passwordField.text.length >= 8)
                                 && (expiry.currentIndex !== 5 || customDate.iso.length > 0)
                        onClicked: dialog.shares.createLink(dialog.itemId, {
                            permission: dialog.permission,
                            label: labelField.text.length > 0 ? labelField.text : dialog.itemName,
                            password: passwordSwitch.checked ? passwordField.text : "",
                            expiresAtMs: dialog.expiresAtMs(),
                            maxDownloads: downloadsField.text,
                            maxFileSizeMb: sizeField.text,
                            allowanceMb: allowanceField.text,
                            maxFiles: filesField.text
                        })
                    }
                }
            }
        }
    }
    onClosed: passwordField.text = ""
}
