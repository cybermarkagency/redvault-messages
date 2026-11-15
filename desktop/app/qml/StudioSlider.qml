// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A labelled slider for the story studio: "Brightness  +12".
ColumnLayout {
    id: row
    property string label
    property alias from: slider.from
    property alias to: slider.to
    property alias value: slider.value
    property alias stepSize: slider.stepSize
    readonly property alias pressed: slider.pressed
    /// What the number beside the label says (defaults to the rounded value).
    property string valueText: (slider.value > 0 && slider.from < 0 ? "+" : "") + Math.round(slider.value)
    signal moved(real value)
    signal began()
    signal ended()
    spacing: 2
    RowLayout {
        Layout.fillWidth: true
        Text { Layout.fillWidth: true; text: row.label; textFormat: Text.PlainText; color: Theme.text2; font.pixelSize: Theme.fontSmall }
        Text { text: row.valueText; textFormat: Text.PlainText; color: Theme.text; font.family: Theme.monoFamily; font.pixelSize: Theme.fontSmall }
    }
    Slider {
        id: slider
        Layout.fillWidth: true
        Accessible.name: row.label
        onMoved: row.moved(slider.value)
        onPressedChanged: slider.pressed ? row.began() : row.ended()
        background: Rectangle {
            x: slider.leftPadding
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: slider.availableWidth
            height: 4
            radius: 2
            color: Qt.alpha(Theme.text, 0.14)
            Rectangle { width: slider.visualPosition * parent.width; height: parent.height; radius: 2; color: Theme.accent }
        }
        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: 16
            height: 16
            radius: 8
            color: Theme.text
        }
    }
}
