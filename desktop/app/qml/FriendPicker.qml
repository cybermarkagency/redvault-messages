pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A checklist of friends; `selected` holds the chosen user ids.
ColumnLayout {
    id: picker
    required property var friends
    /// User ids to leave out (people already in the group).
    property var exclude: []
    property var selected: []

    spacing: 2

    function toggle(userId, on) {
        const next = picker.selected.filter(id => id !== userId)
        if (on)
            next.push(userId)
        picker.selected = next
    }

    function reset() {
        picker.selected = []
    }

    readonly property var available: picker.friends.filter(f => picker.exclude.indexOf(f.userId) < 0)

    Text {
        visible: picker.available.length === 0
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: picker.friends.length === 0 ? "You don't have any friends yet. Add one from Friends first."
                                          : "Everyone you're friends with is already here."
        textFormat: Text.PlainText
        color: Theme.text3
    }

    ScrollView {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(240, list.contentHeight)
        visible: picker.available.length > 0
        clip: true

        ListView {
            id: list
            model: picker.available
            delegate: CheckDelegate {
                id: check
                required property var modelData
                width: ListView.view.width
                text: modelData.handle
                checked: picker.selected.indexOf(modelData.userId) >= 0
                onToggled: picker.toggle(modelData.userId, checked)
                contentItem: Text {
                    rightPadding: check.indicator.width + 8
                    text: check.text
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.text
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: check.hovered ? Theme.surface2 : "transparent"
                    radius: Theme.radiusControl
                }
            }
        }
    }
}
