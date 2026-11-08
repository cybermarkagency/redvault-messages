// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// "Post at": a local date and time (up to a year ahead), as the web's
// datetime-local field. `iso` is the moment it means, or empty while what's
// typed isn't a date and time in the future.
RowLayout {
    id: field
    property string label: "Post at"
    readonly property string iso: {
        const d = /^(\d{4})-(\d{1,2})-(\d{1,2})$/.exec(dateField.text.trim())
        const t = /^(\d{1,2}):(\d{2})$/.exec(timeField.text.trim())
        if (!d || !t) return ""
        const at = new Date(Number(d[1]), Number(d[2]) - 1, Number(d[3]), Number(t[1]), Number(t[2]))
        if (isNaN(at.getTime()) || at.getMonth() !== Number(d[2]) - 1 || at.getTime() <= Date.now()) return ""
        return at.toISOString()
    }
    readonly property bool filled: dateField.text.trim().length > 0 || timeField.text.trim().length > 0
    spacing: 8

    /// Starts with the next whole hour, an hour from now.
    function reset() {
        const at = new Date(Date.now() + 60 * 60 * 1000)
        at.setMinutes(0, 0, 0)
        const pad = (n) => (n < 10 ? "0" : "") + n
        dateField.text = at.getFullYear() + "-" + pad(at.getMonth() + 1) + "-" + pad(at.getDate())
        timeField.text = pad(at.getHours()) + ":" + pad(at.getMinutes())
    }
    function clear() {
        dateField.text = ""
        timeField.text = ""
    }

    Icon {
        name: "clock"
        size: 16
        color: Theme.text2
    }
    Text {
        text: field.label
        textFormat: Text.PlainText
        color: Theme.text2
    }
    RvTextField {
        id: dateField
        Layout.preferredWidth: 130
        placeholderText: "YYYY-MM-DD"
        Accessible.name: field.label + ", date"
    }
    RvTextField {
        id: timeField
        Layout.preferredWidth: 80
        placeholderText: "HH:MM"
        Accessible.name: field.label + ", time"
    }
    Text {
        Layout.fillWidth: true
        visible: field.filled && field.iso.length === 0
        wrapMode: Text.Wrap
        text: "A date and time from now on, like 2026-12-24 and 18:30."
        color: Theme.warn
        font.pixelSize: Theme.fontSmall
    }
}
