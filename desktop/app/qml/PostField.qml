// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic

// A multi-line text box in the app's tokens (a channel post, a description,
// a report). TextArea has no length limit of its own: past `limit` the border
// turns red, and the form keeps its button off.
TextArea {
    id: control
    property int limit: 0
    readonly property bool tooLong: limit > 0 && text.length > limit

    wrapMode: TextArea.Wrap
    textFormat: TextEdit.PlainText
    color: Theme.text
    placeholderTextColor: Theme.text3
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentContrast
    font.pixelSize: Theme.fontSize
    padding: 12
    implicitHeight: Math.max(84, contentHeight + topPadding + bottomPadding)

    background: Rectangle {
        radius: Theme.radiusControl
        color: Theme.surface
        border.width: control.activeFocus || control.tooLong ? 2 : 1
        border.color: control.tooLong ? Theme.danger : control.activeFocus ? Theme.accent : Theme.border
    }
}
