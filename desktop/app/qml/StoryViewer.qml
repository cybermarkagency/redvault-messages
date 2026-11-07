pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Full-window stories, like Instagram's: click the right side for the next
// story, the left for the previous one, hold to pause; ← → and Space work
// too. Friends' stories have a reply box and quick reactions (they arrive as
// messages in your chat with them); your own show who saw them.
Rectangle {
    id: viewer
    required property StoriesSession stories
    signal openSettings()

    readonly property var groups: viewer.stories ? viewer.stories.viewerGroups : []
    property int groupIndex: 0
    property int storyIndex: 0
    readonly property var group: viewer.groups.length > viewer.groupIndex ? viewer.groups[viewer.groupIndex] : null
    readonly property var story: viewer.group && viewer.group.stories.length > viewer.storyIndex ? viewer.group.stories[viewer.storyIndex] : null
    readonly property bool own: viewer.group !== null && viewer.group.owner.isSelf
    property string loadedId: ""
    property string mediaUrl: ""
    property bool ready: false
    property bool failed: false
    property real elapsed: 0
    property bool held: false
    property bool userPaused: false
    property bool typing: false
    /// "", "menu", "viewers", "highlight", "answers", "report" or "link".
    property string sheet: ""
    property bool muted: false
    property real pressAt: 0
    property var viewers: []
    // Stickers (docs/stories-advanced/): what this story's stickers found (your answers and the totals, or yours: the answers sealed to you).
    property var interactions: null
    property string busySticker: ""
    property string typingSticker: ""
    property int hoverCount: 0
    property var pendingLink: null
    property real clockNow: Date.now()
    /// Whether the story is still up (a highlight past its time takes no new answers).
    readonly property bool live: viewer.story !== null && viewer.story.expiresAt > viewer.clockNow
    // The clock stops while the pointer is over a sticker or someone is typing in one.
    readonly property bool paused: viewer.held || viewer.userPaused || viewer.typing || viewer.typingSticker !== "" || viewer.hoverCount > 0 || viewer.sheet !== "" || !viewer.ready
    readonly property real progress: !viewer.story ? 0
                                   : viewer.story.kind === "video" && !viewer.failed ? frame.progress
                                   : Math.min(1, viewer.elapsed / (viewer.secondsFor(viewer.story) * 1000))
    readonly property var quickReactions: ["❤️", "😂", "😮", "😢", "👏", "🔥"]

    visible: viewer.stories !== null && viewer.stories.viewerOpen
    color: Theme.storyStage
    z: 50
    Accessible.role: Accessible.Dialog
    Accessible.name: viewer.group ? (viewer.own ? "Your story" : viewer.group.owner.handle + "'s story") : "Stories"

    /// A story with something to answer stays longer: reading a question and choosing takes more than five seconds.
    function secondsFor(story) {
        return !viewer.own && viewer.stories.interactiveEnabled && story.answerable ? viewer.stories.stickerStorySeconds : viewer.stories.storySeconds
    }
    function loadInteractions() {
        viewer.clockNow = Date.now()
        if (viewer.story && viewer.stories.interactiveEnabled && viewer.story.answerable) viewer.stories.loadInteractions(viewer.story.id)
    }
    function load() {
        if (!viewer.story) return
        if (viewer.loadedId === viewer.story.id) return
        viewer.loadedId = viewer.story.id
        viewer.mediaUrl = ""
        viewer.failed = false
        viewer.elapsed = 0
        viewer.interactions = null
        viewer.busySticker = ""
        viewer.typingSticker = ""
        viewer.hoverCount = 0
        viewer.loadInteractions()
        viewer.ready = viewer.story.kind === "text"
        if (viewer.story.hasMedia) viewer.stories.loadMedia(viewer.story.id)
        // The next photo, ahead of time.
        const upcoming = viewer.group.stories.length > viewer.storyIndex + 1 ? viewer.group.stories[viewer.storyIndex + 1]
                       : viewer.groups.length > viewer.groupIndex + 1 ? viewer.groups[viewer.groupIndex + 1].stories[0] : null
        if (upcoming && upcoming.kind === "image") viewer.stories.loadMedia(upcoming.id)
    }
    function firstUnseen(list) {
        for (let i = 0; i < list.length; i++) if (!list[i].seen) return i
        return 0
    }
    function next() {
        if (!viewer.group) return close()
        if (viewer.storyIndex + 1 < viewer.group.stories.length) viewer.storyIndex++
        else if (viewer.groupIndex + 1 < viewer.groups.length) {
            viewer.storyIndex = viewer.firstUnseen(viewer.groups[viewer.groupIndex + 1].stories)
            viewer.groupIndex++
        } else close()
    }
    function prev() {
        if (viewer.storyIndex > 0) viewer.storyIndex--
        else if (viewer.groupIndex > 0) {
            viewer.storyIndex = 0
            viewer.groupIndex--
        } else {
            viewer.elapsed = 0
            frame.restart()
        }
    }
    function close() {
        viewer.sheet = ""
        viewer.typing = false
        viewer.typingSticker = ""
        viewer.hoverCount = 0
        viewer.interactions = null
        viewer.pendingLink = null
        viewer.loadedId = ""
        viewer.stories.closeViewer()
    }

    onStoryChanged: viewer.load()
    onReadyChanged: {
        if (viewer.ready && !viewer.failed && viewer.story && !viewer.own) viewer.stories.markSeen(viewer.story.id)
    }
    onVisibleChanged: if (visible) keys.forceActiveFocus()

    Connections {
        target: viewer.stories
        function onViewerChanged() {
            if (!viewer.stories.viewerOpen) return
            viewer.loadedId = ""
            viewer.userPaused = false
            viewer.sheet = ""
            viewer.groupIndex = viewer.stories.viewerStartGroup
            viewer.storyIndex = viewer.stories.viewerStartStory
            viewer.load()
        }
        function onMediaReady(storyId, url) {
            if (viewer.story && storyId === viewer.story.id) viewer.mediaUrl = url
        }
        function onMediaFailed(storyId) {
            if (viewer.story && storyId === viewer.story.id) {
                viewer.failed = true
                viewer.ready = true
            }
        }
        function onViewersLoaded(storyId, list) {
            if (viewer.story && storyId === viewer.story.id) viewer.viewers = list
        }
        function onInteractionsLoaded(storyId, result) {
            if (viewer.story && storyId === viewer.story.id) viewer.interactions = result
        }
        function onAnswered(storyId, stickerId, mine) {
            if (!viewer.story || storyId !== viewer.story.id) return
            // Shown at once; the totals follow when the author's numbers have been fetched again.
            const next = viewer.interactions ? Object.assign({}, viewer.interactions) : { own: false, mine: {}, totals: {}, entries: [], unreadable: 0, answerCount: 0 }
            next.mine = Object.assign({}, next.mine)
            next.mine[stickerId] = mine
            viewer.interactions = next
            viewer.busySticker = ""
        }
        function onMessagesChanged() { viewer.busySticker = "" }
        function onToastChanged() {}
    }

    // Photos and text stories stay up for storySeconds (a story with stickers to answer, longer); videos for their length.
    Timer {
        interval: 50
        repeat: true
        running: viewer.visible && !viewer.paused && viewer.story !== null && (viewer.story.kind !== "video" || viewer.failed)
        onTriggered: {
            viewer.elapsed += interval
            if (viewer.elapsed >= viewer.secondsFor(viewer.story) * 1000) viewer.next()
        }
    }

    // Other people's answers and the author's totals change while a story is open.
    Timer {
        interval: 20000
        repeat: true
        running: viewer.visible && viewer.story !== null && viewer.story.answerable
        onTriggered: viewer.loadInteractions()
    }

    Item {
        id: keys
        anchors.fill: parent
        focus: true
        Keys.onPressed: (event) => {
            if (viewer.sheet !== "") {
                if (event.key === Qt.Key_Escape) { viewer.sheet = ""; event.accepted = true }
                return
            }
            if (event.key === Qt.Key_Right) { viewer.next(); event.accepted = true }
            else if (event.key === Qt.Key_Left) { viewer.prev(); event.accepted = true }
            else if (event.key === Qt.Key_Space) { viewer.userPaused = !viewer.userPaused; event.accepted = true }
            else if (event.key === Qt.Key_Escape) { viewer.close(); event.accepted = true }
        }
    }

    // Swallow clicks behind the viewer.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }

    RoundButton {
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: stage.left
        anchors.rightMargin: 16
        visible: viewer.groupIndex > 0 || viewer.storyIndex > 0
        width: 40
        height: 40
        Accessible.name: "Previous story"
        onClicked: viewer.prev()
        background: Rectangle { radius: 20; color: Theme.storyChrome }
        contentItem: Icon { name: "chevron-left"; size: 20; color: Theme.storyColors.black }
    }
    RoundButton {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: stage.right
        anchors.leftMargin: 16
        visible: viewer.group !== null && (viewer.groupIndex + 1 < viewer.groups.length || viewer.storyIndex + 1 < viewer.group.stories.length)
        width: 40
        height: 40
        Accessible.name: "Next story"
        onClicked: viewer.next()
        background: Rectangle { radius: 20; color: Theme.storyChrome }
        contentItem: Icon { name: "chevron-right"; size: 20; color: Theme.storyColors.black }
    }

    Item {
        id: stage
        anchors.centerIn: parent
        height: Math.min(viewer.height - 32, 900, (viewer.width - 32) * 16 / 9)
        width: height * 9 / 16
        clip: true

        StoryFrame {
            id: frame
            anchors.fill: parent
            radius: Theme.radiusCard
            story: viewer.story || ({})
            mediaUrl: viewer.mediaUrl
            muted: viewer.muted
            paused: viewer.paused
            onReady: viewer.ready = true
            onEnded: viewer.next()
            onFailed: {
                viewer.failed = true
                viewer.ready = true
            }
        }
        Text {
            anchors.centerIn: parent
            visible: viewer.failed
            text: "This story couldn't be opened."
            color: Theme.storyChrome
            font.pixelSize: Theme.fontSize
        }

        // Click right for the next story, left for the previous; hold to pause.
        MouseArea {
            anchors.fill: parent
            onPressed: {
                viewer.pressAt = Date.now()
                viewer.held = true
            }
            onReleased: (mouse) => {
                viewer.held = false
                if (Date.now() - viewer.pressAt > 220) return
                if (viewer.sheet !== "") { viewer.sheet = ""; return }
                if (mouse.x < width * 0.3) viewer.prev()
                else viewer.next()
            }
            onCanceled: viewer.held = false
        }

        // Stickers: a card to answer, or a countdown, link, mention, date or place. Above the tap area, so pressing one doesn't go to the next story.
        Repeater {
            model: viewer.story ? viewer.story.stickers : []
            delegate: StorySticker {
                id: stickerItem
                required property var modelData
                sticker: stickerItem.modelData
                frameWidth: stage.width
                own: viewer.own
                mine: viewer.interactions && viewer.interactions.mine[stickerItem.modelData.id] !== undefined ? viewer.interactions.mine[stickerItem.modelData.id] : null
                totals: viewer.interactions && viewer.interactions.totals[stickerItem.modelData.id] !== undefined ? viewer.interactions.totals[stickerItem.modelData.id] : null
                busy: viewer.busySticker === stickerItem.modelData.id
                canAnswer: viewer.stories.interactiveEnabled && viewer.live
                x: stickerItem.modelData.x * stage.width - stickerItem.width / 2
                y: stickerItem.modelData.y * stage.height - stickerItem.height / 2
                rotation: stickerItem.modelData.rotation
                scale: stickerItem.modelData.scale
                onAnswer: (given) => {
                    viewer.busySticker = stickerItem.modelData.id
                    viewer.stories.answer(viewer.story.id, stickerItem.modelData.id, given)
                }
                onLinkRequested: (url) => {
                    viewer.pendingLink = { url: url, host: stickerItem.modelData.host }
                    viewer.sheet = "link"
                }
                onHoveredChanged: viewer.hoverCount = Math.max(0, viewer.hoverCount + (stickerItem.hovered ? 1 : -1))
                onTypingChanged: {
                    if (stickerItem.typing) viewer.typingSticker = stickerItem.modelData.id
                    else if (viewer.typingSticker === stickerItem.modelData.id) viewer.typingSticker = ""
                }
            }
        }

        // --- Top: progress, who, when, controls ---
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            height: 84
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.storyScrim }
                GradientStop { position: 1.0; color: "transparent" }
            }
            Row {
                id: bars
                x: 10
                y: 10
                width: parent.width - 20
                spacing: 4
                Repeater {
                    model: viewer.group ? viewer.group.stories.length : 0
                    delegate: Rectangle {
                        id: bar
                        required property int index
                        width: (bars.width - bars.spacing * (viewer.group.stories.length - 1)) / viewer.group.stories.length
                        height: 3
                        radius: 1.5
                        color: Theme.storyTrack
                        Rectangle {
                            height: parent.height
                            radius: 1.5
                            color: Theme.storyChrome
                            width: parent.width * (bar.index < viewer.storyIndex ? 1 : bar.index === viewer.storyIndex ? viewer.progress : 0)
                        }
                    }
                }
            }
            RowLayout {
                x: 10
                y: 22
                width: parent.width - 20
                spacing: 8
                Avatar {
                    size: 32
                    initial: viewer.group ? viewer.group.owner.handle.charAt(0).toUpperCase() : "?"
                    tintKey: viewer.group ? viewer.group.owner.userId : ""
                    source: viewer.group ? viewer.group.owner.avatarUrl : ""
                }
                Text {
                    text: viewer.group ? (viewer.own ? "Your story" : viewer.group.owner.handle) : ""
                    textFormat: Text.PlainText
                    color: Theme.storyChrome
                    font.pixelSize: Theme.fontSize
                    font.weight: Font.DemiBold
                }
                Text {
                    text: (viewer.story ? viewer.story.age : "") + (viewer.stories.viewerTitle.length > 0 ? " · " + viewer.stories.viewerTitle : "")
                    textFormat: Text.PlainText
                    color: Theme.storyChrome
                    opacity: 0.75
                    font.pixelSize: Theme.fontSmall
                }
                Rectangle {
                    visible: viewer.story !== null && viewer.story.closeFriends
                    implicitWidth: closeLabel.implicitWidth + 14
                    implicitHeight: 20
                    radius: 10
                    color: Theme.storyRingClose
                    Text {
                        id: closeLabel
                        anchors.centerIn: parent
                        text: "★ Close friends"
                        color: Theme.storyColors.black
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                    }
                }
                Item { Layout.fillWidth: true }
                IconButton {
                    iconName: viewer.userPaused ? "play" : "pause"
                    iconColor: Theme.storyChrome
                    tip: viewer.userPaused ? "Play" : "Pause"
                    onClicked: viewer.userPaused = !viewer.userPaused
                }
                IconButton {
                    visible: viewer.story !== null && viewer.story.kind === "video"
                    iconName: viewer.muted ? "speaker-off" : "speaker"
                    iconColor: Theme.storyChrome
                    tip: viewer.muted ? "Turn sound on" : "Turn sound off"
                    onClicked: viewer.muted = !viewer.muted
                }
                IconButton {
                    iconName: "dots"
                    iconColor: Theme.storyChrome
                    tip: "More"
                    onClicked: viewer.sheet = viewer.sheet === "menu" ? "" : "menu"
                }
                IconButton {
                    iconName: "close"
                    iconColor: Theme.storyChrome
                    tip: "Close stories"
                    onClicked: viewer.close()
                }
            }
        }

        // --- Bottom: your activity and highlights, or a reply ---
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: viewer.own ? 64 : (quick.visible ? 120 : 76)
            visible: viewer.own || (viewer.story !== null && viewer.stories.canReply(viewer.story.id))
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: Theme.storyScrim }
            }
            RowLayout {
                visible: viewer.own
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 14
                spacing: 18
                RvButton {
                    secondary: true
                    iconName: "eye"
                    text: viewer.story && viewer.story.viewCount > 0 ? "Seen by " + viewer.story.viewCount : "Activity"
                    onClicked: {
                        viewer.viewers = []
                        viewer.sheet = "viewers"
                        viewer.stories.loadViewers(viewer.story.id)
                    }
                }
                RvButton {
                    visible: viewer.story !== null && viewer.story.answerable && viewer.stories.interactiveEnabled
                    secondary: true
                    iconName: "send"
                    text: viewer.interactions && viewer.interactions.own ? "Answers (" + viewer.interactions.answerCount + ")" : "Answers"
                    onClicked: {
                        viewer.loadInteractions()
                        viewer.sheet = "answers"
                    }
                }
                RvButton {
                    secondary: true
                    iconName: "heart"
                    text: "Highlight"
                    onClicked: {
                        viewer.stories.loadLibrary()
                        viewer.sheet = "highlight"
                    }
                }
            }
            ColumnLayout {
                visible: !viewer.own
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                spacing: 8
                Row {
                    id: quick
                    Layout.alignment: Qt.AlignHCenter
                    visible: replyField.activeFocus && replyField.text.length === 0
                    spacing: 10
                    Repeater {
                        model: viewer.quickReactions
                        delegate: AbstractButton {
                            id: reaction
                            required property string modelData
                            width: 40
                            height: 40
                            Accessible.name: "React " + reaction.modelData
                            onClicked: viewer.stories.reply(viewer.story.id, reaction.modelData, true)
                            contentItem: Text {
                                textFormat: Text.PlainText
                                text: reaction.modelData
                                font.pixelSize: reaction.hovered ? 32 : 28
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: null
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    TextField {
                        id: replyField
                        Layout.fillWidth: true
                        placeholderText: viewer.group ? "Reply to " + viewer.group.owner.handle + "…" : ""
                        placeholderTextColor: Theme.storyTrack
                        color: Theme.storyChrome
                        maximumLength: 2000
                        leftPadding: 16
                        rightPadding: 16
                        topPadding: 9
                        bottomPadding: 9
                        font.pixelSize: Theme.fontSize
                        onActiveFocusChanged: viewer.typing = activeFocus
                        background: Rectangle {
                            radius: height / 2
                            color: "transparent"
                            border.width: 1
                            border.color: Theme.storyChrome
                        }
                        Keys.onEscapePressed: keys.forceActiveFocus()
                        onAccepted: {
                            const t = text.trim()
                            if (t.length === 0) return
                            viewer.stories.reply(viewer.story.id, t, false)
                            text = ""
                            keys.forceActiveFocus()
                        }
                    }
                    IconButton {
                        iconName: replyField.text.trim().length > 0 ? "send" : "heart"
                        iconColor: Theme.storyChrome
                        tip: replyField.text.trim().length > 0 ? "Send reply" : "Like story"
                        onClicked: {
                            const t = replyField.text.trim()
                            viewer.stories.reply(viewer.story.id, t.length > 0 ? t : "❤️", t.length === 0)
                            replyField.text = ""
                            keys.forceActiveFocus()
                        }
                    }
                }
            }
        }

        // What just happened ("Reply sent").
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 92
            visible: toastText.text.length > 0
            width: toastText.implicitWidth + 28
            height: 32
            radius: 16
            color: Theme.storyScrim
            Text {
                id: toastText
                anchors.centerIn: parent
                text: viewer.stories && viewer.visible ? (viewer.stories.error.length > 0 ? viewer.stories.error : viewer.stories.toast) : ""
                textFormat: Text.PlainText
                color: Theme.storyChrome
                font.pixelSize: Theme.fontSmall
            }
            Timer {
                running: parent.visible
                interval: 2200
                onTriggered: viewer.stories.clearMessages()
            }
        }

        // --- More ---
        Rectangle {
            visible: viewer.sheet === "menu"
            anchors.right: parent.right
            anchors.rightMargin: 10
            y: 64
            width: 230
            height: menuColumn.implicitHeight + 12
            radius: Theme.radiusCard
            color: Theme.surface
            border.color: Theme.borderStrong
            ColumnLayout {
                id: menuColumn
                anchors.fill: parent
                anchors.margins: 6
                spacing: 0
                MenuRow {
                    visible: viewer.own && viewer.stories.viewerHighlightId.length > 0
                    text: "Remove from highlight"
                    onClicked: {
                        viewer.stories.removeFromHighlight(viewer.stories.viewerHighlightId, viewer.story.id)
                        viewer.close()
                    }
                }
                MenuRow {
                    visible: viewer.own
                    text: "Delete story"
                    danger: true
                    onClicked: {
                        viewer.sheet = ""
                        confirmDelete.open()
                    }
                }
                MenuRow {
                    visible: viewer.own
                    text: "Add to highlight"
                    onClicked: {
                        viewer.stories.loadLibrary()
                        viewer.sheet = "highlight"
                    }
                }
                MenuRow {
                    visible: viewer.own
                    text: "Story settings"
                    onClicked: {
                        viewer.close()
                        viewer.openSettings()
                    }
                }
                MenuRow {
                    visible: !viewer.own && viewer.group !== null
                    text: viewer.group && viewer.stories.isMuted(viewer.group.owner.userId) ? "Unmute " + viewer.group.owner.handle
                                                                                             : "Mute " + (viewer.group ? viewer.group.owner.handle : "") + "'s stories"
                    onClicked: {
                        viewer.stories.setMuted(viewer.group.owner.userId, !viewer.stories.isMuted(viewer.group.owner.userId))
                        viewer.sheet = ""
                    }
                }
                MenuRow {
                    visible: !viewer.own && viewer.group !== null
                    text: "Report story"
                    onClicked: viewer.sheet = "report"
                }
                MenuRow {
                    text: "Cancel"
                    onClicked: viewer.sheet = ""
                }
            }
        }

        // --- The author's answers ---
        StoryAnswersSheet {
            visible: viewer.sheet === "answers" && viewer.own
            stories: viewer.stories
            story: viewer.story
            interactions: viewer.interactions
            onClosed: viewer.sheet = ""
        }

        // --- Report this story ---
        StoryReportSheet {
            visible: viewer.sheet === "report" && !viewer.own
            stories: viewer.stories
            story: viewer.story
            onClosed: viewer.sheet = ""
        }

        // --- A link sticker: say where it goes before leaving ---
        StorySheet {
            visible: viewer.sheet === "link" && viewer.pendingLink !== null
            title: "Open this link?"
            closable: false
            Accessible.role: Accessible.AlertMessage
            Text {
                Layout.fillWidth: true
                text: "A friend's story links to " + (viewer.pendingLink ? viewer.pendingLink.host : "") + ". RedVault hasn't checked it."
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
            Text {
                Layout.fillWidth: true
                text: viewer.pendingLink ? viewer.pendingLink.url : ""
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                color: Theme.text2
                font.pixelSize: Theme.fontSmall
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                RvButton { secondary: true; text: "Stay here"; onClicked: viewer.sheet = "" }
                Item { Layout.fillWidth: true }
                RvButton {
                    text: "Open"
                    onClicked: {
                        const url = viewer.pendingLink.url
                        viewer.sheet = ""
                        Qt.openUrlExternally(url)
                    }
                }
            }
        }

        // --- Who saw it ---
        Sheet {
            visible: viewer.sheet === "viewers"
            title: viewer.viewers.length > 0 ? "Seen by " + viewer.viewers.length : "Activity"
            note: "Friends who turned off sharing views aren't listed. No times are kept."
            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 260)
                clip: true
                model: viewer.viewers
                delegate: RowLayout {
                    id: viewerRow
                    required property var modelData
                    width: ListView.view.width
                    height: 44
                    spacing: 10
                    Avatar {
                        size: 32
                        initial: viewerRow.modelData.handle.charAt(0).toUpperCase()
                        tintKey: viewerRow.modelData.userId
                        source: viewerRow.modelData.avatarUrl
                    }
                    Text {
                        Layout.fillWidth: true
                        text: viewerRow.modelData.handle
                        textFormat: Text.PlainText
                        color: Theme.text
                        elide: Text.ElideRight
                    }
                }
            }
        }

        // --- Add to a highlight ---
        Sheet {
            visible: viewer.sheet === "highlight"
            title: "Add to highlight"
            note: "Highlights stay on your profile after 24 hours, for friends who could see these stories."
            Repeater {
                model: viewer.stories.ownHighlights
                delegate: RowLayout {
                    id: highlightRow
                    required property var modelData
                    readonly property bool contains: highlightRow.modelData.stories.some(s => viewer.story && s.id === viewer.story.id)
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: highlightRow.modelData.title.length > 0 ? highlightRow.modelData.title : "Highlight"
                        textFormat: Text.PlainText
                        color: Theme.text
                        elide: Text.ElideRight
                    }
                    RvButton {
                        secondary: true
                        enabled: !highlightRow.contains
                        text: highlightRow.contains ? "Added" : "Add"
                        onClicked: {
                            viewer.stories.addToHighlight(highlightRow.modelData.id, viewer.story.id)
                            viewer.sheet = ""
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                RvTextField {
                    id: highlightName
                    Layout.fillWidth: true
                    placeholderText: "New highlight name"
                    maximumLength: 40
                    onAccepted: createButton.clicked()
                }
                RvButton {
                    id: createButton
                    text: "Create"
                    enabled: highlightName.text.trim().length > 0
                    onClicked: {
                        viewer.stories.saveHighlight("", highlightName.text, [viewer.story.id])
                        highlightName.text = ""
                        viewer.sheet = ""
                    }
                }
            }
        }
    }

    ConfirmDialog {
        id: confirmDelete
        title: "Delete this story?"
        message: "It's removed for everyone."
        confirmText: "Delete"
        danger: true
        onConfirmed: {
            const id = viewer.story.id
            viewer.stories.deleteStory(id)
            viewer.close()
        }
    }

    component MenuRow: ItemDelegate {
        id: menuRow
        property bool danger: false
        Layout.fillWidth: true
        implicitHeight: visible ? 40 : 0
        leftPadding: 12
        background: Rectangle {
            radius: Theme.radiusSmall
            color: menuRow.hovered ? Theme.surface2 : "transparent"
        }
        contentItem: Text {
            textFormat: Text.PlainText
            text: menuRow.text
            color: menuRow.danger ? Theme.danger : Theme.text
            font.pixelSize: Theme.fontSize
            verticalAlignment: Text.AlignVCenter
        }
    }

    component Sheet: Rectangle {
        id: sheetBox
        property string title
        property string note
        default property alias content: sheetColumn.data
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Math.min(parent.height * 0.7, sheetColumn.implicitHeight + 32)
        radius: Theme.radiusDialog
        color: Theme.surface
        MouseArea { anchors.fill: parent }
        ColumnLayout {
            id: sheetColumn
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: sheetBox.title
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                }
                IconButton {
                    iconName: "close"
                    tip: "Close"
                    onClicked: viewer.sheet = ""
                }
            }
            Text {
                Layout.fillWidth: true
                text: sheetBox.note
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
        }
    }
}
