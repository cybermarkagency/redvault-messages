// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

// A drop-down in the app's tokens over [{value, label}] options. `value` is
// the chosen option's value; setting it picks that option.
ComboBox {
    id: box
    property var options: []
    property var value: ""
    signal picked(var value)

    model: box.options
    textRole: "label"
    valueRole: "value"
    currentIndex: Math.max(0, box.options.findIndex(o => o.value === box.value))
    onActivated: (index) => {
        box.value = box.options[index].value
        box.picked(box.value)
    }
    implicitHeight: 40
    implicitWidth: 220

    background: Rectangle {
        radius: Theme.radiusControl
        color: Theme.surface
        border.width: box.activeFocus ? 2 : 1
        border.color: box.activeFocus ? Theme.accent : Theme.border
    }
    contentItem: Text {
        leftPadding: 12
        rightPadding: box.indicator.width + 8
        text: box.displayText
        textFormat: Text.PlainText
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        color: Theme.text
        font.pixelSize: Theme.fontSize
    }
    indicator: Icon {
        x: box.width - width - 12
        y: (box.height - height) / 2
        name: "chevron-down"
        size: 16
        color: Theme.text3
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
