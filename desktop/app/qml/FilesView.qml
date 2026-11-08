// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// Files: encrypted cloud storage. Folder sidebar (240px), file table, and the
// selected file's details (320px). Everything is encrypted on this computer
// before upload, file names too; search runs here over the decrypted index.
Item {
    id: root
    required property FilesSession files
    required property ChatSession chats
    /// Public share links (null until signed in).
    property SharesSession shares: App.shares

    function kindTile(label, tint) { return { label: label, tint: tint } }

    FileDialog {
        id: uploadDialog
        title: "Upload files"
        fileMode: FileDialog.OpenFiles
        onAccepted: uploadLock.openForUpload(selectedFiles)
    }
    UploadLockDialog { id: uploadLock; objectName: "uploadLockDialog"; files: root.files }
    UnlockFileDialog {
        id: unlockDialog
        files: root.files
        // "Change lock" on a locked file unlocks it first, then asks for the new lock.
        onUnlocked: (id, then) => {
            if (then !== "changeLock" && then !== "removeLock") return
            const s = root.files.selected
            if (s.id !== id) return
            uploadLock.openForChange(s.id, s.name, s.lockKind, s.lockKeyId)
            if (then === "removeLock") uploadLock.mode = "none"
        }
    }
    FileDialog {
        id: versionDialog
        property string targetId
        title: "Upload a new version"
        fileMode: FileDialog.OpenFile
        onAccepted: root.files.uploadNewVersion(targetId, selectedFile)
    }
    FileDialog {
        id: saveDialog
        property string targetId
        title: "Save a decrypted copy"
        fileMode: FileDialog.SaveFile
        currentFolder: StandardPaths.writableLocation(StandardPaths.DownloadLocation)
        onAccepted: root.files.download(targetId, selectedFile)
    }
    FolderDialog {
        id: pcBackupDialog
        title: "Choose a folder on this PC to keep backed up"
        onAccepted: root.files.setPcBackupFolder(selectedFolder)
    }

    // --- Small dialogs ---
    RvDialog {
        id: nameDialog
        property string mode: "folder"   // "folder" or "rename"
        property string targetId
        title: mode === "folder" ? "New folder" : "Rename"
        width: 420
        contentItem: ColumnLayout {
            spacing: 14
            RvTextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: nameDialog.mode === "folder" ? "Folder name" : "Name"
                onAccepted: nameOk.clicked()
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                RvButton { primary: false; text: "Cancel"; onClicked: nameDialog.close() }
                RvButton {
                    id: nameOk
                    text: nameDialog.mode === "folder" ? "Create" : "Rename"
                    enabled: nameField.text.trim().length > 0
                    onClicked: {
                        if (nameDialog.mode === "folder") root.files.newFolder(nameField.text)
                        else root.files.rename(nameDialog.targetId, nameField.text)
                        nameDialog.close()
                    }
                }
            }
        }
        onOpened: nameField.forceActiveFocus()
    }

    ShareLinkDialog {
        id: shareLinkDialog
        objectName: "shareLinkDialog"
        shares: root.shares
    }

    ContactPickerDialog {
        id: sharePicker
        property string targetId
        title: "Share with"
        chats: root.chats
        onPicked: (userId) => root.files.shareWith(targetId, userId)
    }

    Menu {
        id: moveMenu
        property string targetId
        Repeater {
            model: moveMenu.targetId.length > 0 ? root.files.moveTargets(moveMenu.targetId) : []
            delegate: MenuItem {
                required property var modelData
                text: modelData.name
                onTriggered: root.files.move(moveMenu.targetId, modelData.id)
            }
        }
    }

    Menu {
        id: rowMenu
        property var row: ({})
        readonly property bool needsUnlock: (rowMenu.row.lock || "").length > 0 && !rowMenu.row.unlocked
        MenuItem {
            text: rowMenu.row.unlocked ? "Lock now" : "Unlock…"
            visible: (rowMenu.row.lock || "").length > 0
            height: visible ? implicitHeight : 0
            onTriggered: {
                if (rowMenu.row.unlocked) return root.files.lockNow(rowMenu.row.id)
                root.files.select(rowMenu.row.id)
                unlockDialog.openFor(root.files.selected)
            }
        }
        MenuItem {
            text: "Open"
            enabled: rowMenu.row.kind === "file" && !rowMenu.needsUnlock
            onTriggered: root.files.openFile(rowMenu.row.id)
        }
        MenuItem {
            text: "Download…"
            enabled: rowMenu.row.kind === "file" && !rowMenu.needsUnlock
            onTriggered: {
                saveDialog.targetId = rowMenu.row.id
                saveDialog.selectedFile = saveDialog.currentFolder + "/" + encodeURIComponent(rowMenu.row.name)
                App.lock.expectExternalReturn()
                saveDialog.open()
            }
        }
        MenuItem {
            text: "Share link…"
            enabled: root.shares !== null && !rowMenu.row.incoming && !rowMenu.row.trashed && (rowMenu.row.lock || "").length === 0
            onTriggered: shareLinkDialog.openFor(rowMenu.row.id, rowMenu.row.name, rowMenu.row.kind === "folder")
        }
        MenuItem {
            text: "Rename…"
            enabled: !rowMenu.row.incoming && !rowMenu.row.trashed
            onTriggered: {
                nameDialog.mode = "rename"
                nameDialog.targetId = rowMenu.row.id
                nameField.text = rowMenu.row.name
                nameDialog.open()
            }
        }
        MenuItem {
            text: "Move to…"
            enabled: !rowMenu.row.incoming && !rowMenu.row.trashed
            onTriggered: {
                moveMenu.targetId = rowMenu.row.id
                moveMenu.popup()
            }
        }
        MenuItem {
            text: "Upload a new version…"
            enabled: rowMenu.row.kind === "file" && !rowMenu.row.incoming && !rowMenu.row.trashed
            onTriggered: {
                versionDialog.targetId = rowMenu.row.id
                App.lock.expectExternalReturn()
                versionDialog.open()
            }
        }
        MenuSeparator {}
        MenuItem {
            text: rowMenu.row.trashed ? "Restore" : "Move to Trash"
            enabled: !rowMenu.row.incoming
            onTriggered: rowMenu.row.trashed ? root.files.restore(rowMenu.row.id) : root.files.trash(rowMenu.row.id)
        }
        MenuItem {
            text: "Delete forever"
            visible: rowMenu.row.trashed === true
            height: visible ? implicitHeight : 0
            onTriggered: root.files.deleteForever(rowMenu.row.id)
        }
    }

    component SideItem: ItemDelegate {
        id: side
        property string key
        property string iconName
        property string current
        property int count: -1
        signal picked(string key)
        Layout.fillWidth: true
        implicitHeight: 42
        leftPadding: 14
        onClicked: picked(key)
        background: Rectangle {
            radius: Theme.radiusControl
            color: side.current === side.key ? Theme.selected : side.hovered ? Qt.alpha(Theme.selected, 0.5) : "transparent"
        }
        contentItem: RowLayout {
            spacing: 12
            Icon {
                name: side.iconName
                size: 18
                color: side.current === side.key ? Theme.accent : Theme.text2
            }
            Text {
                Layout.fillWidth: true
                text: side.text
                textFormat: Text.PlainText
                color: side.current === side.key ? Theme.accent : Theme.text
                font.pixelSize: 14
                font.weight: side.current === side.key ? Font.DemiBold : Font.Normal
            }
            Text {
                visible: side.count >= 0
                text: side.count
                textFormat: Text.PlainText
                color: side.current === side.key ? Theme.accent : Theme.text3
                font.family: Theme.monoFamily
                font.pixelSize: 11
            }
        }
    }

    // The extra-lock pill in the table: "Key · 7F3A", "Password", "Unlocked now" or a dash.
    component LockPill: Item {
        id: pill
        property string lock
        property string lockShort
        property bool unlocked
        implicitHeight: 24
        implicitWidth: lock.length === 0 ? dash.implicitWidth : pillBox.implicitWidth
        Text { id: dash; visible: pill.lock.length === 0; anchors.verticalCenter: parent.verticalCenter; text: "—"; color: Theme.text3 }
        Rectangle {
            id: pillBox
            visible: pill.lock.length > 0
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: pillRow.implicitWidth + 16
            implicitHeight: 24
            radius: 8
            color: pill.unlocked ? Theme.accentSoft : Theme.lockBg
            Row {
                id: pillRow
                anchors.centerIn: parent
                spacing: 5
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: pill.unlocked ? "lock-open" : pill.lock === "key" ? "key" : "credit-card"
                    size: 12
                    color: pill.unlocked ? Theme.accent : Theme.lockText
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: pill.unlocked ? "Unlocked now" : pill.lock === "key" ? "Key ·" : "Password"
                    textFormat: Text.PlainText
                    color: pill.unlocked ? Theme.accent : Theme.lockText
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !pill.unlocked && pill.lock === "key"
                    text: pill.lockShort
                    textFormat: Text.PlainText
                    color: Theme.lockText
                    font.family: Theme.monoFamily
                    font.pixelSize: 11
                }
            }
        }
    }

    // A locked file's tile: a lock instead of its type.
    component LockTile: Rectangle {
        property int size: 32
        implicitWidth: size
        implicitHeight: size
        radius: 8
        color: Theme.lockBg
        Icon { anchors.centerIn: parent; name: "lock"; size: 14; color: Theme.lockText }
    }

    component TypeTile: Rectangle {
        id: tile
        property string label
        property color tint
        property int size: 32
        implicitWidth: size
        implicitHeight: size
        radius: 8
        color: Qt.alpha(tint, 0.14)
        Text {
            anchors.centerIn: parent
            text: tile.label
            textFormat: Text.PlainText
            color: tile.tint
            font.family: Theme.monoFamily
            font.pixelSize: tile.size > 60 ? 22 : 9
            font.weight: Font.Medium
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // --- Sidebar ---
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
                    text: "Files"
                    color: Theme.text
                    font.family: Theme.displayFamily
                    font.pixelSize: 28
                    font.weight: Font.Bold
                }
                SideItem { key: "all"; iconName: "folder"; text: "All files"; current: root.files.section; onPicked: (k) => root.files.section = k }
                SideItem { key: "recent"; iconName: "clock"; text: "Recent"; current: root.files.section; onPicked: (k) => root.files.section = k }
                SideItem { key: "locked"; iconName: "lock"; text: "Locked files"; count: root.files.lockedCount; current: root.files.section; onPicked: (k) => root.files.section = k }
                SideItem { key: "photos"; iconName: "image"; text: "Photos"; current: root.files.section; onPicked: (k) => root.files.section = k }
                SideItem { key: "shared"; iconName: "users"; text: "Shared with me"; current: root.files.section; onPicked: (k) => root.files.section = k }
                SideItem { key: "links"; iconName: "link"; text: "Shared links"; count: root.shares ? root.shares.activeLinks : -1; current: root.files.section; onPicked: (k) => root.files.section = k }
                SideItem { key: "incoming"; iconName: "inbox"; text: "Incoming"; count: root.shares && root.shares.incomingCount > 0 ? root.shares.incomingCount : -1; current: root.files.section; onPicked: (k) => root.files.section = k }
                SideItem { key: "trash"; iconName: "trash"; text: "Trash"; current: root.files.section; onPicked: (k) => root.files.section = k }
                Text {
                    Layout.leftMargin: 14
                    Layout.topMargin: 18
                    Layout.bottomMargin: 4
                    text: "SECURITY"
                    color: Theme.text3
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.8
                }
                SideItem {
                    key: "keys"
                    iconName: "key"
                    text: "Encryption keys"
                    count: root.files.keys.length
                    current: root.files.section
                    onPicked: (k) => { root.files.hideKeyExport(); root.files.section = k }
                }
                Item { Layout.fillHeight: true }

                // Storage: your plan's space, and how much is used.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: usageColumn.implicitHeight + 28
                    radius: Theme.radiusCard
                    color: root.files.usage.nearlyFull ? Theme.warnSoft : Theme.accentSoft
                    border.color: root.files.usage.nearlyFull ? Qt.alpha(Theme.warn, 0.35) : Theme.accentBorder
                    ColumnLayout {
                        id: usageColumn
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 6
                        RowLayout {
                            spacing: 8
                            Icon { name: "cloud"; size: 16; color: root.files.usage.nearlyFull ? Theme.warn : Theme.accent }
                            Text {
                                text: (root.files.usage.plan || "") + " · " + (root.files.usage.limitText || "")
                                textFormat: Text.PlainText
                                color: root.files.usage.nearlyFull ? Theme.warn : Theme.accent
                                font.weight: Font.DemiBold
                            }
                        }
                        Text {
                            text: (root.files.usage.usedText || "0 bytes") + " used"
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.family: Theme.monoFamily
                            font.pixelSize: 13
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 4
                            radius: 2
                            color: Qt.alpha(Theme.text, 0.12)
                            Rectangle {
                                width: parent.width * (root.files.usage.fraction || 0)
                                height: parent.height
                                radius: 2
                                color: root.files.usage.full ? Theme.danger : root.files.usage.nearlyFull ? Theme.warn : Theme.accent
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: root.files.usage.full ? "Your storage is full. Delete files or empty the Trash to upload more."
                                  : (root.files.usage.remainingText || "") + " left. Everything is encrypted before it leaves this PC."
                            textFormat: Text.PlainText
                            color: Theme.text2
                            font.pixelSize: 12
                        }
                    }
                }
            }
        }

        FileKeysView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.files.section === "keys"
            files: root.files
        }

        // Public share links and what came in through them (docs/public-share-links.md).
        ShareLinksView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.files.section === "links" && root.shares !== null
            shares: root.shares
        }
        IncomingSharesView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.files.section === "incoming" && root.shares !== null
            shares: root.shares
        }

        // --- Main ---
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.files.section !== "keys" && root.files.section !== "links" && root.files.section !== "incoming"

            DropArea {
                anchors.fill: parent
                onDropped: (drop) => {
                    if (drop.hasUrls) uploadLock.openForUpload(drop.urls)
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 28
                anchors.topMargin: 20
                spacing: 16

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        spacing: 4
                        Row {
                            spacing: 4
                            Text {
                                text: "Files"
                                color: Theme.text3
                                font.pixelSize: 13
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.files.openFolder("") }
                            }
                            Text { text: "/"; color: Theme.text3; font.pixelSize: 13 }
                            Text {
                                visible: root.files.breadcrumb.length === 0
                                text: ({ all: "All files", recent: "Recent", photos: "Photos", locked: "Locked files", shared: "Shared with me", links: "Shared links", trash: "Trash" })[root.files.section] || ""
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: 13
                            }
                            Repeater {
                                model: root.files.breadcrumb
                                delegate: Row {
                                    id: crumb
                                    required property var modelData
                                    required property int index
                                    spacing: 4
                                    Text { visible: crumb.index > 0; text: "/"; color: Theme.text3; font.pixelSize: 13 }
                                    Text {
                                        text: crumb.modelData.name
                                        textFormat: Text.PlainText
                                        color: crumb.index === root.files.breadcrumb.length - 1 ? Theme.text : Theme.text3
                                        font.pixelSize: 13
                                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.files.openFolder(crumb.modelData.id) }
                                    }
                                }
                            }
                        }
                        RowLayout {
                            spacing: 6
                            Icon { name: "lock"; size: 12; color: Theme.accent }
                            Text {
                                text: "End-to-end encrypted · hash checked on download"
                                color: Theme.accent
                                font.family: Theme.monoFamily
                                font.pixelSize: 12
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                    RvButton {
                        visible: root.files.section === "trash"
                        primary: false
                        danger: true
                        iconName: "trash"
                        text: "Empty Trash"
                        onClicked: root.files.emptyTrash()
                    }
                    RvButton {
                        visible: root.files.section === "all"
                        secondary: true
                        iconName: "folder-plus"
                        text: "New folder"
                        onClicked: {
                            nameDialog.mode = "folder"
                            nameField.text = ""
                            nameDialog.open()
                        }
                    }
                    RvButton {
                        visible: root.files.section !== "trash"
                        iconName: "upload"
                        text: "Upload"
                        onClicked: {
                            App.lock.expectExternalReturn()
                            uploadDialog.open()
                        }
                    }
                }

                SearchField {
                    Layout.fillWidth: true
                    implicitHeight: 42
                    placeholderText: "Search your files (searched on this device)"
                    text: root.files.search
                    onTextChanged: root.files.search = text
                }

                Banner {
                    Layout.fillWidth: true
                    text: root.files.error
                    MouseArea { anchors.fill: parent; onClicked: root.files.clearMessages() }
                }
                Banner {
                    Layout.fillWidth: true
                    error: false
                    text: root.files.notice
                }

                // Folder tiles, and the PC backup.
                // Never taller than its tiles: an empty nested layout would otherwise take
                // the table's height (inside a folder without subfolders).
                GridLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    visible: root.files.section === "all" && root.files.search.length === 0
                             && (root.files.folders.length > 0 || pcTile.visible)
                    columns: Math.max(1, Math.floor(width / 190))
                    columnSpacing: 12
                    rowSpacing: 12
                    Repeater {
                        model: root.files.folders
                        delegate: AbstractButton {
                            id: folderTile
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 98
                            hoverEnabled: true
                            onClicked: root.files.openFolder(modelData.id)
                            onPressAndHold: {
                                rowMenu.row = { id: modelData.id, kind: "folder", name: modelData.name, incoming: false, trashed: false }
                                rowMenu.popup()
                            }
                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                onTapped: {
                                    rowMenu.row = { id: folderTile.modelData.id, kind: "folder", name: folderTile.modelData.name, incoming: false, trashed: false }
                                    rowMenu.popup()
                                }
                            }
                            background: Rectangle {
                                radius: Theme.radiusCard
                                color: folderTile.hovered ? Theme.selected : Theme.surface
                                border.color: Theme.borderStrong
                            }
                            contentItem: ColumnLayout {
                                spacing: 4
                                Icon {
                                    Layout.leftMargin: 14
                                    Layout.topMargin: 14
                                    name: folderTile.modelData.pcBackup ? "monitor" : "folder"
                                    size: 18
                                    color: Theme.tintFor(folderTile.modelData.name)
                                }
                                Item { Layout.fillHeight: true }
                                Text {
                                    Layout.leftMargin: 14
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: folderTile.modelData.name
                                    textFormat: Text.PlainText
                                    color: Theme.text
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.leftMargin: 14
                                    Layout.bottomMargin: 12
                                    text: folderTile.modelData.detail
                                    textFormat: Text.PlainText
                                    color: Theme.text3
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                    // Set up PC backup (only while it isn't).
                    AbstractButton {
                        id: pcTile
                        visible: root.files.pcBackupPath.length === 0 && root.files.breadcrumb.length === 0
                        Layout.fillWidth: true
                        implicitHeight: 98
                        hoverEnabled: true
                        onClicked: {
                            App.lock.expectExternalReturn()
                            pcBackupDialog.open()
                        }
                        background: Rectangle {
                            radius: Theme.radiusCard
                            color: pcTile.hovered ? Theme.selected : "transparent"
                            border.color: Theme.borderStrong
                            border.width: 1
                        }
                        contentItem: ColumnLayout {
                            spacing: 4
                            Icon { Layout.leftMargin: 14; Layout.topMargin: 14; name: "monitor"; size: 18; color: Theme.text3 }
                            Item { Layout.fillHeight: true }
                            Text { Layout.leftMargin: 14; text: "PC backup"; color: Theme.text; font.weight: Font.DemiBold }
                            Text { Layout.leftMargin: 14; Layout.bottomMargin: 12; text: "Keep a folder backed up"; color: Theme.text3; font.pixelSize: 12 }
                        }
                    }
                }

                // --- Table ---
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Theme.radiusCard
                    color: "transparent"
                    border.color: Theme.borderStrong
                    clip: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 38
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16
                            spacing: 12
                            Text { Layout.fillWidth: true; Layout.preferredWidth: 1; text: "NAME"; color: Theme.text3; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.8 }
                            Text { Layout.preferredWidth: 140; text: "EXTRA LOCK"; color: Theme.text3; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.8 }
                            Text { Layout.preferredWidth: 96; text: "MODIFIED"; color: Theme.text3; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.8 }
                            Text { Layout.preferredWidth: 72; text: "SIZE"; color: Theme.text3; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.8 }
                            Text { Layout.preferredWidth: 112; text: "STATUS"; color: Theme.text3; font.pixelSize: 11; font.weight: Font.DemiBold; font.letterSpacing: 0.8 }
                        }
                        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }

                        ListView {
                            id: table
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: root.files.rows
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar {}
                            delegate: ItemDelegate {
                                id: fileRow
                                required property var modelData
                                width: ListView.view.width
                                height: 48
                                leftPadding: 16
                                rightPadding: 16
                                onClicked: root.files.select(modelData.id)
                                onDoubleClicked: {
                                    if (modelData.kind === "folder") return root.files.openFolder(modelData.id)
                                    if (modelData.kind !== "file") return
                                    if ((modelData.lock || "").length > 0 && !modelData.unlocked) {
                                        root.files.select(modelData.id)
                                        return unlockDialog.openFor(root.files.selected)
                                    }
                                    root.files.openFile(modelData.id)
                                }
                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: {
                                        root.files.select(fileRow.modelData.id)
                                        if (fileRow.modelData.kind === "upload") return
                                        rowMenu.row = fileRow.modelData
                                        rowMenu.popup()
                                    }
                                }
                                background: Rectangle {
                                    color: root.files.selectedId === fileRow.modelData.id ? Theme.selected
                                         : fileRow.hovered ? Qt.alpha(Theme.selected, 0.5) : "transparent"
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
                                }
                                contentItem: RowLayout {
                                    spacing: 12
                                    readonly property bool lockedNow: (fileRow.modelData.lock || "").length > 0 && !fileRow.modelData.unlocked
                                    TypeTile { visible: !parent.lockedNow; label: fileRow.modelData.typeLabel; tint: fileRow.modelData.tint }
                                    LockTile { visible: parent.lockedNow }
                                    Text {
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: 1
                                        elide: Text.ElideMiddle
                                        text: fileRow.modelData.name
                                        textFormat: Text.PlainText
                                        color: Theme.text
                                        font.pixelSize: 14
                                    }
                                    Item {
                                        Layout.preferredWidth: 140
                                        implicitHeight: 24
                                        LockPill {
                                            anchors.verticalCenter: parent.verticalCenter
                                            lock: fileRow.modelData.lock || ""
                                            lockShort: fileRow.modelData.lockShort || ""
                                            unlocked: fileRow.modelData.unlocked === true
                                        }
                                    }
                                    Text {
                                        Layout.preferredWidth: 96
                                        text: fileRow.modelData.modified
                                        textFormat: Text.PlainText
                                        color: Theme.text2
                                        font.pixelSize: 13
                                    }
                                    Text {
                                        Layout.preferredWidth: 72
                                        text: fileRow.modelData.size
                                        textFormat: Text.PlainText
                                        color: Theme.text2
                                        font.family: Theme.monoFamily
                                        font.pixelSize: 12
                                    }
                                    ColumnLayout {
                                        Layout.preferredWidth: 112
                                        Layout.fillWidth: false
                                        spacing: 4
                                        RowLayout {
                                            spacing: 5
                                            Icon {
                                                visible: fileRow.modelData.status === "synced"
                                                name: fileRow.modelData.trashed ? "trash" : "check"
                                                size: 14
                                                color: fileRow.modelData.trashed ? Theme.text3 : Theme.accent
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                                text: fileRow.modelData.status === "synced" ? (fileRow.modelData.trashed ? "In Trash" : "Synced")
                                                    : fileRow.modelData.status === "encrypting" ? "Encrypting · " + Math.round(fileRow.modelData.progress * 100) + "%"
                                                    : fileRow.modelData.status === "uploading" ? "Uploading · " + Math.round(fileRow.modelData.progress * 100) + "%"
                                                    : fileRow.modelData.status === "queued" ? "Waiting…"
                                                    : fileRow.modelData.status === "paused" ? "Paused"
                                                    : "Didn't upload"
                                                textFormat: Text.PlainText
                                                color: fileRow.modelData.status === "synced" ? (fileRow.modelData.trashed ? Theme.text3 : Theme.accent)
                                                     : fileRow.modelData.status === "failed" ? Theme.danger : Theme.warn
                                                font.pixelSize: 13
                                                font.weight: Font.Medium
                                            }
                                        }
                                        Rectangle {
                                            visible: fileRow.modelData.status === "encrypting" || fileRow.modelData.status === "uploading"
                                            Layout.fillWidth: true
                                            implicitHeight: 4
                                            radius: 2
                                            color: Qt.alpha(Theme.text, 0.12)
                                            Rectangle {
                                                width: parent.width * fileRow.modelData.progress
                                                height: parent.height
                                                radius: 2
                                                color: Theme.warn
                                            }
                                        }
                                    }
                                }
                            }
                            Column {
                                anchors.centerIn: parent
                                width: parent.width - 80
                                visible: table.count === 0
                                spacing: 8
                                BusyIndicator {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    running: !root.files.loaded
                                    visible: running
                                }
                                Text {
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.Wrap
                                    visible: root.files.loaded
                                    text: root.files.search.length > 0 ? "Nothing matches that name."
                                        : root.files.section === "trash" ? "The Trash is empty."
                                        : root.files.section === "shared" ? "Files people share with you show up here."
                                        : root.files.section === "links" ? "Files you've made a share link for show up here."
                                        : root.files.section === "photos" ? "Your photos show up here."
                                        : root.files.section === "locked" ? "Files with an extra lock (a key or a password) show up here."
                                        : root.files.folders.length > 0 ? "No files here yet. Drop some in, or choose Upload."
                                        : "Drop files here or choose Upload. They're encrypted on this computer before they leave it."
                                    textFormat: Text.PlainText
                                    color: Theme.text3
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- Details ---
        Rectangle {
            id: details
            visible: root.files.section !== "keys" && root.files.section !== "links" && root.files.section !== "incoming"
            Layout.fillHeight: true
            Layout.preferredWidth: 340
            color: Theme.panel
            Rectangle { width: 1; height: parent.height; color: Theme.border }
            readonly property var s: root.files.selected
            readonly property bool lockedNow: s.locked === true && !s.unlocked
            readonly property bool ownFile: s.kind === "file" && !s.incoming && !s.uploading && !s.trashed

            Text {
                anchors.centerIn: parent
                width: parent.width - 48
                visible: details.s.id === undefined
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: "Select a file to see its details, who it's shared with and its share links."
                color: Theme.text3
            }

            Flickable {
                id: detailsFlick
                anchors.fill: parent
                anchors.margins: 20
                anchors.leftMargin: 21
                visible: details.s.id !== undefined
                clip: true
                contentWidth: width
                contentHeight: detailsColumn.height
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { parent: details; anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom }

            ColumnLayout {
                id: detailsColumn
                width: detailsFlick.width
                // At least the panel's height, so the buttons stay at the bottom when there's room.
                height: Math.max(detailsFlick.height, implicitHeight)
                spacing: 12

                // Preview (decrypted on this PC) or the type tile.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 150
                    radius: Theme.radiusCard
                    color: Theme.surface
                    clip: true
                    // Locked: nothing about the content until it's unlocked.
                    Rectangle {
                        anchors.fill: parent
                        visible: details.s.locked === true && !details.s.unlocked
                        z: 2
                        radius: Theme.radiusCard
                        color: Theme.lockSurface
                        border.color: Theme.lockBorder
                        Column {
                            anchors.centerIn: parent
                            spacing: 8
                            Icon { anchors.horizontalCenter: parent.horizontalCenter; name: "lock"; size: 26; color: Theme.lockText }
                            Text { text: "Locked · no preview until unlocked"; color: Theme.lockText; font.pixelSize: 13 }
                        }
                    }
                    Image {
                        anchors.fill: parent
                        visible: status === Image.Ready
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        source: details.s.previewUrl || ""
                    }
                    Column {
                        anchors.centerIn: parent
                        spacing: 6
                        visible: !(details.s.previewUrl)
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: details.s.typeLabel || ""
                            textFormat: Text.PlainText
                            color: details.s.tint || Theme.text3
                            font.family: Theme.monoFamily
                            font.pixelSize: 20
                            font.weight: Font.Medium
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: details.s.isImage ? "[decrypting a preview…]" : "[no preview for this type]"
                            textFormat: Text.PlainText
                            color: Theme.text3
                            font.family: Theme.monoFamily
                            font.pixelSize: 11
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WrapAnywhere
                        text: details.s.name || ""
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: details.s.kind === "folder" ? "Folder" : (details.s.sizeText || "") + " · modified " + (details.s.modifiedText || "")
                              + " · " + (details.s.sharedText || "only you")
                              + (details.s.versions > 1 ? " · " + details.s.versions + " versions" : "")
                        textFormat: Text.PlainText
                        color: Theme.text2
                        font.pixelSize: 13
                    }
                    RowLayout {
                        visible: details.s.kind === "file" && !details.s.uploading
                        spacing: 6
                        Icon { name: "shield-check"; size: 14; color: Theme.accent }
                        Text { text: "Encrypted on this PC · hash checked on download"; color: Theme.accent; font.pixelSize: 12 }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        wrapMode: Text.Wrap
                        text: details.s.statusText || ""
                        textFormat: Text.PlainText
                        color: details.s.failed ? Theme.danger : Theme.warn
                        font.pixelSize: 13
                    }
                    // An upload in progress, waiting or paused: stop it (a resumable one is dropped on the server too).
                    RvButton {
                        visible: details.s.kind === "upload" && details.s.uploading === true
                        secondary: true
                        iconName: "close"
                        text: "Cancel upload"
                        onClicked: root.files.cancelUpload(details.s.id)
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    visible: details.s.incoming === true || details.s.kind === "folder"
                    implicitHeight: encText.implicitHeight + 24
                    radius: Theme.radiusControl
                    color: Theme.accentSoft
                    border.color: Theme.accentBorder
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 10
                        Icon { Layout.alignment: Qt.AlignTop; name: "lock"; size: 16; color: Theme.accent }
                        Text {
                            id: encText
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: details.s.incoming
                                  ? "Shared with you by " + details.s.owner + ". Encrypted end to end: only the people it's shared with hold the key."
                                  : "Encrypted on this PC before upload. Only you and the people you share with hold the key. We can't open it."
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: 13
                        }
                    }
                }

                // Extra lock: locked with a key / a password, unlocked for this session, or none.
                Rectangle {
                    Layout.fillWidth: true
                    visible: details.ownFile
                    implicitHeight: lockColumn.implicitHeight + 32
                    radius: Theme.radiusCard
                    color: details.s.unlocked ? Theme.accentSoft : details.s.locked ? Theme.lockSurface : Theme.surface
                    border.color: details.s.unlocked ? Theme.accentBorder : details.s.locked ? Theme.lockBorder : Theme.border
                    ColumnLayout {
                        id: lockColumn
                        anchors.fill: parent
                        anchors.margins: 15
                        spacing: 10
                        Text {
                            text: details.s.unlocked ? "Unlocked for this session"
                                  : details.s.lockKind === "key" ? "Locked with a key"
                                  : details.s.lockKind === "password" ? "Locked with a password" : "No extra lock"
                            textFormat: Text.PlainText
                            color: details.s.unlocked ? Theme.accent : details.s.locked ? Theme.lockText : Theme.text
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                        // The key (locked with a key).
                        Rectangle {
                            Layout.fillWidth: true
                            visible: details.lockedNow && details.s.lockKind === "key"
                            implicitHeight: 54
                            radius: 12
                            color: Theme.lockBg
                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 10
                                Icon { name: "key"; size: 16; color: Theme.lockText }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: details.s.lockKeyName || ""; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold; elide: Text.ElideRight }
                                    Text { textFormat: Text.PlainText; text: details.s.lockFingerprint || ""; color: Theme.lockText; font.family: Theme.monoFamily; font.pixelSize: 11 }
                                }
                            }
                        }
                        // Unlocked: which key, if any, and that it locks again.
                        RowLayout {
                            visible: details.s.unlocked === true && details.s.lockKind === "key"
                            spacing: 8
                            Icon { name: "key"; size: 14; color: Theme.text2 }
                            Text { textFormat: Text.PlainText; text: details.s.lockKeyName || ""; color: Theme.text; font.pixelSize: 13 }
                            Text { textFormat: Text.PlainText; text: details.s.lockFingerprint || ""; color: Theme.accent; font.family: Theme.monoFamily; font.pixelSize: 11 }
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: details.s.unlocked
                                  ? "The file's key is never saved back to the server unlocked, even if you rename or move it. It locks again when you quit the app."
                                  : details.s.lockKind === "key"
                                    ? (details.s.lockKeyAvailable
                                       ? (details.s.lockKeyHasPassword ? "Opening it needs your account plus this key and its password." : "Opening it needs your account plus this key.")
                                       : "This key isn't among your encryption keys. Import it to open the file.")
                                  : details.s.lockKind === "password" ? "Opening it needs your account plus this file's own password."
                                  : "Your account protects this file. Add a key or a password if it needs more."
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: 13
                            lineHeight: 1.15
                        }
                        RvButton {
                            Layout.fillWidth: true
                            visible: details.lockedNow
                            implicitHeight: 42
                            text: "Unlock for this session"
                            onClicked: unlockDialog.openFor(root.files.selected)
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: details.s.locked === true
                            spacing: 8
                            RvButton {
                                Layout.fillWidth: true
                                visible: details.s.unlocked === true
                                secondary: true
                                implicitHeight: 38
                                text: "Lock now"
                                onClicked: root.files.lockNow(root.files.selected.id)
                            }
                            RvButton {
                                Layout.fillWidth: true
                                visible: !details.s.unlocked
                                secondary: true
                                implicitHeight: 38
                                text: "Change lock"
                                onClicked: unlockDialog.openFor(root.files.selected, "changeLock")
                            }
                            RvButton {
                                Layout.fillWidth: true
                                visible: !details.s.unlocked
                                secondary: true
                                implicitHeight: 38
                                text: "Remove lock"
                                onClicked: unlockDialog.openFor(root.files.selected, "removeLock")
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: details.s.unlocked === true
                            spacing: 8
                            RvButton {
                                Layout.fillWidth: true
                                primary: false
                                implicitHeight: 34
                                text: "Change or remove the lock"
                                onClicked: uploadLock.openForChange(root.files.selected.id, root.files.selected.name, root.files.selected.lockKind, root.files.selected.lockKeyId)
                            }
                        }
                        RvButton {
                            Layout.fillWidth: true
                            visible: !details.s.locked
                            secondary: true
                            implicitHeight: 40
                            iconName: "lock"
                            text: "Add a lock"
                            onClicked: uploadLock.openForChange(root.files.selected.id, root.files.selected.name, "", "")
                        }
                    }
                }
                InfoCard {
                    Layout.fillWidth: true
                    visible: details.ownFile && details.lockedNow
                    text: "Unlock this file to download it, share it or create a link."
                }
                InfoCard {
                    Layout.fillWidth: true
                    visible: details.ownFile && details.s.unlocked === true
                    tone: "warn"
                    text: "Sharing this file gives the other person a copy they can open without your " + (details.s.lockKind === "password" ? "password." : "key.")
                }

                // Shared with
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: details.ownFile && !details.lockedNow
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: "SHARED WITH"
                            color: Theme.text3
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.8
                        }
                        RvButton {
                            primary: false
                            implicitHeight: 28
                            text: "Add"
                            iconName: "user-plus"
                            onClicked: {
                                sharePicker.targetId = root.files.selected.id
                                sharePicker.open()
                            }
                        }
                    }
                    Text {
                        visible: (root.files.selected.sharedWith || []).length === 0
                        text: "Only you"
                        color: Theme.text2
                    }
                    Repeater {
                        model: root.files.selected.sharedWith || []
                        delegate: RowLayout {
                            id: person
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 10
                            Avatar { size: 30; initial: person.modelData.initial; tintKey: person.modelData.userId }
                            Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: person.modelData.handle; color: Theme.text; elide: Text.ElideRight }
                            Text { textFormat: Text.PlainText; text: person.modelData.role; color: Theme.text3; font.pixelSize: 12 }
                            IconButton {
                                implicitWidth: 28
                                implicitHeight: 28
                                iconSize: 14
                                iconName: "close"
                                tip: "Stop sharing with " + person.modelData.handle
                                onClicked: root.files.unshare(root.files.selected.id, person.modelData.userId)
                            }
                        }
                    }
                }

                // Share link settings, and existing links.
                Rectangle {
                    Layout.fillWidth: true
                    visible: details.ownFile && !details.lockedNow && (root.files.selected.links || []).length > 0
                    implicitHeight: linkColumn.implicitHeight + 24
                    radius: Theme.radiusCard
                    color: Theme.surface
                    ColumnLayout {
                        id: linkColumn
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 4
                        Text { text: "Older file links"; color: Theme.text; font.weight: Font.DemiBold }
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: "Made before Shared links. They still work until you remove them; new links are under Share."
                            color: Theme.text3
                            font.pixelSize: 12
                        }
                        Repeater {
                            model: root.files.selected.links || []
                            delegate: RowLayout {
                                id: link
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                spacing: 6
                                Icon { name: "link"; size: 14; color: Theme.accent }
                                Text {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: link.modelData.expires + (link.modelData.hasPassword ? " · password" : "") + (link.modelData.allowDownload ? " · download" : " · view only")
                                    textFormat: Text.PlainText
                                    color: Theme.text2
                                    font.pixelSize: 12
                                }
                                IconButton {
                                    visible: link.modelData.canCopy
                                    implicitWidth: 28; implicitHeight: 28; iconSize: 14
                                    iconName: "copy"
                                    tip: "Copy link"
                                    onClicked: root.files.copyLink(root.files.selected.id, link.modelData.id)
                                }
                                IconButton {
                                    implicitWidth: 28; implicitHeight: 28; iconSize: 14
                                    iconName: "trash"
                                    tip: "Remove link (it stops working)"
                                    onClicked: root.files.deleteLink(root.files.selected.id, link.modelData.id)
                                }
                            }
                        }
                    }
                }

                // PC backup controls, on its folder.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: details.s.kind === "folder" && root.files.pcBackupPath.length > 0 && details.s.name === "PC backup"
                    spacing: 6
                    Text { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; text: "Backing up " + root.files.pcBackupPath; color: Theme.text2; font.pixelSize: 12 }
                    RowLayout {
                        RvButton { secondary: true; text: "Sync now"; iconName: "refresh"; onClicked: root.files.syncPcBackupNow() }
                        RvButton { primary: false; danger: true; text: "Stop"; onClicked: root.files.stopPcBackup() }
                    }
                }

                Item { Layout.fillHeight: true }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    visible: details.s.kind === "file"
                    RvButton {
                        Layout.fillWidth: true
                        secondary: true
                        iconName: "download"
                        enabled: !details.lockedNow
                        ToolTip.visible: hovered && details.lockedNow
                        ToolTip.text: "Unlock this file to download it."
                        Accessible.description: details.lockedNow ? "Unavailable: unlock this file to download it." : ""
                        text: "Download"
                        onClicked: {
                            saveDialog.targetId = root.files.selected.id
                            saveDialog.selectedFile = saveDialog.currentFolder + "/" + encodeURIComponent(root.files.selected.name)
                            App.lock.expectExternalReturn()
                            saveDialog.open()
                        }
                    }
                    RvButton {
                        Layout.fillWidth: true
                        visible: !root.files.selected.incoming && !root.files.selected.trashed
                        enabled: !details.lockedNow
                        ToolTip.visible: hovered && details.lockedNow
                        ToolTip.text: "Unlock this file to share it."
                        Accessible.description: details.lockedNow ? "Unavailable: unlock this file to share it." : ""
                        iconName: "share"
                        text: "Share"
                        onClicked: shareLinkDialog.openFor(root.files.selected.id, root.files.selected.name, false)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    visible: details.s.kind === "folder"
                    RvButton { Layout.fillWidth: true; secondary: true; text: "Open"; onClicked: root.files.openFolder(root.files.selected.id) }
                    RvButton {
                        Layout.fillWidth: true
                        visible: root.shares !== null && !root.files.selected.trashed
                        iconName: "share"
                        text: "Share"
                        onClicked: shareLinkDialog.openFor(root.files.selected.id, root.files.selected.name, true)
                    }
                }
            }
            }
        }
    }
}
