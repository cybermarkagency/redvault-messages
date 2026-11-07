import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The frame of the lock and key dialogs: an icon tile, a large title with a
// line under it, and a close button. The content goes in `contentItem`.
Dialog {
    id: dialog
    property string iconName: "key"
    property string subtitle
    /// Rich text for the subtitle (bold key names, mono fingerprints); plain `subtitle` otherwise.
    property string subtitleRich
    property bool showIcon: true

    modal: true
    anchors.centerIn: Overlay.overlay
    width: 580
    padding: 28
    topPadding: 20
    closePolicy: Popup.CloseOnEscape

    Overlay.modal: Rectangle { color: "#B0000000" }

    background: Rectangle {
        radius: 24
        color: Theme.panel
        border.color: Theme.borderStrong
    }

    header: RowLayout {
        spacing: 14
        Rectangle {
            visible: dialog.showIcon
            Layout.leftMargin: 28
            Layout.topMargin: 26
            Layout.alignment: Qt.AlignTop
            implicitWidth: 44
            implicitHeight: 44
            radius: 12
            color: Theme.lockBg
            Icon { anchors.centerIn: parent; name: dialog.iconName; size: 20; color: Theme.lockText }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: dialog.showIcon ? 0 : 28
            Layout.topMargin: 24
            spacing: 4
            Text {
                Layout.fillWidth: true
                text: dialog.title
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.text
                font.family: Theme.displayFamily
                font.pixelSize: 26
                font.weight: Font.Bold
                font.letterSpacing: -0.4
            }
            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: dialog.subtitleRich.length > 0 ? dialog.subtitleRich : dialog.subtitle
                textFormat: dialog.subtitleRich.length > 0 ? Text.StyledText : Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.text2
                font.pixelSize: 14
            }
        }
        IconButton {
            Layout.rightMargin: 26
            Layout.topMargin: 26
            Layout.alignment: Qt.AlignTop
            filled: true
            iconName: "x"
            iconSize: 16
            tip: "Close"
            onClicked: dialog.close()
        }
    }
}
