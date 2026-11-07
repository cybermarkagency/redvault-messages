import QtQuick

// An error (danger) or notice (accent) message; hidden when empty.
Rectangle {
    id: banner
    property string text
    property bool error: true

    visible: text.length > 0
    implicitHeight: visible ? label.implicitHeight + 16 : 0
    radius: Theme.radiusControl
    color: error ? Theme.dangerSoft : Theme.accentSoft

    Text {
        id: label
        anchors.fill: parent
        anchors.margins: 8
        text: banner.text
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: banner.error ? Theme.danger : Theme.accent
        font.pixelSize: Theme.fontSmall + 1
    }
}
