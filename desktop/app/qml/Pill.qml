// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick

// A small status label: "ok" (confirmed: Verified, connected), "accent",
// "warn", "danger", "lock" or "plain", with an optional Lucide icon so the
// state never rests on its colour alone.
Rectangle {
    id: pill
    property string text
    property string tone: "plain"
    property string iconName: ""

    readonly property color ink: tone === "ok" ? Theme.ok : tone === "accent" ? Theme.accent : tone === "warn" ? Theme.warn
                               : tone === "danger" ? Theme.danger : tone === "lock" ? Theme.lockText : Theme.text2

    implicitWidth: row.implicitWidth + 18
    implicitHeight: 22
    radius: 11
    color: tone === "ok" ? Theme.okSoft : tone === "accent" ? Theme.accentSoft : tone === "warn" ? Theme.warnSoft
         : tone === "danger" ? Theme.dangerSoft : tone === "lock" ? Theme.lockBg : Theme.surface2
    border.color: tone === "ok" ? Theme.okBorder : tone === "accent" ? Theme.accentBorder : tone === "warn" ? Theme.warnBorder
                : tone === "danger" ? Theme.dangerBorder : tone === "lock" ? Theme.lockBorder : Theme.border
    Accessible.role: Accessible.StaticText
    Accessible.name: text

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 4
        Icon {
            anchors.verticalCenter: parent.verticalCenter
            visible: pill.iconName.length > 0
            name: pill.iconName
            size: 12
            color: pill.ink
        }
        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            text: pill.text
            textFormat: Text.PlainText
            color: pill.ink
            font.pixelSize: 12
            font.weight: Font.Medium
        }
    }
}
