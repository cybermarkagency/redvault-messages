import QtQuick
import QtQuick.Controls.Basic

// Primary (filled accent) or quiet (text-only) button in the app's tokens,
// with an optional Lucide icon. With an icon and no text it's a square icon
// button; give it a ToolTip so it's still clear what it does.
Button {
    id: control
    property bool primary: true
    property bool danger: false
    /// Raised (not filled with the accent), like the design's secondary buttons.
    property bool secondary: false
    /// A Lucide icon name (icons/lucide), or empty for text only.
    property string iconName: ""
    property int iconSize: 18

    readonly property color ink: control.secondary ? Theme.text
                               : control.primary ? (control.danger ? "#FFFFFF" : Theme.accentContrast)
                               : (control.danger ? Theme.danger : Theme.accent)
    readonly property bool iconOnly: control.iconName.length > 0 && control.text.length === 0

    implicitHeight: 40
    leftPadding: iconOnly ? 8 : 16
    rightPadding: iconOnly ? 8 : 16
    font.pixelSize: Theme.fontSize
    font.weight: Font.DemiBold

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        opacity: control.enabled ? 1 : 0.55

        Row {
            id: row
            anchors.centerIn: parent
            spacing: 6

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.iconName.length > 0
                name: control.iconName
                size: control.iconSize
                color: control.ink
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.text.length > 0
                text: control.text
                textFormat: Text.PlainText
                font: control.font
                color: control.ink
                elide: Text.ElideRight
            }
        }
    }

    background: Rectangle {
        radius: Theme.radiusControl
        color: control.secondary ? (control.hovered ? Theme.borderStrong : Theme.surface3)
             : control.primary ? (control.danger ? Theme.dangerStrong : Theme.accent)
             : (control.hovered ? Theme.surface2 : "transparent")
        opacity: control.enabled ? (control.down ? 0.85 : 1) : 0.45
    }
}
