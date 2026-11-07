pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The row of stories above the chat list: yours first (with a + to post),
// then friends with something new, then the rest.
ListView {
    id: tray
    required property StoriesSession stories
    required property AccountSession account

    implicitHeight: 92
    orientation: ListView.Horizontal
    spacing: 6
    leftMargin: 14
    rightMargin: 14
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    model: tray.stories ? tray.stories.tray : []
    Accessible.role: Accessible.List
    Accessible.name: "Stories"

    header: Item {
        width: 76
        height: tray.height
        ItemDelegate {
            id: own
            width: 70
            height: parent.height
            padding: 0
            Accessible.name: tray.stories && tray.stories.ownCount > 0 ? "Your story" : "Add to your story"
            onClicked: {
                if (!tray.stories) return
                if (tray.stories.ownCount > 0) tray.stories.openAuthor("me")
                else tray.stories.openComposer()
            }
            background: null
            contentItem: Column {
                spacing: 4
                StoryRing {
                    anchors.horizontalCenter: parent.horizontalCenter
                    size: 54
                    kind: tray.stories && tray.stories.ownCount > 0 ? "seen" : "none"
                    Avatar {
                        size: 54
                        initial: App.handle.length > 0 ? App.handle.charAt(0).toUpperCase() : "?"
                        tintKey: App.handle
                        source: tray.account ? tray.account.avatarUrl : ""
                    }
                }
                Text {
                    width: 70
                    horizontalAlignment: Text.AlignHCenter
                    text: "Your story"
                    elide: Text.ElideRight
                    color: Theme.text2
                    font.pixelSize: Theme.fontSmall
                }
            }
        }
        Rectangle {
            x: 48
            y: 44
            width: 20
            height: 20
            radius: 10
            color: Theme.accent
            border.width: 2
            border.color: Theme.panel
            Icon {
                anchors.centerIn: parent
                name: "plus"
                size: 12
                color: Theme.accentContrast
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                Accessible.role: Accessible.Button
                Accessible.name: "New story"
                onClicked: if (tray.stories) tray.stories.openComposer()
            }
        }
    }

    delegate: ItemDelegate {
        id: item
        required property var modelData
        width: 70
        height: tray.height
        padding: 0
        background: null
        Accessible.name: item.modelData.handle + "'s story" + (item.modelData.unseen > 0 ? ", " + item.modelData.unseen + " new" : "")
        onClicked: tray.stories.openAuthor(item.modelData.authorId)
        contentItem: Column {
            spacing: 4
            StoryRing {
                anchors.horizontalCenter: parent.horizontalCenter
                size: 54
                kind: item.modelData.ring
                Avatar {
                    size: 54
                    initial: item.modelData.handle.charAt(0).toUpperCase()
                    tintKey: item.modelData.authorId
                    source: item.modelData.avatarUrl
                }
            }
            Text {
                width: 70
                horizontalAlignment: Text.AlignHCenter
                text: item.modelData.handle
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: item.modelData.unseen > 0 ? Theme.text : Theme.text2
                font.pixelSize: Theme.fontSmall
            }
        }
    }
}
