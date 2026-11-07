pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// One existing link: its settings (name, permission, expiry, limits), its
// password, or a new address for it ("regenerate"). Changes apply on the
// server at once.
RvDialog {
    id: dialog
    required property SharesSession shares
    property var link: ({})
    property string mode: "settings"   // "settings", "password" or "regenerate"
    property string permission: "viewer"
    property string madeUrl

    title: madeUrl.length > 0 ? "New link ready"
           : mode === "password" ? (link.hasPassword ? "Change the password" : "Add a password")
           : mode === "regenerate" ? "Make a new link" : "Link settings"
    width: 520

    function openFor(l, m) {
        link = l
        mode = m
        madeUrl = ""
        permission = l.permission
        labelField.text = l.label || ""
        expiry.currentIndex = l.expiresAtMs > 0 ? 0 : 1
        downloadsField.text = l.maxDownloads > 0 ? String(l.maxDownloads) : ""
        sizeField.text = l.maxFileSizeMb > 0 ? String(Math.round(l.maxFileSizeMb * 100) / 100) : ""
        allowanceField.text = l.allowanceMb > 0 ? String(Math.round(l.allowanceMb * 100) / 100) : ""
        filesField.text = l.maxFiles > 0 ? String(l.maxFiles) : ""
        passwordField.text = ""
        regenPassword.checked = l.hasPassword
        shares.clearMessages()
        open()
    }

    // 0: keep, 1: never, then the usual choices.
    function expiresAtMs() {
        const hour = 3600 * 1000
        switch (expiry.currentIndex) {
        case 0: return dialog.link.expiresAtMs > 0 ? -1 : 0
        case 1: return 0
        case 2: return Date.now() + hour
        case 3: return Date.now() + 24 * hour
        case 4: return Date.now() + 7 * 24 * hour
        case 5: return Date.now() + 30 * 24 * hour
        case 6: return customDate.iso.length > 0 ? Date.parse(customDate.iso) : -1
        default: return -1
        }
    }

    Connections {
        target: dialog.shares
        function onLinkReady(url) {
            if (dialog.opened && dialog.mode === "regenerate") dialog.madeUrl = url
        }
        function onBusyChanged() {
            // Settings and passwords close once saved without an error.
            if (!dialog.shares.busy && dialog.opened && dialog.mode !== "regenerate" && dialog.shares.error.length === 0 && dialog.saving) {
                dialog.saving = false
                dialog.close()
            }
        }
    }
    property bool saving: false

    contentItem: ColumnLayout {
        spacing: 12

        // --- Settings ---
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.mode === "settings"
            spacing: 12
            RvTextField { id: labelField; Layout.fillWidth: true; placeholderText: "Name (only you see it)"; maximumLength: 120 }
            RvSwitch {
                Layout.fillWidth: true
                visible: dialog.link.kind === "folder"
                label: "Recipients can upload"
                description: dialog.permission === "contributor"
                             ? "Anyone with this link can view and download files, and upload new files. They cannot delete or replace your files."
                             : "Anyone with this link can view and download files. Uploading and editing are disabled."
                checked: dialog.permission === "contributor"
                onToggled: (on) => dialog.permission = on ? "contributor" : "viewer"
            }
            InfoCard {
                Layout.fillWidth: true
                visible: dialog.link.permission === "contributor" && dialog.permission === "viewer"
                text: "Uploads stop at once: any still in progress are cancelled. Files already received stay in Incoming."
            }
            RowLayout {
                Layout.fillWidth: true
                visible: dialog.permission === "contributor"
                spacing: 8
                RvTextField { id: sizeField; Layout.fillWidth: true; placeholderText: "Largest file (MB)" }
                RvTextField { id: allowanceField; Layout.fillWidth: true; placeholderText: "Total uploads (MB)" }
                RvTextField { id: filesField; Layout.fillWidth: true; placeholderText: "Most files" }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Text { text: "Expires"; color: Theme.text2 }
                RvChoice {
                    id: expiry
                    Layout.fillWidth: true
                    model: [dialog.link.expiresAtMs > 0 ? "Keep: " + dialog.link.expires : "Never (as now)", "Never", "In 1 hour", "In 1 day", "In 7 days", "In 30 days", "On a date…"]
                    Accessible.name: "Expires"
                }
            }
            ChannelScheduleField { id: customDate; Layout.fillWidth: true; visible: expiry.currentIndex === 6; label: "Expires at"; Component.onCompleted: reset() }
            RvTextField { id: downloadsField; Layout.fillWidth: true; placeholderText: "Download limit (empty: no limit)" }
        }

        // --- Password ---
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.mode === "password"
            spacing: 12
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Anyone opening the link will need it. The link itself stays the same; people who already downloaded files keep them."
                color: Theme.text2
            }
            RvTextField { id: passwordField; Layout.fillWidth: true; placeholderText: "At least 8 characters"; echoMode: TextInput.Password }
            InfoCard {
                Layout.fillWidth: true
                text: "The password isn't part of the link. Send it separately (another app, or in person) so one leaked message isn't enough."
            }
        }

        // --- Regenerate ---
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.mode === "regenerate" && dialog.madeUrl.length === 0
            spacing: 12
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "The current link stops working at once, including downloads in progress. Its settings and counts stay. Files people already downloaded stay with them."
                color: Theme.text2
            }
            RvSwitch {
                id: regenPassword
                Layout.fillWidth: true
                label: "Password"
                description: dialog.link.hasPassword ? "Type it again (or a new one): it isn't stored anywhere." : "Needed to open the new link."
            }
            RvTextField { id: regenPasswordField; Layout.fillWidth: true; visible: regenPassword.checked; placeholderText: "At least 8 characters"; echoMode: TextInput.Password }
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.madeUrl.length > 0
            spacing: 10
            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "The old link no longer works. Send this one to the people who should still have access."; color: Theme.text2 }
            RvTextField { id: newUrl; Layout.fillWidth: true; readOnly: true; text: dialog.madeUrl; onActiveFocusChanged: if (activeFocus) selectAll() }
        }

        Banner { Layout.fillWidth: true; visible: dialog.shares.error.length > 0; text: dialog.shares.error }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            RvButton {
                visible: dialog.mode === "password" && dialog.link.hasPassword && dialog.madeUrl.length === 0
                primary: false
                danger: true
                text: "Remove password"
                enabled: !dialog.shares.busy
                onClicked: {
                    dialog.saving = true
                    dialog.shares.setPassword(dialog.link.id, "")
                }
            }
            RvButton { primary: false; text: dialog.madeUrl.length > 0 ? "Close" : "Cancel"; onClicked: dialog.close() }
            RvButton {
                visible: dialog.madeUrl.length > 0
                iconName: "copy"
                text: "Copy link"
                onClicked: {
                    newUrl.selectAll()
                    newUrl.copy()
                    dialog.close()
                }
            }
            RvButton {
                visible: dialog.madeUrl.length === 0
                text: dialog.shares.busy ? "Working…" : dialog.mode === "regenerate" ? "Make new link" : "Save"
                enabled: !dialog.shares.busy
                         && (dialog.mode !== "password" || passwordField.text.length >= 8)
                         && (dialog.mode !== "regenerate" || !regenPassword.checked || regenPasswordField.text.length >= 8)
                         && (dialog.mode !== "settings" || expiry.currentIndex !== 6 || customDate.iso.length > 0)
                onClicked: {
                    if (dialog.mode === "password") {
                        dialog.saving = true
                        dialog.shares.setPassword(dialog.link.id, passwordField.text)
                    } else if (dialog.mode === "regenerate") {
                        dialog.shares.regenerate(dialog.link.id, regenPassword.checked ? regenPasswordField.text : "")
                    } else {
                        dialog.saving = true
                        dialog.shares.saveSettings(dialog.link.id, {
                            label: labelField.text,
                            permission: dialog.permission,
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
    onClosed: {
        passwordField.text = ""
        regenPasswordField.text = ""
        saving = false
    }
}
