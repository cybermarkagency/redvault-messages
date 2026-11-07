import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A picture from a chat, full size over the whole window. Esc, the close
// button or a click outside the picture closes it.
Popup {
    id: viewer
    property string messageId: ""
    property string title: ""
    property string subtitle: ""
    property bool canOpen: true
    signal saveRequested(string messageId)
    signal openRequested(string messageId)

    function show(id, name, detail) {
        viewer.messageId = id
        viewer.title = name
        viewer.subtitle = detail
        open()
    }

    parent: Overlay.overlay
    x: 0
    y: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        color: "#F20A0D0C"
    }
    onClosed: messageId = ""

    contentItem: Item {
        // Clicking around the picture closes it.
        TapHandler {
            onTapped: viewer.close()
        }

        Image {
            id: full
            anchors.fill: parent
            anchors.margins: 72
            anchors.topMargin: 80
            asynchronous: true
            cache: false
            fillMode: Image.PreserveAspectFit
            source: viewer.messageId.length > 0 ? "image://chatimage/full/" + viewer.messageId : ""
            // Don't let a click on the picture itself close the viewer.
            TapHandler {}
        }
        BusyIndicator {
            anchors.centerIn: parent
            running: full.status === Image.Loading
            visible: running
        }

        RowLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 18
            spacing: 8
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    Layout.fillWidth: true
                    text: viewer.title
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: Theme.text
                    font.weight: Font.DemiBold
                }
                Text {
                    text: viewer.subtitle
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall
                }
            }
            IconButton {
                iconName: "download"
                tip: "Save as…"
                onClicked: viewer.saveRequested(viewer.messageId)
            }
            IconButton {
                iconName: "external-link"
                tip: "Open in another app"
                enabled: viewer.canOpen
                onClicked: viewer.openRequested(viewer.messageId)
            }
            IconButton {
                iconName: "x"
                tip: "Close (Esc)"
                onClicked: viewer.close()
            }
        }
    }
}
