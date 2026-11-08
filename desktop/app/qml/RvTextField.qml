// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: control
    implicitHeight: 40
    leftPadding: 12
    rightPadding: 12
    font.pixelSize: Theme.fontSize
    color: Theme.text
    placeholderTextColor: Theme.text3
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentContrast

    background: Rectangle {
        radius: Theme.radiusControl
        color: Theme.surface
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.accent : Theme.border
    }
}
