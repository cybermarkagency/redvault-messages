// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

// A microphone, speaker or camera picker. `devices` is [{id, label}]; an
// empty `currentId` means the system default.
ComboBox {
    id: box
    property var devices: []
    property string currentId: ""
    property string iconName: "mic"
    property string defaultLabel: "System default"
    signal picked(string id)

    readonly property var options: [{ id: "", label: defaultLabel }].concat(devices)

    model: options
    textRole: "label"
    valueRole: "id"
    currentIndex: Math.max(0, options.findIndex(d => d.id === currentId))
    onActivated: (index) => picked(options[index].id)
    implicitHeight: 40
    implicitWidth: 240
    Accessible.name: defaultLabel

    background: Rectangle {
        radius: Theme.radiusSmall
        color: Theme.surface2
        border.color: box.activeFocus ? Theme.accentBorder : Theme.borderStrong
    }
    contentItem: Row {
        leftPadding: 12
        spacing: 8
        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: box.iconName
            size: 16
            color: Theme.text3
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: box.width - 70
            text: box.displayText
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.text2
            font.pixelSize: 13
        }
    }
    popup.background: Rectangle {
        radius: Theme.radiusSmall
        color: Theme.surface
        border.color: Theme.borderStrong
    }
    delegate: ItemDelegate {
        id: item
        required property var modelData
        required property int index
        width: box.width
        highlighted: box.highlightedIndex === item.index
        contentItem: Text {
            text: item.modelData.label
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: 13
        }
        background: Rectangle {
            color: item.highlighted ? Theme.selected : "transparent"
        }
    }
}
