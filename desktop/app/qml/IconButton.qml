// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic

// A 40px icon-only button with a tooltip (its accessible name). `toggled`
// shows the design's "on" state (light fill, dark icon); `danger` is the red
// end/decline button; `round` for call controls.
AbstractButton {
    id: control
    property string iconName
    property string tip
    property int iconSize: 20
    property int badge: 0
    property bool toggled: false
    property bool danger: false
    property bool round: false
    property bool filled: false
    /// Part of a plan the user doesn't have: a small lock in the corner. The
    /// button stays clickable so the click can explain the plan (PlanGate.ask).
    property bool locked: false
    property color iconColor: Theme.text2

    implicitWidth: 40
    implicitHeight: 40
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.role: Accessible.Button
    Accessible.name: tip
    Accessible.checkable: toggled
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.text: tip
    ToolTip.delay: 400

    background: Rectangle {
        radius: control.round ? width / 2 : Theme.radiusSmall
        color: control.danger ? Theme.dangerStrong
             : control.toggled ? Theme.toggleOnBg
             : control.filled ? (control.hovered ? Theme.borderStrong : Theme.surface3)
             : control.hovered ? Theme.surface2 : "transparent"
        opacity: control.enabled ? (control.down ? 0.8 : 1) : 0.4

        // Keyboard focus ring.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: parent.radius + 2
            color: "transparent"
            border.width: 2
            border.color: Theme.focusRing
            visible: control.visualFocus
        }
    }

    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            name: control.iconName
            size: control.iconSize
            color: control.danger ? "#FFFFFF" : control.toggled ? Theme.toggleOnFg : control.iconColor
            opacity: control.enabled ? (control.locked ? 0.55 : 1) : 0.5
        }
        Rectangle {
            visible: control.locked && control.badge === 0
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 3
            anchors.bottomMargin: 3
            width: 15
            height: 15
            radius: 8
            color: Theme.lockBg
            border.color: Theme.lockBorder
            Icon { anchors.centerIn: parent; name: "lock"; size: 9; color: Theme.lockText }
        }
        Rectangle {
            visible: control.badge > 0
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: 2
            anchors.topMargin: 2
            width: 16
            height: 16
            radius: 8
            color: Theme.accentFill
            Text {
                anchors.centerIn: parent
                text: control.badge > 9 ? "9+" : control.badge
                textFormat: Text.PlainText
                color: Theme.accentContrast
                font.pixelSize: 9
                font.weight: Font.Bold
            }
        }
    }
}
