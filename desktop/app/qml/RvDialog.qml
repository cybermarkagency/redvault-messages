// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A modal card in the app's tokens, with a title and a close button.
Dialog {
    id: dialog
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 480
    padding: 22
    topPadding: 6
    closePolicy: Popup.CloseOnEscape

    Overlay.modal: Rectangle {
        color: Theme.dark ? "#B0000000" : "#66101817"
    }

    background: Rectangle {
        radius: Theme.radiusDialog
        color: Theme.surface
        border.color: Theme.border
    }

    header: RowLayout {
        spacing: 8
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 22
            Layout.topMargin: 18
            text: dialog.title
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
        }
        RvButton {
            Layout.rightMargin: 10
            Layout.topMargin: 12
            primary: false
            iconName: "x"
            ToolTip.visible: hovered
            ToolTip.text: "Close"
            implicitHeight: 32
            leftPadding: 10
            rightPadding: 10
            onClicked: dialog.close()
        }
    }
}
