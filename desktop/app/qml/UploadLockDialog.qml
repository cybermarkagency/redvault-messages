import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Uploading files with an optional extra lock (a key or a password), or
// adding / changing / removing the lock of a file that's already stored
// (`purpose: "change"`). The lock wraps each file's own key, so nothing is
// re-encrypted or re-uploaded to change it.
LockDialog {
    id: dialog
    required property FilesSession files
    property string purpose: "upload"
    /// purpose "upload": the local files (file:// URLs).
    property var urls: []
    readonly property var described: purpose === "upload" ? files.describeFiles(urls) : []
    /// purpose "change": the file, and the lock it has now ("", "key", "password").
    property string targetId
    property string targetName
    property string currentLock
    property string mode: "key"
    property string keyId
    property string error

    function openForUpload(list) {
        purpose = "upload"
        urls = list
        mode = files.keys.length > 0 ? "key" : "none"
        open()
    }
    function openForChange(id, name, lock, lockKeyId) {
        purpose = "change"
        targetId = id
        targetName = name
        currentLock = lock
        mode = lock.length > 0 ? lock : files.keys.length > 0 ? "key" : "password"
        if (lockKeyId) keyId = lockKeyId
        open()
    }

    showIcon: purpose === "change"
    iconName: "lock"
    title: purpose === "upload" ? (described.length === 1 ? "Upload 1 file" : "Upload " + described.length + " files")
                                : currentLock.length > 0 ? "Change the lock" : "Add a lock"
    subtitle: purpose === "upload" ? (described.length === 1 ? "Encrypted and hashed on this PC before it leaves it."
                                                               : "Encrypted and hashed on this PC before they leave it.")
                                   : targetName
    width: 620

    onOpened: {
        error = ""
        if (keyId.length === 0 && files.keys.length > 0) keyId = files.keys[files.keys.length - 1].id
        keyPassword.text = ""
    }

    Connections {
        target: dialog.files
        function onLockDone(action, error) {
            if (!dialog.opened || action !== (dialog.purpose === "upload" ? "upload" : "changeLock")) return
            if (error.length === 0) dialog.close()
            else dialog.error = error
        }
    }

    component Choice: AbstractButton {
        id: choice
        property string value
        property string heading
        property string detail
        readonly property bool on: dialog.mode === value
        Layout.fillWidth: true
        implicitHeight: 62
        onClicked: { dialog.mode = value; dialog.error = "" }
        Accessible.role: Accessible.RadioButton
        Accessible.name: heading
        Accessible.checked: on
        background: Rectangle {
            radius: 14
            color: choice.on ? Theme.accentSoft : Theme.surface
            border.color: choice.on ? Theme.accent : Theme.borderStrong
            border.width: 1
        }
        contentItem: RowLayout {
            spacing: 12
            Item { implicitWidth: 4 }
            Rectangle {
                implicitWidth: 18
                implicitHeight: 18
                radius: 9
                color: "transparent"
                border.color: choice.on ? Theme.accent : Theme.text3
                border.width: 2
                Rectangle { anchors.centerIn: parent; width: 8; height: 8; radius: 4; color: Theme.accent; visible: choice.on }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text { textFormat: Text.PlainText; text: choice.heading; color: Theme.text; font.pixelSize: 14; font.weight: Font.DemiBold }
                Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: choice.detail; color: Theme.text2; font.pixelSize: 13; elide: Text.ElideRight }
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 12

        // The files being uploaded.
        Rectangle {
            Layout.fillWidth: true
            visible: dialog.purpose === "upload"
            implicitHeight: Math.min(fileList.contentHeight, 51 * 3)
            radius: 14
            color: Theme.surface
            clip: true
            ListView {
                id: fileList
                anchors.fill: parent
                model: dialog.described
                interactive: contentHeight > height
                delegate: RowLayout {
                    id: fileItem
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 51
                    spacing: 12
                    Item { implicitWidth: 0 }
                    Rectangle {
                        implicitWidth: 30; implicitHeight: 30; radius: 8
                        color: Theme.surface3
                        Text { textFormat: Text.PlainText; anchors.centerIn: parent; text: fileItem.modelData.typeLabel; color: Theme.text2; font.family: Theme.monoFamily; font.pixelSize: 9 }
                    }
                    Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: fileItem.modelData.name; color: Theme.text; elide: Text.ElideMiddle }
                    Text { textFormat: Text.PlainText; text: fileItem.modelData.size; color: Theme.text2; font.family: Theme.monoFamily; font.pixelSize: 13 }
                    Item { implicitWidth: 2 }
                }
            }
        }

        Text { Layout.topMargin: 4; text: "Extra lock"; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }

        Choice {
            value: "none"
            heading: dialog.purpose === "change" && dialog.currentLock.length > 0 ? "Remove the lock" : "No extra lock"
            detail: dialog.purpose === "upload" ? "Your account protects them, like your other files." : "Your account protects it, like your other files."
        }
        Choice {
            value: "key"
            heading: "Lock with a key"
            detail: dialog.purpose === "upload" ? "Opening them will need your account plus the key." : "Opening it will need your account plus the key."
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 30
            visible: dialog.mode === "key"
            spacing: 8
            KeyPicker {
                Layout.fillWidth: true
                files: dialog.files
                keyId: dialog.keyId
                onKeyIdChanged: dialog.keyId = keyId
            }
            RvTextField {
                id: keyPassword
                Layout.fillWidth: true
                // Re-read when keys change (a key opened this session needs no password).
                visible: dialog.files.keys, dialog.files.keyNeedsPassword(dialog.keyId)
                implicitHeight: 44
                echoMode: TextInput.Password
                placeholderText: "Key password"
                Accessible.name: "Key password"
            }
        }
        Choice {
            value: "password"
            heading: "Lock with a password"
            detail: dialog.purpose === "upload" ? "Opening them will need your account plus this password." : "Opening it will need your account plus this password."
        }
        Loader {
            id: passwordLoader
            Layout.fillWidth: true
            Layout.leftMargin: 30
            active: dialog.mode === "password"
            visible: active
            sourceComponent: LockPasswordField { files: dialog.files; label: "File password" }
        }

        InfoCard {
            Layout.fillWidth: true
            visible: dialog.error.length > 0
            tone: "error"
            text: dialog.error
        }
        InfoCard {
            Layout.fillWidth: true
            text: "The lock wraps each file's own key. Our servers and the storage provider see no difference between locked and unlocked files. You can add, change or remove a lock later without re-uploading."
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 10
            Item { Layout.fillWidth: true }
            RvButton { secondary: true; implicitHeight: 46; text: "Cancel"; onClicked: dialog.close() }
            RvButton {
                implicitHeight: 46
                danger: dialog.purpose === "change" && dialog.mode === "none"
                text: dialog.files.lockBusy ? "Working…"
                      : dialog.purpose === "upload" ? (dialog.mode === "none" ? "Upload" : "Upload and lock")
                      : dialog.mode === "none" ? "Remove lock" : "Save lock"
                enabled: !dialog.files.lockBusy
                         && (dialog.mode !== "key" || (dialog.keyId.length > 0 && (!keyPassword.visible || keyPassword.text.length > 0)))
                         && (dialog.mode !== "password" || (passwordLoader.item && passwordLoader.item.acceptable))
                         && !(dialog.purpose === "change" && dialog.mode === "none" && dialog.currentLock.length === 0)
                onClicked: {
                    dialog.error = ""
                    const filePassword = dialog.mode === "password" && passwordLoader.item ? passwordLoader.item.text : ""
                    if (dialog.purpose === "upload") dialog.files.uploadLocked(dialog.urls, dialog.mode, dialog.keyId, keyPassword.text, filePassword)
                    else dialog.files.changeLock(dialog.targetId, dialog.mode, dialog.keyId, keyPassword.text, filePassword)
                }
            }
        }
    }
}
