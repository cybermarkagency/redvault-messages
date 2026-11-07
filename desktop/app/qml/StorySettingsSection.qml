pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Settings > Stories: who sees your stories, whether others see that you
// viewed theirs, the archive and highlights, and muted people. These
// settings are encrypted on this computer and synced to your other devices;
// the server can't read them.
ColumnLayout {
    id: section
    required property StoriesSession stories
    spacing: 12

    Component.onCompleted: if (section.stories) section.stories.loadLibrary()

    component Caption: Text {
        Layout.topMargin: 10
        color: Theme.text2
        font.pixelSize: Theme.fontSmall
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
    }
    component Note: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.text3
        font.pixelSize: Theme.fontSmall
    }
    component PickRow: ItemDelegate {
        id: pickRow
        property string title
        property string detail
        Layout.fillWidth: true
        implicitHeight: pickColumn.implicitHeight + 24
        leftPadding: 16
        rightPadding: 16
        background: Rectangle {
            radius: Theme.radiusControl
            color: pickRow.hovered ? Theme.surface2 : Theme.surface
        }
        contentItem: ColumnLayout {
            id: pickColumn
            spacing: 3
            Text { textFormat: Text.PlainText; text: pickRow.title; color: Theme.text; font.pixelSize: Theme.fontSize; font.weight: Font.Medium }
            Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: pickRow.detail; wrapMode: Text.Wrap; color: Theme.text2; font.pixelSize: Theme.fontSmall }
        }
    }

    function names(key) {
        const list = section.stories ? section.stories.friends.filter(f => f[key]) : []
        if (list.length === 0) return "Nobody"
        const shown = list.slice(0, 3).map(f => f.handle).join(", ")
        return list.length <= 3 ? shown : shown + " and " + (list.length - 3) + " more"
    }

    Note {
        text: "Stories are photos, videos and text your friends can see for 24 hours. They're end-to-end encrypted: each one has its own key, sealed only to the friends you share it with. Every account, on every plan, can post them."
        color: Theme.text2
        font.pixelSize: Theme.fontSize
    }
    Text {
        Layout.fillWidth: true
        visible: section.stories !== null && section.stories.error.length > 0
        text: section.stories ? section.stories.error : ""
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.danger
    }

    Caption { text: "Who sees your stories" }
    PickRow {
        title: "Close friends"
        detail: section.names("close") + ". Share a story with just them by picking “Close friends” when you post."
        onClicked: picker.openFor("close")
    }
    PickRow {
        title: "Hide story from"
        detail: section.names("hidden") + ". They won't get your stories, including ones already up."
        onClicked: picker.openFor("hidden")
    }
    RvSwitch {
        Layout.fillWidth: true
        label: "Allow replies"
        description: "Friends can reply and react; replies arrive as messages in your chat with them. You can change it for each story."
        checked: section.stories !== null && section.stories.allowReplies
        onToggled: (on) => section.stories.setAllowReplies(on)
    }

    Caption { text: "Viewing" }
    RvSwitch {
        Layout.fillWidth: true
        label: "Share that you viewed stories"
        description: "When on, friends see you in their story's viewer list (never when you looked). When off, this app never tells the server you saw a story."
        checked: section.stories !== null && section.stories.shareViews
        onToggled: (on) => section.stories.setShareViews(on)
    }

    Caption { text: "Archive and highlights" }
    RvSwitch {
        Layout.fillWidth: true
        label: "Save to archive"
        description: "Keep your stories, encrypted and visible only to you, after their 24 hours. Off: they're deleted (unless in a highlight)."
        checked: section.stories !== null && section.stories.archiveEnabled
        onToggled: (on) => section.stories.setArchive(on)
    }
    Note {
        visible: section.stories !== null && section.stories.library.length === 0
        text: "No stories yet. Your stories from today and the archive show here."
    }
    Flow {
        Layout.fillWidth: true
        spacing: 6
        Repeater {
            model: section.stories ? section.stories.library : []
            delegate: StoryThumb {
                required property var modelData
                required property int index
                story: modelData
                onClicked: section.stories.openLibrary(index)
            }
        }
    }
    Repeater {
        model: section.stories ? section.stories.ownHighlights : []
        delegate: RowLayout {
            id: highlightRow
            required property var modelData
            Layout.fillWidth: true
            spacing: 10
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text { textFormat: Text.PlainText; text: highlightRow.modelData.title.length > 0 ? highlightRow.modelData.title : "Highlight"; color: Theme.text; font.weight: Font.Medium }
                Text { textFormat: Text.PlainText; text: highlightRow.modelData.stories.length + (highlightRow.modelData.stories.length === 1 ? " story" : " stories"); color: Theme.text3; font.pixelSize: Theme.fontSmall }
            }
            RvButton {
                secondary: true
                text: "View"
                onClicked: section.stories.openHighlight(App.userId, highlightRow.modelData.id)
            }
            RvButton {
                secondary: true
                text: "Edit"
                onClicked: editor.openFor(highlightRow.modelData)
            }
            RvButton {
                primary: false
                danger: true
                text: "Delete"
                onClicked: section.stories.deleteHighlight(highlightRow.modelData.id)
            }
        }
    }
    RvButton {
        secondary: true
        iconName: "plus"
        text: "New highlight"
        onClicked: editor.openFor(null)
    }
    Note { text: "Highlights stay on your profile for friends who could see the stories in them." }

    Caption { text: "Muted" }
    Note {
        visible: section.stories === null || section.stories.friends.filter(f => f.muted).length === 0
        text: "Nobody's muted. Mute someone from the ⋯ menu on their story: they won't know."
    }
    Repeater {
        model: section.stories ? section.stories.friends.filter(f => f.muted) : []
        delegate: RowLayout {
            id: mutedRow
            required property var modelData
            Layout.fillWidth: true
            spacing: 10
            Avatar {
                size: 32
                initial: mutedRow.modelData.handle.charAt(0).toUpperCase()
                tintKey: mutedRow.modelData.userId
                source: mutedRow.modelData.avatarUrl
            }
            Text { Layout.fillWidth: true; text: mutedRow.modelData.handle; textFormat: Text.PlainText; color: Theme.text; elide: Text.ElideRight }
            RvButton {
                secondary: true
                text: "Unmute"
                onClicked: section.stories.setMuted(mutedRow.modelData.userId, false)
            }
        }
    }

    Caption { text: "What the server sees" }
    Note {
        text: "When you posted, how big it is, whether it's a photo, video or text, and which friends got a key. Not what it shows, its text or stickers, your close friends list or who you hide stories from. Anyone who can see a story could still photograph their screen."
    }

    // --- Pick close friends, or who to hide stories from ---
    RvDialog {
        id: picker
        property string key: "close"
        property var chosen: []
        title: key === "close" ? "Close friends" : "Hide story from"
        function openFor(k) {
            picker.key = k
            picker.chosen = section.stories.friends.filter(f => f[k]).map(f => f.userId)
            search.text = ""
            picker.open()
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 10
            SearchField {
                id: search
                Layout.fillWidth: true
                placeholderText: "Search friends"
            }
            Note {
                visible: section.stories !== null && section.stories.friends.length === 0
                text: "Add friends first."
            }
            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 320)
                clip: true
                model: section.stories ? section.stories.friends.filter(f => f.isFriend && f.handle.toLowerCase().indexOf(search.text.trim().toLowerCase()) >= 0) : []
                delegate: CheckDelegate {
                    id: friendRow
                    required property var modelData
                    width: ListView.view.width
                    text: friendRow.modelData.handle
                    checked: picker.chosen.indexOf(friendRow.modelData.userId) >= 0
                    onToggled: {
                        const next = picker.chosen.filter(id => id !== friendRow.modelData.userId)
                        if (checked) next.push(friendRow.modelData.userId)
                        picker.chosen = next
                    }
                    contentItem: Text {
                        text: friendRow.text
                        textFormat: Text.PlainText
                        color: Theme.text
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                RvButton { secondary: true; text: "Cancel"; onClicked: picker.close() }
                RvButton {
                    text: "Save (" + picker.chosen.length + ")"
                    onClicked: {
                        if (picker.key === "close") section.stories.setCloseFriends(picker.chosen)
                        else section.stories.setHideFrom(picker.chosen)
                        picker.close()
                    }
                }
            }
        }
    }

    // --- Make or change a highlight ---
    RvDialog {
        id: editor
        property var highlight: null
        property var picked: []
        width: 560
        title: editor.highlight ? "Edit highlight" : "New highlight"
        function openFor(h) {
            editor.highlight = h
            editor.picked = h ? [h.coverStoryId].concat(h.stories.map(s => s.id).filter(id => id !== h.coverStoryId)) : []
            nameField.text = h ? h.title : ""
            section.stories.loadLibrary()
            editor.open()
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 10
            RvTextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: "Name"
                maximumLength: 40
            }
            Note { text: "Pick stories from today and your archive. The first one you pick is the cover." }
            Flickable {
                Layout.fillWidth: true
                Layout.preferredHeight: 300
                clip: true
                contentHeight: pickFlow.implicitHeight
                Flow {
                    id: pickFlow
                    width: parent.width
                    spacing: 6
                    Repeater {
                        model: section.stories ? section.stories.library : []
                        delegate: StoryThumb {
                            required property var modelData
                            story: modelData
                            selected: editor.picked.indexOf(modelData.id) >= 0
                            onClicked: {
                                const id = modelData.id
                                editor.picked = editor.picked.indexOf(id) >= 0 ? editor.picked.filter(x => x !== id) : editor.picked.concat([id])
                            }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                RvButton { secondary: true; text: "Cancel"; onClicked: editor.close() }
                RvButton {
                    text: "Save"
                    enabled: nameField.text.trim().length > 0 && editor.picked.length > 0
                    onClicked: {
                        section.stories.saveHighlight(editor.highlight ? editor.highlight.id : "", nameField.text, editor.picked)
                        editor.close()
                    }
                }
            }
        }
    }

    // A small still of a story: its photo, a video tile, or its text on its background.
    component StoryThumb: AbstractButton {
        id: thumb
        property var story: ({})
        property bool selected: false
        property string image: ""
        width: 84
        height: 150
        Accessible.name: (thumb.story.kind === "text" ? "Text" : thumb.story.kind === "image" ? "Photo" : "Video") + " story"
        Component.onCompleted: if (thumb.story.kind === "image") section.stories.loadMedia(thumb.story.id)
        Connections {
            target: section.stories
            function onMediaReady(storyId, url) {
                if (storyId === thumb.story.id && thumb.story.kind === "image") thumb.image = url.replace("/full/", "/thumb/")
            }
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            clip: true
            border.width: thumb.selected ? 3 : 0
            border.color: Theme.accent
            gradient: Gradient {
                GradientStop { position: 0.0; color: thumb.story.kind === "text" ? (Theme.storyBackgrounds[thumb.story.background] || Theme.storyBackgrounds.sunset)[0] : Theme.surface2 }
                GradientStop { position: 1.0; color: thumb.story.kind === "text" ? (Theme.storyBackgrounds[thumb.story.background] || Theme.storyBackgrounds.sunset)[1] : Theme.surface2 }
            }
        }
        contentItem: Item {
            Image {
                anchors.fill: parent
                anchors.margins: thumb.selected ? 3 : 0
                visible: thumb.image.length > 0
                source: thumb.image
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
            Icon {
                anchors.centerIn: parent
                visible: thumb.story.kind === "video"
                name: "play"
                size: 24
                color: Theme.text2
            }
            Text {
                anchors.fill: parent
                anchors.margins: 6
                visible: thumb.story.kind === "text"
                text: (thumb.story.text || "").substring(0, 40)
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: Theme.storyColors.white
                font.pixelSize: 11
                font.weight: Font.DemiBold
            }
        }
    }
}
