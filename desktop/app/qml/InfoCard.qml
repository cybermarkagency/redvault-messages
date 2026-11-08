// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Layouts
import Redvault

// A callout with an icon: "ok" (accent), "error" (danger), "warn", or
// "neutral". Children placed inside go under the text (e.g. a button).
Rectangle {
    id: card
    property string tone: "neutral"
    property string iconName: tone === "ok" ? "check" : tone === "error" ? "alert-circle" : tone === "warn" ? "triangle-alert" : "info"
    property string text
    property string detail
    property bool rich: false
    default property alias extra: extraColumn.data

    readonly property color ink: tone === "ok" ? Theme.accent : tone === "error" ? Theme.danger : tone === "warn" ? Theme.warn : Theme.text3

    implicitHeight: row.implicitHeight + 28
    radius: 14
    color: tone === "ok" ? Theme.accentSoft : tone === "error" ? Theme.dangerSoft : tone === "warn" ? Theme.warnSoft : Theme.surface
    border.color: tone === "ok" ? Theme.accentBorder : tone === "error" ? "#5A2D28" : tone === "warn" ? "#4A3F28" : Theme.border

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 14
        spacing: 12
        Icon { Layout.alignment: Qt.AlignTop; Layout.topMargin: 1; name: card.iconName; size: 18; color: card.ink }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3
            Text {
                Layout.fillWidth: true
                visible: card.text.length > 0
                text: card.text
                textFormat: card.rich ? Text.StyledText : Text.PlainText
                wrapMode: Text.Wrap
                color: card.tone === "neutral" ? Theme.text2 : card.ink
                font.pixelSize: 13
                font.weight: card.tone === "ok" ? Font.DemiBold : Font.Normal
                lineHeight: 1.15
            }
            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: card.detail
                textFormat: Text.StyledText
                wrapMode: Text.Wrap
                color: Theme.text
                font.pixelSize: 12
            }
            ColumnLayout {
                id: extraColumn
                Layout.fillWidth: true
                visible: children.length > 0
            }
        }
    }
}
