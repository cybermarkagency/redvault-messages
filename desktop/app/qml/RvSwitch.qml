import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A labelled on/off setting with an explanation underneath.
RowLayout {
    id: row
    property alias checked: toggle.checked
    property string label
    property string description
    signal toggled(bool checked)

    spacing: 12

    ColumnLayout {
        Layout.fillWidth: true
        // Take the space that's left rather than asking for the text's full width.
        Layout.preferredWidth: 0
        spacing: 2
        Text {
            Layout.fillWidth: true
            text: row.label
            textFormat: Text.PlainText
            color: Theme.text
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            visible: row.description.length > 0
            text: row.description
            textFormat: Text.PlainText
            color: Theme.text2
            font.pixelSize: Theme.fontSmall + 1
            wrapMode: Text.Wrap
        }
    }

    Switch {
        id: toggle
        onToggled: row.toggled(checked)

        indicator: Rectangle {
            implicitWidth: 42
            implicitHeight: 24
            x: toggle.leftPadding
            y: parent.height / 2 - height / 2
            radius: 12
            color: toggle.checked ? Theme.accent : Theme.surface3
            Rectangle {
                x: toggle.checked ? parent.width - width - 3 : 3
                y: 3
                width: 18
                height: 18
                radius: 9
                color: toggle.checked ? Theme.accentContrast : Theme.surface
                Behavior on x { NumberAnimation { duration: 120 } }
            }
        }
    }
}
