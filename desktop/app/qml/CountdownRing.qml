// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick

// The one-time code's 36px countdown ring, with the seconds left inside.
Item {
    id: ring
    property int seconds: 0
    property int period: 30
    implicitWidth: 36
    implicitHeight: 36
    Accessible.name: seconds + " seconds left"

    onSecondsChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const r = width / 2 - 2
            ctx.lineWidth = 3
            ctx.strokeStyle = Qt.alpha(Theme.text, 0.14)
            ctx.beginPath()
            ctx.arc(width / 2, height / 2, r, 0, 2 * Math.PI)
            ctx.stroke()
            ctx.strokeStyle = ring.seconds <= 5 ? Theme.warn : Theme.accent
            ctx.beginPath()
            const start = -Math.PI / 2
            ctx.arc(width / 2, height / 2, r, start, start + 2 * Math.PI * (ring.seconds / Math.max(1, ring.period)))
            ctx.stroke()
        }
    }
    Text {
        anchors.centerIn: parent
        text: ring.seconds
        textFormat: Text.PlainText
        color: Theme.text
        font.family: Theme.monoFamily
        font.pixelSize: 11
    }
}
