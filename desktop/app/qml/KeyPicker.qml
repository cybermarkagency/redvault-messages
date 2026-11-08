// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Chooses one of the person's encryption keys: name and fingerprint, in
// the lock colours.
ComboBox {
    id: picker
    required property FilesSession files
    /// The chosen key's id (vault item id).
    property string keyId
    readonly property var current: {
        const list = files.keys
        for (let i = 0; i < list.length; i++) if (list[i].id === keyId) return list[i]
        return null
    }

    model: files.keys
    textRole: "name"
    valueRole: "id"
    implicitHeight: 48
    Accessible.name: "Key"
    currentIndex: {
        const list = files.keys
        for (let i = 0; i < list.length; i++) if (list[i].id === keyId) return i
        return -1
    }
    onActivated: (index) => keyId = files.keys[index].id

    background: Rectangle {
        radius: 12
        color: Theme.lockSurface
        border.color: picker.activeFocus ? Theme.lockText : Theme.lockBorder
    }
    contentItem: RowLayout {
        spacing: 10
        Item { implicitWidth: 4 }
        Icon { name: "key"; size: 16; color: Theme.lockText }
        Text {
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: picker.current ? picker.current.name : picker.files.keys.length === 0 ? "No keys yet: create or import one" : "Choose a key"
            textFormat: Text.PlainText
            color: picker.current ? Theme.text : Theme.text3
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }
        Text {
            visible: picker.current !== null
            text: picker.current ? picker.current.fingerprint : ""
            textFormat: Text.PlainText
            color: Theme.lockText
            font.family: Theme.monoFamily
            font.pixelSize: 12
        }
        Item { implicitWidth: 30 }
    }
    indicator: Icon {
        x: picker.width - width - 14
        y: (picker.height - height) / 2
        name: "chevron-down"
        size: 16
        color: Theme.lockText
    }
    delegate: ItemDelegate {
        id: option
        required property var modelData
        required property int index
        width: ListView.view ? ListView.view.width : picker.width
        highlighted: picker.highlightedIndex === index
        background: Rectangle { color: option.highlighted ? Theme.selected : "transparent" }
        contentItem: RowLayout {
            spacing: 10
            Icon { name: "key"; size: 14; color: Theme.lockText }
            Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: option.modelData.name; color: Theme.text; elide: Text.ElideRight }
            Text { textFormat: Text.PlainText; text: option.modelData.fingerprint; color: Theme.text3; font.family: Theme.monoFamily; font.pixelSize: 12 }
        }
    }
    popup.background: Rectangle { radius: 12; color: Theme.surface; border.color: Theme.borderStrong }
}
