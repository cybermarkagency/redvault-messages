import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The Terms of Service or Privacy Policy, read in place over the sign-up
// form: App.legalDocument() returns the Markdown built into the app.
RvDialog {
    id: dialog
    property string body: ""

    width: Math.min(680, Overlay.overlay ? Overlay.overlay.width - 48 : 680)
    height: Math.min(760, Overlay.overlay ? Overlay.overlay.height - 48 : 640)

    function show(name) {
        const markdown = App.legalDocument(name)
        // The first line is the document's "# " title; it goes in the header.
        const newline = markdown.indexOf("\n")
        dialog.title = markdown.substring(0, newline).replace(/^#\s*/, "")
        // Shown as plain text like on the web and Android: an escaped "@" stops
        // Markdown from turning the contact address into a (blue) link.
        dialog.body = markdown.substring(newline + 1).replace(/@/g, "\\@")
        flick.contentY = 0
        dialog.open()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: doc.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            Text {
                id: doc
                width: flick.width - 14
                text: dialog.body
                textFormat: Text.MarkdownText
                wrapMode: Text.Wrap
                color: Theme.text2
                font.pixelSize: Theme.fontSmall + 2
                lineHeight: 1.15
            }
        }

        RvButton {
            Layout.alignment: Qt.AlignRight
            text: "Done"
            onClicked: dialog.close()
        }
    }
}
