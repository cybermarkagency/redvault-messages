import QtQuick
import QtQuick.Controls.Basic

// A filter chip / segmented option: raised when off, light when on.
AbstractButton {
    id: control
    checkable: false
    hoverEnabled: true
    implicitHeight: 32
    implicitWidth: label.implicitWidth + 28
    Accessible.name: text

    background: Rectangle {
        radius: 16
        color: control.checked ? Theme.toggleOnBg : (control.hovered ? Theme.surface3 : Theme.surface2)
    }
    contentItem: Text {
        textFormat: Text.PlainText
        id: label
        text: control.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: control.checked ? Theme.toggleOnFg : Theme.text2
        font.pixelSize: 13
        font.weight: Font.Medium
    }
}
