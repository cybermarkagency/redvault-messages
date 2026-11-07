import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A channel's photo at full resolution over the whole window, with Save.
// Esc, the close button or a click outside the picture closes it.
Popup {
    id: viewer
    property var media: null
    signal saveRequested(var media)

    function show(m) {
        viewer.media = m
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
    onClosed: media = null

    contentItem: Item {
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
            source: viewer.media ? "image://channelmedia/" + encodeURIComponent("full|" + viewer.media.url) : ""
            TapHandler {}
        }
        BusyIndicator {
            anchors.centerIn: parent
            running: full.status === Image.Loading
            visible: running
        }
        Text {
            anchors.centerIn: parent
            visible: full.status === Image.Error
            text: "Couldn't load the photo. Reload the channel and try again."
            color: "#FFFFFF"
        }
        RowLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 20
            spacing: 10
            Text {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: viewer.media ? viewer.media.name + " · " + viewer.media.sizeLabel : ""
                textFormat: Text.PlainText
                color: "#FFFFFF"
                font.weight: Font.DemiBold
            }
            RvButton {
                secondary: true
                iconName: "download"
                text: "Save"
                onClicked: viewer.saveRequested(viewer.media)
            }
            RvButton {
                secondary: true
                iconName: "x"
                Accessible.name: "Close"
                onClicked: viewer.close()
            }
        }
    }
}
