import QtQuick
import QtQuick.Controls.Basic
import Redvault

// A drop-down in the app's tokens, for the story forms (lifetime, who sees the
// results, a friend to mention, a report reason). `model` is a list of strings
// or of maps with `textRole`.
ComboBox {
    id: combo
    implicitHeight: 40
    font.pixelSize: Theme.fontSize

    background: Rectangle {
        radius: Theme.radiusSmall
        color: Theme.surface2
        border.width: combo.activeFocus ? 2 : 1
        border.color: combo.activeFocus ? Theme.accent : Theme.borderStrong
    }
    contentItem: Text {
        leftPadding: 12
        rightPadding: 30
        text: combo.displayText
        textFormat: Text.PlainText
        color: Theme.text
        font: combo.font
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }
    indicator: Icon {
        x: combo.width - width - 10
        y: (combo.height - height) / 2
        name: "chevron-down"
        size: 16
        color: Theme.text3
    }
    delegate: ItemDelegate {
        id: option
        required property var modelData
        required property int index
        width: ListView.view ? ListView.view.width : combo.width
        highlighted: combo.highlightedIndex === option.index
        background: Rectangle { color: option.highlighted ? Theme.selected : "transparent" }
        contentItem: Text {
            text: combo.textRole.length > 0 && option.modelData !== null && typeof option.modelData === "object" ? option.modelData[combo.textRole] : option.modelData
            textFormat: Text.PlainText
            color: Theme.text
            font: combo.font
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
    }
    popup.background: Rectangle { radius: Theme.radiusSmall; color: Theme.surface; border.color: Theme.borderStrong }
}
