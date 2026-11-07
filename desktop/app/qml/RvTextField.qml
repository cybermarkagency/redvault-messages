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
