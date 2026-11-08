// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Layouts
import Redvault

// Right after creating an account: the recovery key, shown once.
Item {
    Rectangle {
        anchors.centerIn: parent
        width: 460
        height: column.implicitHeight + 64
        radius: Theme.radiusCard
        color: Theme.surface
        border.color: Theme.border

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: 32
            spacing: 12

            Text {
                text: "Your recovery key"
                color: Theme.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }

            RecoveryKeyPanel {
                Layout.fillWidth: true
                recoveryKey: App.recoveryKey
                onFinished: App.acknowledgeRecoveryKey()
            }
        }
    }
}
