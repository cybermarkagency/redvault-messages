// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault
import "EmojiData.js" as EmojiData

// Emoji for the composer: search by name, browse by category, recent ones
// first. Recents are kept for this session only, like everything else.
Popup {
    id: picker
    signal picked(string emoji)

    property int category: 0
    property var recent: []
    readonly property var categories: [{ name: "Recent", emoji: picker.recent.map(e => [e, ""]) }].concat(EmojiData.categories)
    readonly property var shown: {
        const q = search.text.trim().toLowerCase()
        if (q.length === 0) return picker.categories[picker.category].emoji
        const out = []
        for (const c of EmojiData.categories) {
            for (const e of c.emoji) {
                if (e[1].indexOf(q) >= 0) out.push(e)
                if (out.length >= 200) return out
            }
        }
        return out
    }

    width: 360
    height: 380
    padding: 10
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: {
        search.text = ""
        if (picker.category === 0 && picker.recent.length === 0) picker.category = 1
        search.forceActiveFocus()
    }

    function choose(emoji) {
        picker.recent = [emoji].concat(picker.recent.filter(e => e !== emoji)).slice(0, 32)
        picker.picked(emoji)
    }

    background: Rectangle {
        radius: Theme.radiusCard
        color: Theme.surface
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 8

        SearchField {
            id: search
            Layout.fillWidth: true
            placeholderText: "Search emoji"
            Keys.onReturnPressed: if (picker.shown.length > 0) picker.choose(picker.shown[0][0])
        }

        // Categories, each shown by its first emoji.
        Row {
            visible: search.text.trim().length === 0
            spacing: 2
            Repeater {
                model: picker.categories.length
                delegate: AbstractButton {
                    id: tab
                    required property int index
                    readonly property var cat: picker.categories[tab.index]
                    visible: tab.index > 0 || picker.recent.length > 0
                    width: 36
                    height: 32
                    hoverEnabled: true
                    ToolTip.visible: hovered
                    ToolTip.text: tab.cat.name
                    onClicked: picker.category = tab.index
                    background: Rectangle {
                        radius: 8
                        color: picker.category === tab.index ? Theme.selected : tab.hovered ? Theme.surface2 : "transparent"
                    }
                    contentItem: Text {
                        textFormat: Text.PlainText
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: tab.index === 0 ? "\u{1F552}" : tab.cat.emoji.length > 0 ? tab.cat.emoji[0][0] : ""
                        font.pixelSize: 18
                    }
                }
            }
        }

        Text {
            text: search.text.trim().length > 0 ? (picker.shown.length > 0 ? "Results" : "No emoji found")
                                                : picker.categories[picker.category].name
            textFormat: Text.PlainText
            color: Theme.text3
            font.pixelSize: Theme.fontSmall
            font.weight: Font.DemiBold
        }

        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            cellWidth: Math.floor(width / 8)
            cellHeight: 40
            model: picker.shown
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: AbstractButton {
                id: cell
                required property var modelData
                width: grid.cellWidth
                height: grid.cellHeight
                hoverEnabled: true
                ToolTip.visible: hovered && cell.modelData[1].length > 0
                ToolTip.delay: 600
                ToolTip.text: cell.modelData[1]
                Accessible.name: cell.modelData[1]
                onClicked: picker.choose(cell.modelData[0])
                background: Rectangle {
                    radius: 8
                    color: cell.hovered ? Theme.surface2 : "transparent"
                }
                contentItem: Text {
                    textFormat: Text.PlainText
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: cell.modelData[0]
                    font.pixelSize: 24
                }
            }
        }
    }
}
