// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic

// A filter chip / segmented option: raised when off, light when on.
AbstractButton {
    id: control
    checkable: false
    hoverEnabled: true
    implicitHeight: 32
    /// Part of a plan the user doesn't have: a small lock after the text.
    property bool locked: false
    implicitWidth: label.implicitWidth + (locked ? 18 : 0) + 28
    Accessible.name: locked ? text + qsTr(" (locked)") : text

    background: Rectangle {
        radius: 16
        color: control.checked ? Theme.toggleOnBg : (control.hovered ? Theme.surface3 : Theme.surface2)
    }
    contentItem: Item {
        Row {
            anchors.centerIn: parent
            spacing: 4
            Text {
                textFormat: Text.PlainText
                id: label
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                color: control.checked ? Theme.toggleOnFg : Theme.text2
                font.pixelSize: 13
                font.weight: Font.Medium
            }
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.locked
                name: "lock"
                size: 12
                color: control.checked ? Theme.toggleOnFg : Theme.text3
            }
        }
    }
}
