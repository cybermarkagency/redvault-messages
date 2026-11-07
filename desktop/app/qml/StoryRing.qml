import QtQuick

// A ring around an avatar, like Instagram's: colourful for something new,
// green for close friends, grey once seen, nothing for "none".
Item {
    id: ring
    /// "new", "close", "seen" or "none".
    property string kind: "none"
    property int size: 54
    default property alias content: inner.data

    implicitWidth: size + 8
    implicitHeight: size + 8

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        visible: ring.kind !== "none"
        color: ring.kind === "close" ? Theme.storyRingClose : Theme.storyRingSeen
        gradient: ring.kind === "new" ? newGradient : null
        Gradient {
            id: newGradient
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Theme.storyRing[0] }
            GradientStop { position: 0.35; color: Theme.storyRing[1] }
            GradientStop { position: 0.7; color: Theme.storyRing[2] }
            GradientStop { position: 1.0; color: Theme.storyRing[3] }
        }
    }
    Rectangle {
        anchors.centerIn: parent
        width: ring.size + 4
        height: width
        radius: width / 2
        color: Theme.panel
    }
    Item {
        id: inner
        anchors.centerIn: parent
        width: ring.size
        height: ring.size
    }
}
