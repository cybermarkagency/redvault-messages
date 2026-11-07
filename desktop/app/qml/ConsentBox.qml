import QtQuick
import QtQuick.Controls.Basic
import Redvault

// A sign-up box. Its label may hold a link (styled text); following the
// link opens the document and never ticks the box.
CheckBox {
    id: box
    property string label: ""
    signal linkActivated(string link)

    // The plain words: for screen readers, and so the indicator sits at the start.
    text: box.label.replace(/<[^>]*>/g, "")
    spacing: 10

    indicator: Rectangle {
        x: box.leftPadding
        y: box.topPadding + (box.availableHeight - height) / 2
        implicitWidth: 18
        implicitHeight: 18
        radius: 4
        color: box.checked ? Theme.accent : "transparent"
        border.color: box.checked ? Theme.accent : (box.visualFocus || box.hovered ? Theme.text2 : Theme.text3)
        border.width: 1.5

        Icon {
            anchors.centerIn: parent
            visible: box.checked
            name: "check"
            size: 12
            color: Theme.accentContrast
        }
    }

    contentItem: Text {
        leftPadding: box.indicator.width + box.spacing
        text: box.label
        textFormat: Text.StyledText
        linkColor: Theme.accent
        color: Theme.text
        wrapMode: Text.Wrap
        verticalAlignment: Text.AlignVCenter
        onLinkActivated: (link) => box.linkActivated(link)

        HoverHandler {
            cursorShape: parent.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
    }
}
