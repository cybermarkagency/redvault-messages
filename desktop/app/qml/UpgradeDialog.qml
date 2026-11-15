// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// "<Feature> is part of Pro/Premium", with what that plan adds and a way to the
// plans. Opened by the shell when a screen calls PlanGate.ask().
RvDialog {
    id: dialog
    property string tier: "pro"
    property string feature: ""

    readonly property var adds: tier === "premium"
        ? ["Video call links", "Start group voice and video calls", "Screen sharing in calls",
           "Create and post in your own channels", "100 GB of Files storage", "Everything in Pro"]
        : ["Send files, photos and pasted images in chats", "Voice call links", "50 GB of Files storage"]

    signal seePlans()

    function show(tierId, what) {
        tier = tierId === "premium" ? "premium" : "pro"
        feature = what
        open()
    }

    title: (feature.length > 0 ? feature : qsTr("This")) + qsTr(" is part of ") + PlanGate.tierName(tier)

    ColumnLayout {
        width: parent.width
        spacing: 12

        RowLayout {
            spacing: 10
            Rectangle {
                width: 36; height: 36; radius: 18
                color: Theme.accentSoft
                border.color: Theme.accentBorder
                Icon { anchors.centerIn: parent; name: "lock"; size: 18; color: Theme.accent }
            }
            Text {
                Layout.fillWidth: true
                text: dialog.tier === "premium"
                      ? qsTr("Premium adds:") : qsTr("Pro adds:")
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontSize
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }
        }

        Repeater {
            model: dialog.adds
            RowLayout {
                required property string modelData
                Layout.fillWidth: true
                spacing: 8
                Icon { name: "check"; size: 16; color: Theme.accent }
                Text {
                    Layout.fillWidth: true
                    text: modelData
                    textFormat: Text.PlainText
                    color: Theme.text2
                    font.pixelSize: Theme.fontSize
                    wrapMode: Text.WordWrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 8
            Item { Layout.fillWidth: true }
            RvButton { primary: false; text: qsTr("Not now"); onClicked: dialog.close() }
            RvButton {
                text: qsTr("See plans")
                onClicked: { dialog.close(); dialog.seePlans() }
            }
        }
    }
}
