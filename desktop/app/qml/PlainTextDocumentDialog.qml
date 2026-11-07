import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A long built-in text (the app's licence, the third-party notices) read in place: plain text only, never rich
// text or Markdown, read-only but selectable. It comes as pages (App.openSourceDocument), so the list only lays
// out what's on screen: the notices are about 100,000 lines.
RvDialog {
    id: dialog
    property var pages: []

    width: Math.min(820, Overlay.overlay ? Overlay.overlay.width - 48 : 820)
    height: Math.min(820, Overlay.overlay ? Overlay.overlay.height - 48 : 640)

    function show(name, heading) {
        dialog.title = heading
        dialog.pages = App.openSourceDocument(name)
        list.positionViewAtBeginning()
        dialog.open()
    }

    onClosed: pages = []

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        Text {
            Layout.fillWidth: true
            visible: dialog.pages.length === 0
            text: "This text isn't built into this copy of the app."
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.warn
        }

        ListView {
            id: list
            objectName: "plainTextPages"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dialog.pages
            cacheBuffer: 2000
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: TextEdit {
                required property string modelData
                width: ListView.view.width - 14
                text: modelData
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                color: Theme.text2
                selectionColor: Theme.accentBorder
                selectedTextColor: Theme.text
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSmall + 1
            }
        }

        RvButton {
            Layout.alignment: Qt.AlignRight
            text: "Done"
            onClicked: dialog.close()
        }
    }
}
