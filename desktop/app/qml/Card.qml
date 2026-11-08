// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Layouts

// A settings group / card: surface, strong border, 16px corners. Children
// go in a column with 16px padding.
Rectangle {
    id: card
    default property alias content: column.data
    property int padding: 16
    property alias spacing: column.spacing

    implicitHeight: column.implicitHeight + padding * 2
    radius: Theme.radiusCard
    color: Theme.surface
    border.color: Theme.borderStrong

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: card.padding
        spacing: 12
    }
}
