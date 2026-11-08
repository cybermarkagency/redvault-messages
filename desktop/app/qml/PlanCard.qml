// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// One plan in Settings' plan chooser (AccountSession.planChoices): its name,
// price, what it includes, where you stand, and the button that changes it.
Rectangle {
    id: card
    /// A planChoices entry: tier, name, price, per, includes, current, status, action, actionLabel, confirm, note.
    property var choice: ({})
    property bool busy: false
    signal chosen()

    readonly property bool current: card.choice.current === true

    implicitWidth: 210
    implicitHeight: content.implicitHeight + 36
    radius: Theme.radiusCard
    color: card.current ? Theme.accentSoft : Theme.surface
    border.color: card.current ? Theme.accent : Theme.border
    border.width: card.current ? 2 : 1

    Accessible.role: Accessible.Grouping
    Accessible.name: card.choice.name + (card.current ? ", your plan" : "")

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 18
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: card.choice.name
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            Rectangle {
                visible: card.current
                radius: height / 2
                color: Theme.accent
                implicitHeight: badge.implicitHeight + 6
                implicitWidth: badge.implicitWidth + 16
                Text {
                    id: badge
                    anchors.centerIn: parent
                    text: "Current"
                    color: Theme.accentContrast
                    font.pixelSize: Theme.fontSmall
                    font.weight: Font.DemiBold
                }
            }
        }

        RowLayout {
            spacing: 6
            Text {
                text: card.choice.price
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: 30
                font.weight: Font.Bold
            }
            Text {
                Layout.alignment: Qt.AlignBottom
                Layout.bottomMargin: 6
                visible: text.length > 0
                text: card.choice.per
                textFormat: Text.PlainText
                color: Theme.text2
                font.pixelSize: Theme.fontSmall
            }
        }

        Text {
            Layout.fillWidth: true
            visible: text.length > 0
            text: card.choice.status
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: card.current ? Theme.accent : Theme.text2
            font.pixelSize: Theme.fontSmall
            font.weight: card.current ? Font.DemiBold : Font.Normal
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }

        Repeater {
            model: card.choice.includes
            delegate: RowLayout {
                required property string modelData
                Layout.fillWidth: true
                spacing: 8
                Icon {
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 2
                    name: "check"
                    size: 14
                    color: Theme.accent
                }
                Text {
                    Layout.fillWidth: true
                    text: modelData
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall + 1
                }
            }
        }

        // Keeps the buttons of side-by-side cards on one line.
        Item {
            Layout.fillHeight: true
            implicitHeight: 4
        }

        RvButton {
            Layout.fillWidth: true
            visible: (card.choice.actionLabel || "").length > 0
            text: card.choice.actionLabel || ""
            primary: card.choice.action === "checkout" || card.choice.action === "switch" || card.choice.action === "keep"
            danger: card.choice.action === "cancel"
            enabled: !card.busy
            onClicked: card.chosen()
        }
        Text {
            Layout.fillWidth: true
            visible: text.length > 0
            text: card.choice.note || ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.text3
            font.pixelSize: Theme.fontSmall
        }
    }
}
