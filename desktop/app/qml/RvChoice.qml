import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A drop-down of plain text choices in the app's tokens.
ComboBox {
    id: choice
    implicitHeight: 40
    font.pixelSize: 14

    background: Rectangle {
        radius: Theme.radiusSmall
        color: Theme.surface2
        border.color: choice.activeFocus ? Theme.accent : Theme.borderStrong
    }
    contentItem: Text {
        textFormat: Text.PlainText
        leftPadding: 12
        rightPadding: 34
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        text: choice.displayText
        color: Theme.text
        font: choice.font
    }
    indicator: Icon {
        x: choice.width - width - 12
        y: (choice.height - height) / 2
        name: "chevron-down"
        size: 16
        color: Theme.text2
    }
    delegate: ItemDelegate {
        id: option
        required property var modelData
        required property int index
        width: ListView.view ? ListView.view.width : choice.width
        highlighted: choice.highlightedIndex === index
        background: Rectangle { color: option.highlighted ? Theme.selected : "transparent" }
        contentItem: Text {
            textFormat: Text.PlainText
            text: typeof option.modelData === "string" ? option.modelData : option.modelData[choice.textRole]
            color: Theme.text
            elide: Text.ElideRight
            font.pixelSize: 14
        }
    }
    popup.background: Rectangle { radius: 12; color: Theme.surface; border.color: Theme.borderStrong }
}
