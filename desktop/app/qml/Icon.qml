import QtQuick

// One of the design's icons (icons/ui, from the web app) or a Lucide icon
// (icons/lucide, ISC licensed) in any color, rendered at twice its size so it
// stays sharp on high-DPI screens. Layouts size it by `size`.
Item {
    id: icon
    /// A file name in icons/ui or icons/lucide without ".svg", e.g. "paperclip".
    property string name
    property color color: Theme.text
    property int size: 18

    function hex(c) {
        return [c.r, c.g, c.b].map(v => Math.round(v * 255).toString(16).padStart(2, "0")).join("")
    }

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    Image {
        anchors.fill: parent
        sourceSize: Qt.size(icon.size * 2, icon.size * 2)
        fillMode: Image.PreserveAspectFit
        smooth: true
        opacity: icon.color.a
        source: icon.name.length > 0 ? "image://icon/" + icon.name + "/" + icon.hex(icon.color) : ""
    }
}
