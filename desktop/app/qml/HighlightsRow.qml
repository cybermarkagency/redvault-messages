pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// On a friend's details: their story (when they have one up) and the
// highlights you can see (only stories they shared with you).
ColumnLayout {
    id: row
    required property StoriesSession stories
    required property string userId
    property var highlights: []
    readonly property var trayEntry: {
        if (!row.stories) return null
        const list = row.stories.tray
        for (let i = 0; i < list.length; i++) if (list[i].authorId === row.userId) return list[i]
        return null
    }
    spacing: 8
    visible: row.trayEntry !== null || row.highlights.length > 0

    onUserIdChanged: load()
    Component.onCompleted: load()
    function load() {
        row.highlights = []
        if (row.stories && row.userId.length > 0) row.stories.loadHighlights(row.userId)
    }
    Connections {
        target: row.stories
        function onHighlightsLoaded(userId, list) {
            if (userId === row.userId) row.highlights = list
        }
    }

    Text {
        text: "STORIES"
        color: Theme.text3
        font.pixelSize: Theme.fontSmall
        font.weight: Font.DemiBold
    }
    Flow {
        Layout.fillWidth: true
        spacing: 12
        ItemDelegate {
            visible: row.trayEntry !== null
            width: 64
            height: 86
            padding: 0
            background: null
            Accessible.name: "View their story"
            onClicked: row.stories.openAuthor(row.userId)
            contentItem: Column {
                spacing: 4
                StoryRing {
                    anchors.horizontalCenter: parent.horizontalCenter
                    size: 50
                    kind: row.trayEntry ? row.trayEntry.ring : "none"
                    Avatar {
                        size: 50
                        initial: row.trayEntry ? row.trayEntry.handle.charAt(0).toUpperCase() : "?"
                        tintKey: row.userId
                        source: row.trayEntry ? row.trayEntry.avatarUrl : ""
                    }
                }
                Text {
                    width: 64
                    horizontalAlignment: Text.AlignHCenter
                    text: "Story"
                    color: Theme.text2
                    font.pixelSize: Theme.fontSmall
                }
            }
        }
        Repeater {
            model: row.highlights
            delegate: ItemDelegate {
                id: highlight
                required property var modelData
                width: 64
                height: 86
                padding: 0
                background: null
                Accessible.name: "Highlight: " + (highlight.modelData.title.length > 0 ? highlight.modelData.title : "untitled")
                onClicked: row.stories.openHighlight(row.userId, highlight.modelData.id)
                contentItem: Column {
                    spacing: 4
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 54
                        height: 54
                        radius: 27
                        color: Theme.surface2
                        border.width: 2
                        border.color: Theme.borderStrong
                        Text {
                            anchors.centerIn: parent
                            text: highlight.modelData.title.length > 0 ? highlight.modelData.title.charAt(0).toUpperCase() : "★"
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: 20
                            font.weight: Font.Bold
                        }
                    }
                    Text {
                        width: 64
                        horizontalAlignment: Text.AlignHCenter
                        text: highlight.modelData.title.length > 0 ? highlight.modelData.title : "Highlight"
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text2
                        font.pixelSize: Theme.fontSmall
                    }
                }
            }
        }
    }
}
