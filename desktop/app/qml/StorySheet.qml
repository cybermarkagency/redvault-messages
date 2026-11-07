import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A panel that slides over the bottom of a story (answers, reports, a link to
// confirm). Anything inside it is kept off the story behind: a click on the
// panel is not a tap on the story.
Rectangle {
    id: sheet
    property string title
    property string note
    /// Show a close button in the header.
    property bool closable: true
    default property alias content: column.data
    signal closed()

    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    height: Math.min(parent.height * 0.8, column.implicitHeight + 32)
    radius: Theme.radiusDialog
    color: Theme.surface

    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = false }
    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: sheet.title
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            IconButton {
                visible: sheet.closable
                iconName: "close"
                tip: "Close"
                onClicked: sheet.closed()
            }
        }
        Text {
            Layout.fillWidth: true
            visible: sheet.note.length > 0
            text: sheet.note
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.text3
            font.pixelSize: Theme.fontSmall
        }
    }
}
