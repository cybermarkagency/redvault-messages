import QtQuick
import QtQuick.Controls.Basic

// A rounded search box with a magnifier.
TextField {
    id: control
    implicitHeight: 40
    leftPadding: 40
    rightPadding: 12
    font.pixelSize: Theme.fontSize
    color: Theme.text
    placeholderTextColor: Theme.placeholder
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentContrast
    Accessible.name: placeholderText

    background: Rectangle {
        radius: Theme.radiusControl
        color: Theme.surface2
        border.width: control.activeFocus ? 1 : 0
        border.color: Theme.accentBorder
        Icon {
            anchors.left: parent.left
            anchors.leftMargin: 13
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            size: 17
            color: Theme.text3
        }
    }
}
