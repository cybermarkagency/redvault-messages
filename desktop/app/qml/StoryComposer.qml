// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// Make a story: a photo, a video or just text, with text and stickers you can
// drag around (scroll over one to resize it). Photos are re-encoded before
// they leave this computer, which leaves their metadata behind.
Rectangle {
    id: composer
    required property StoriesSession stories
    signal openSettings()

    /// "pick", "media" or "text".
    property string mode: "pick"
    property url mediaFile: ""
    property string mediaKind: ""
    property string pickError: ""
    property string storyText: ""
    property string background: "sunset"
    property string font: "classic"
    property var overlays: []
    property int selected: -1
    /// "friends", "close_friends" or "custom" (the friends in customIds, one by one).
    property string audience: "friends"
    property var customIds: []
    property bool allowReplies: true
    /// How long it stays up, in seconds (one of stories.lifetimes).
    property int ttlSeconds: 86400
    /// Polls, quizzes, questions, sliders, countdowns, links, mentions, dates and places (sticker maps from StoriesSession.checkSticker).
    property var stickers: []
    property int selectedSticker: -1
    property bool stickerSheetOpen: false
    property bool stickerEditing: false
    property int stickerEditIndex: -1
    property bool peopleOpen: false
    /// The draft this story came from or was last saved as ("" for none); saving again replaces it, sharing deletes it.
    property string draftId: ""
    /// Leaving after the draft has been saved.
    property bool closeAfterSave: false
    property string notice: ""
    // The text being added or edited: index -1 adds one.
    property bool editingText: false
    property int editIndex: -1
    property string editColor: "white"
    property string editFont: "classic"
    property bool editBoxed: false
    readonly property bool busy: composer.stories !== null && composer.stories.postStage.length > 0
    readonly property bool dirty: composer.mode !== "pick" && (composer.mediaFile.toString().length > 0 || composer.storyText.trim().length > 0 || composer.overlays.length > 0 || composer.stickers.length > 0)
    /// Friends you can pick one by one (not the ones you hide your stories from).
    readonly property var pickable: {
        const out = []
        const list = composer.stories ? composer.stories.friends : []
        for (let i = 0; i < list.length; i++) if (list[i].isFriend && !list[i].hidden) out.push(list[i])
        return out
    }
    /// How many friends see it. (The listed counts are read too, so this follows them when they change.)
    readonly property int reach: composer.stories ? (composer.stories.audienceCount, composer.stories.closeFriendsCount, composer.stories.recipientCount(composer.audience, composer.customIds)) : 0
    readonly property string lifetimeText: {
        const list = composer.stories ? composer.stories.lifetimes : []
        for (let i = 0; i < list.length; i++) if (list[i].seconds === composer.ttlSeconds) return list[i].label
        return "24 hours"
    }
    readonly property bool interactiveOk: composer.stories !== null && composer.stories.interactiveEnabled
    readonly property var stickerChoices: [
        { type: "poll", label: "Poll", hint: "Two to four options", interactive: true },
        { type: "quiz", label: "Quiz", hint: "One right answer", interactive: true },
        { type: "question", label: "Question", hint: "Friends write back", interactive: true },
        { type: "slider", label: "Emoji slider", hint: "How much?", interactive: true },
        { type: "countdown", label: "Countdown", hint: "Counts down to a time", interactive: false },
        { type: "link", label: "Link", hint: "A web address", interactive: false },
        { type: "mention", label: "Mention", hint: "Tag a friend", interactive: false },
        { type: "date", label: "Date or time", hint: "Shows in each person's time zone", interactive: false },
        { type: "location", label: "Place", hint: "Typed by you, never read from your device", interactive: false }
    ]
    readonly property var fontLabels: ({ classic: "Classic", modern: "Modern", typewriter: "Typewriter", strong: "Strong" })

    visible: composer.stories !== null && composer.stories.composerOpen
    color: Theme.storyStage
    z: 50
    Accessible.role: Accessible.Dialog
    Accessible.name: "New story"

    function reset() {
        composer.mode = "pick"
        composer.mediaFile = ""
        composer.mediaKind = ""
        composer.pickError = ""
        composer.storyText = ""
        composer.background = "sunset"
        composer.font = "classic"
        composer.overlays = []
        composer.selected = -1
        composer.stickers = []
        composer.selectedSticker = -1
        composer.stickerSheetOpen = false
        composer.stickerEditing = false
        composer.peopleOpen = false
        composer.audience = "friends"
        composer.customIds = []
        composer.allowReplies = composer.stories ? composer.stories.allowReplies : true
        composer.ttlSeconds = 86400
        composer.editingText = false
        composer.draftId = ""
        composer.closeAfterSave = false
        composer.notice = ""
    }
    /// Saves what is on screen as a draft (over the one it came from), on this computer only.
    function keepDraft(thenLeave) {
        composer.closeAfterSave = thenLeave
        composer.stories.saveDraft(composer.draftId, {
            mode: composer.mode, text: composer.storyText, background: composer.background, font: composer.font,
            overlays: composer.overlays, stickers: composer.stickers, audience: composer.audience, customIds: composer.customIds,
            allowReplies: composer.allowReplies, ttlSeconds: composer.ttlSeconds,
            mediaFile: composer.mediaFile.toString(), mediaKind: composer.mediaKind
        }, preview.duration)
    }
    /// Puts a saved draft back on screen.
    function restore(d) {
        composer.reset()
        composer.mediaFile = d.mediaFile
        composer.mediaKind = d.mediaKind
        composer.storyText = d.text
        composer.background = d.background
        composer.font = d.font
        composer.overlays = d.overlays
        composer.stickers = d.stickers
        composer.audience = d.audience
        composer.customIds = d.customIds
        composer.allowReplies = d.allowReplies
        let known = false
        const choices = composer.stories.lifetimes
        for (let i = 0; i < choices.length; i++) if (choices[i].seconds === d.ttlSeconds) known = true
        composer.ttlSeconds = known ? d.ttlSeconds : 86400
        composer.draftId = d.id
        composer.mode = d.mode
        if (d.mode === "text") Qt.callLater(storyTextInput.forceActiveFocus)
    }
    onSelectedChanged: if (composer.selected >= 0) composer.selectedSticker = -1
    onSelectedStickerChanged: if (composer.selectedSticker >= 0) composer.selected = -1
    function close() {
        if (composer.busy) return
        if (composer.dirty) {
            leaveDialog.open()
            return
        }
        composer.stories.closeComposer()
    }
    function cycle(list, value) {
        return list[(list.indexOf(value) + 1) % list.length]
    }
    function addOverlay(o) {
        if (composer.overlays.length >= 30) return
        const next = composer.overlays.slice()
        next.push(Object.assign({ x: 0.5, y: 0.45, scale: 1, rotation: 0 }, o))
        composer.overlays = next
        composer.selected = next.length - 1
    }
    function updateOverlay(index, change) {
        const next = composer.overlays.slice()
        next[index] = Object.assign({}, next[index], change)
        composer.overlays = next
    }
    function removeOverlay(index) {
        const next = composer.overlays.slice()
        next.splice(index, 1)
        composer.overlays = next
        composer.selected = -1
    }
    function updateSticker(index, change) {
        const next = composer.stickers.slice()
        next[index] = Object.assign({}, next[index], change)
        composer.stickers = next
    }
    function removeSticker(index) {
        const next = composer.stickers.slice()
        next.splice(index, 1)
        composer.stickers = next
        composer.selectedSticker = -1
    }
    function editSticker(type, index) {
        composer.stickerEditIndex = index
        stickerEditor.open(type, index >= 0 ? composer.stickers[index] : null)
        composer.stickerSheetOpen = false
        composer.stickerEditing = true
    }
    function saveSticker(sticker) {
        const next = composer.stickers.slice()
        if (composer.stickerEditIndex < 0) {
            next.push(Object.assign({}, sticker, { y: 0.5 + 0.1 * (next.length % 3) }))
            composer.stickers = next
            composer.selectedSticker = next.length - 1
        } else {
            next[composer.stickerEditIndex] = sticker
            composer.stickers = next
        }
        composer.stickerEditing = false
    }
    function togglePerson(userId, on) {
        composer.customIds = on ? composer.customIds.concat([userId]) : composer.customIds.filter(id => id !== userId)
    }
    function startText(index) {
        const o = index >= 0 ? composer.overlays[index] : null
        textInput.text = o ? o.text : ""
        composer.editIndex = index
        composer.editColor = o ? o.color : "white"
        composer.editFont = o ? o.font : "classic"
        composer.editBoxed = o ? o.boxed : false
        composer.editingText = true
        textInput.forceActiveFocus()
    }
    function saveText() {
        const t = textInput.text.trim().substring(0, 300)
        const fields = { type: "text", text: t, color: composer.editColor, font: composer.editFont, boxed: composer.editBoxed }
        if (composer.editIndex < 0) {
            if (t.length > 0) composer.addOverlay(fields)
        } else if (t.length === 0) {
            composer.removeOverlay(composer.editIndex)
        } else {
            composer.updateOverlay(composer.editIndex, fields)
        }
        composer.editingText = false
    }
    function share() {
        if (composer.mode === "text") {
            composer.stories.postText(composer.storyText, composer.background, composer.font, composer.overlays, composer.stickers,
                                      composer.audience, composer.customIds, composer.allowReplies, composer.ttlSeconds)
        } else {
            composer.stories.postMedia(composer.mediaFile, composer.overlays, composer.stickers, composer.audience, composer.customIds,
                                       composer.allowReplies, composer.ttlSeconds, preview.duration)
        }
    }

    onVisibleChanged: if (visible) composer.reset()

    Connections {
        target: composer.stories
        function onPosted() {
            // What was shared no longer needs its draft.
            if (composer.draftId.length > 0) composer.stories.deleteDraft(composer.draftId)
            composer.reset()
        }
        function onDraftSaved(id) {
            composer.draftId = id
            composer.notice = "Draft saved on this device"
            noticeTimer.restart()
            if (composer.closeAfterSave) {
                composer.closeAfterSave = false
                composer.stories.closeComposer()
            }
        }
        function onDraftLoaded(draft) { composer.restore(draft) }
        function onMessagesChanged() {
            if (composer.stories.error.length > 0) composer.closeAfterSave = false
        }
    }
    Timer { id: noticeTimer; interval: 2500; onTriggered: composer.notice = "" }

    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }

    FileDialog {
        id: fileDialog
        title: "Add a photo or video"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Photos and videos (*.jpg *.jpeg *.png *.webp *.gif *.bmp *.mp4 *.webm *.mov)"]
        onAccepted: {
            const check = composer.stories.checkFile(selectedFile)
            composer.pickError = check.error
            if (check.error.length > 0) return
            composer.mediaFile = selectedFile
            composer.mediaKind = check.kind
            composer.mode = "media"
        }
    }

    RvDialog {
        id: leaveDialog
        title: "Leave this story?"
        width: 440
        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 18
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "A draft stays on this computer, encrypted, and is never sent to RedVault."
                color: Theme.text2
                font.pixelSize: Theme.fontSize
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                RvButton { primary: false; text: "Keep editing"; onClicked: leaveDialog.close() }
                RvButton {
                    primary: false
                    danger: true
                    text: "Discard"
                    onClicked: {
                        leaveDialog.close()
                        composer.stories.closeComposer()
                    }
                }
                RvButton {
                    text: "Save draft"
                    onClicked: {
                        leaveDialog.close()
                        composer.keepDraft(true)
                    }
                }
            }
        }
    }

    EmojiPicker {
        id: stickerPicker
        x: stage.x + stage.width - width - 10
        y: stage.y + 56
        onPicked: (emoji) => {
            composer.addOverlay({ type: "emoji", text: emoji, color: "white", font: "classic", boxed: false })
            stickerPicker.close()
        }
    }

    Item {
        id: stage
        anchors.centerIn: parent
        height: Math.min(composer.height - 32, 900, (composer.width - 32) * 16 / 9)
        width: height * 9 / 16

        // --- Pick what to post ---
        Rectangle {
            anchors.fill: parent
            visible: composer.mode === "pick"
            radius: Theme.radiusCard
            color: Theme.panel
            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width - 48
                spacing: 14
                Text {
                    text: "Add to your story"
                    color: Theme.text
                    font.family: Theme.displayFamily
                    font.pixelSize: 24
                    font.weight: Font.Bold
                }
                Text {
                    Layout.fillWidth: true
                    text: "Seen by the friends you choose, for as long as you choose (24 hours unless you pick another). End-to-end encrypted: the server can't see it."
                    wrapMode: Text.Wrap
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall
                }
                RvButton {
                    Layout.fillWidth: true
                    secondary: true
                    iconName: "image"
                    text: "Photo or video"
                    enabled: composer.stories !== null && composer.stories.mediaEnabled
                    onClicked: fileDialog.open()
                }
                Text {
                    Layout.fillWidth: true
                    visible: composer.stories !== null && !composer.stories.mediaEnabled
                    text: "Photo and video stories aren't available on this server yet."
                    wrapMode: Text.Wrap
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall
                }
                RvButton {
                    Layout.fillWidth: true
                    secondary: true
                    text: "Aa   Text"
                    onClicked: {
                        composer.mode = "text"
                        Qt.callLater(storyTextInput.forceActiveFocus)
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: composer.pickError.length > 0
                    text: composer.pickError
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.danger
                    font.pixelSize: Theme.fontSmall
                }
                Text {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: composer.stories !== null && composer.stories.drafts.length > 0
                    text: "Drafts"
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    visible: composer.stories !== null && composer.stories.drafts.length > 0
                    text: "Saved on this computer only, encrypted. They are never sent to RedVault."
                    wrapMode: Text.Wrap
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall
                }
                ListView {
                    id: draftList
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(draftList.contentHeight, 230)
                    visible: composer.stories !== null && composer.stories.drafts.length > 0
                    clip: true
                    spacing: 4
                    model: composer.stories ? composer.stories.drafts : []
                    delegate: RowLayout {
                        id: draftRow
                        required property var modelData
                        width: ListView.view.width
                        spacing: 6
                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.text2
                            font.pixelSize: Theme.fontSmall
                            text: (draftRow.modelData.preview.length > 0 ? draftRow.modelData.preview : draftRow.modelData.kind === "text" ? "Text story" : draftRow.modelData.kind)
                                  + (draftRow.modelData.stickerCount > 0 ? " · " + draftRow.modelData.stickerCount + (draftRow.modelData.stickerCount === 1 ? " sticker" : " stickers") : "")
                                  + " · " + new Date(draftRow.modelData.updatedAt).toLocaleDateString(Qt.locale(), Locale.ShortFormat)
                        }
                        RvButton {
                            secondary: true
                            text: "Continue"
                            implicitHeight: 32
                            enabled: !composer.busy
                            Accessible.name: "Continue this draft"
                            onClicked: composer.stories.openDraft(draftRow.modelData.id)
                        }
                        IconButton {
                            iconName: "trash"
                            tip: "Delete draft"
                            onClicked: composer.stories.deleteDraft(draftRow.modelData.id)
                        }
                    }
                }
            }
        }

        // --- The story as it will look ---
        StoryFrame {
            id: preview
            anchors.fill: parent
            visible: composer.mode !== "pick"
            radius: Theme.radiusCard
            editing: true
            muted: true
            story: ({ kind: composer.mode === "text" ? "text" : composer.mediaKind, background: composer.background, font: composer.font,
                      text: composer.storyText, overlays: [] })
            mediaUrl: composer.mode === "media" ? composer.mediaFile.toString() : ""
            onEnded: frameRestart.start()
            Timer { id: frameRestart; interval: 10; onTriggered: preview.restart() }

            MouseArea {
                anchors.fill: parent
                onPressed: {
                    composer.selected = -1
                    composer.selectedSticker = -1
                }
            }

            TextArea {
                id: storyTextInput
                visible: composer.mode === "text"
                anchors.centerIn: parent
                width: parent.width * 0.84
                height: parent.height * 0.6
                text: composer.storyText
                onTextChanged: composer.storyText = text.substring(0, 700)
                placeholderText: "Type something"
                placeholderTextColor: Theme.storyTrack
                wrapMode: TextEdit.Wrap
                horizontalAlignment: TextEdit.AlignHCenter
                verticalAlignment: TextEdit.AlignVCenter
                color: Theme.storyColors.white
                font.family: preview.fontFamily(composer.font)
                font.weight: composer.font === "typewriter" ? Font.Medium : composer.font === "classic" ? Font.DemiBold : Font.Bold
                font.capitalization: composer.font === "strong" ? Font.AllUppercase : Font.MixedCase
                font.pixelSize: Math.max(12, preview.width * 0.075)
                background: null
                Accessible.name: "Story text"
            }

            // Text and stickers: drag to move, scroll to resize.
            Repeater {
                model: composer.overlays
                delegate: Item {
                    id: overlayItem
                    required property var modelData
                    required property int index
                    // While dragged it follows the pointer here; the list changes once, on release (a changed list
                    // rebuilds every item, which dropped the drag after its first step).
                    property real dragX: -1
                    property real dragY: -1
                    x: (overlayItem.dragX >= 0 ? overlayItem.dragX : overlayItem.modelData.x) * preview.width - width / 2
                    y: (overlayItem.dragY >= 0 ? overlayItem.dragY : overlayItem.modelData.y) * preview.height - height / 2
                    width: overlayLabel.implicitWidth + (overlayItem.modelData.type === "text" ? preview.width * 0.05 : 0)
                    height: overlayLabel.implicitHeight + (overlayItem.modelData.type === "text" ? preview.width * 0.025 : 0)
                    rotation: overlayItem.modelData.rotation
                    scale: overlayItem.modelData.scale
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -4
                        color: "transparent"
                        border.width: composer.selected === overlayItem.index ? 2 : 0
                        border.color: Theme.storyChrome
                        radius: 6
                    }
                    Rectangle {
                        anchors.fill: parent
                        visible: overlayItem.modelData.type === "text" && overlayItem.modelData.boxed
                        radius: height * 0.25
                        color: preview.colorOf(overlayItem.modelData.color)
                    }
                    Text {
                        id: overlayLabel
                        anchors.centerIn: parent
                        width: Math.min(implicitWidth, preview.width * 0.86)
                        text: overlayItem.modelData.text
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                        color: overlayItem.modelData.type === "emoji" ? Theme.storyColors.white
                             : overlayItem.modelData.boxed ? preview.colorOf(Theme.storyLightColors.indexOf(overlayItem.modelData.color) >= 0 ? "black" : "white")
                             : preview.colorOf(overlayItem.modelData.color)
                        font.family: preview.fontFamily(overlayItem.modelData.font)
                        font.weight: overlayItem.modelData.font === "classic" ? Font.DemiBold : Font.Bold
                        font.capitalization: overlayItem.modelData.font === "strong" ? Font.AllUppercase : Font.MixedCase
                        font.pixelSize: Math.max(10, preview.width * (overlayItem.modelData.type === "emoji" ? 0.15 : 0.06))
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.OpenHandCursor
                        property point grab
                        Accessible.role: Accessible.Button
                        Accessible.name: overlayItem.modelData.type === "emoji" ? "Sticker " + overlayItem.modelData.text : "Text: " + overlayItem.modelData.text
                        onPressed: (mouse) => {
                            composer.selected = overlayItem.index
                            const p = mapToItem(preview, mouse.x, mouse.y)
                            grab = Qt.point(p.x / preview.width - overlayItem.modelData.x, p.y / preview.height - overlayItem.modelData.y)
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed) return
                            const p = mapToItem(preview, mouse.x, mouse.y)
                            overlayItem.dragX = Math.min(1, Math.max(0, p.x / preview.width - grab.x))
                            overlayItem.dragY = Math.min(1, Math.max(0, p.y / preview.height - grab.y))
                        }
                        onReleased: {
                            if (overlayItem.dragX < 0) return
                            const fx = overlayItem.dragX
                            const fy = overlayItem.dragY
                            overlayItem.dragX = -1
                            overlayItem.dragY = -1
                            composer.updateOverlay(overlayItem.index, { x: fx, y: fy })
                        }
                        onCanceled: {
                            overlayItem.dragX = -1
                            overlayItem.dragY = -1
                        }
                        onDoubleClicked: if (overlayItem.modelData.type === "text") composer.startText(overlayItem.index)
                        onWheel: (wheel) => {
                            const s = overlayItem.modelData.scale * (wheel.angleDelta.y > 0 ? 1.08 : 0.92)
                            composer.updateOverlay(overlayItem.index, { scale: Math.min(4, Math.max(0.5, s)) })
                        }
                    }
                }
            }

            // Stickers: a preview of each, nothing to press; drag to move, scroll to resize, double-click to edit.
            Repeater {
                model: composer.stickers
                delegate: Item {
                    id: stickerItem
                    required property var modelData
                    required property int index
                    // While dragged the sticker follows the pointer here; the list changes once, on release (a changed list
                    // rebuilds every sticker, which would drop the drag).
                    property real dragX: -1
                    property real dragY: -1
                    x: (stickerItem.dragX >= 0 ? stickerItem.dragX : stickerItem.modelData.x) * preview.width - width / 2
                    y: (stickerItem.dragY >= 0 ? stickerItem.dragY : stickerItem.modelData.y) * preview.height - height / 2
                    width: stickerCard.width
                    height: stickerCard.height
                    rotation: stickerItem.modelData.rotation
                    scale: stickerItem.modelData.scale
                    StorySticker {
                        id: stickerCard
                        sticker: stickerItem.modelData
                        frameWidth: preview.width
                        mode: "edit"
                        enabled: false
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -4
                        color: "transparent"
                        border.width: composer.selectedSticker === stickerItem.index ? 2 : 0
                        border.color: Theme.storyChrome
                        radius: 8
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.OpenHandCursor
                        property point grab
                        Accessible.role: Accessible.Button
                        Accessible.name: stickerCard.label() + ". Drag to move; double-click to edit."
                        onPressed: (mouse) => {
                            composer.selectedSticker = stickerItem.index
                            const p = mapToItem(preview, mouse.x, mouse.y)
                            grab = Qt.point(p.x / preview.width - stickerItem.modelData.x, p.y / preview.height - stickerItem.modelData.y)
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed) return
                            const p = mapToItem(preview, mouse.x, mouse.y)
                            stickerItem.dragX = Math.min(1, Math.max(0, p.x / preview.width - grab.x))
                            stickerItem.dragY = Math.min(1, Math.max(0, p.y / preview.height - grab.y))
                        }
                        onReleased: {
                            if (stickerItem.dragX < 0) return
                            const fx = stickerItem.dragX
                            const fy = stickerItem.dragY
                            stickerItem.dragX = -1
                            stickerItem.dragY = -1
                            composer.updateSticker(stickerItem.index, { x: fx, y: fy })
                        }
                        onCanceled: {
                            stickerItem.dragX = -1
                            stickerItem.dragY = -1
                        }
                        onDoubleClicked: composer.editSticker(stickerItem.modelData.type, stickerItem.index)
                        onWheel: (wheel) => {
                            const s = stickerItem.modelData.scale * (wheel.angleDelta.y > 0 ? 1.08 : 0.92)
                            composer.updateSticker(stickerItem.index, { scale: Math.min(2.5, Math.max(0.6, s)) })
                        }
                    }
                }
            }
        }

        // --- Tools ---
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            height: toolsColumn.implicitHeight + 20
            color: "transparent"
            visible: composer.mode !== "pick"
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.storyScrim }
                GradientStop { position: 1.0; color: "transparent" }
            }
            ColumnLayout {
                id: toolsColumn
                x: 10
                y: 10
                width: parent.width - 20
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    IconButton {
                        iconName: "close"
                        iconColor: Theme.storyChrome
                        tip: "Close"
                        onClicked: composer.close()
                    }
                    Item { Layout.fillWidth: true }
                    IconButton {
                        visible: composer.mode === "text"
                        iconName: "refresh"
                        iconColor: Theme.storyChrome
                        tip: "Change background"
                        onClicked: composer.background = composer.cycle(composer.stories.backgrounds, composer.background)
                    }
                    ToolText {
                        visible: composer.mode === "text"
                        text: composer.fontLabels[composer.font]
                        tip: "Font"
                        onClicked: composer.font = composer.cycle(composer.stories.fonts, composer.font)
                    }
                    ToolText {
                        text: "Aa"
                        tip: "Add text"
                        onClicked: composer.startText(-1)
                    }
                    IconButton {
                        iconName: "smile"
                        iconColor: Theme.storyChrome
                        tip: "Add an emoji"
                        onClicked: stickerPicker.open()
                    }
                    ToolText {
                        text: "Stickers"
                        tip: "Add a poll, question, countdown and more"
                        onClicked: composer.stickerSheetOpen = true
                    }
                    ToolText {
                        text: "Draft"
                        tip: "Save as a draft on this computer"
                        enabled: !composer.busy
                        onClicked: composer.keepDraft(false)
                    }
                }
                Row {
                    Layout.alignment: Qt.AlignHCenter
                    visible: composer.selected >= 0 && composer.selected < composer.overlays.length && !composer.editingText
                    spacing: 6
                    ToolText { text: "A−"; tip: "Smaller"; onClicked: composer.updateOverlay(composer.selected, { scale: Math.max(0.5, composer.overlays[composer.selected].scale / 1.2) }) }
                    ToolText { text: "A+"; tip: "Bigger"; onClicked: composer.updateOverlay(composer.selected, { scale: Math.min(4, composer.overlays[composer.selected].scale * 1.2) }) }
                    ToolText { text: "⟲"; tip: "Rotate left"; onClicked: composer.updateOverlay(composer.selected, { rotation: (composer.overlays[composer.selected].rotation + 345) % 360 }) }
                    ToolText { text: "⟳"; tip: "Rotate right"; onClicked: composer.updateOverlay(composer.selected, { rotation: (composer.overlays[composer.selected].rotation + 15) % 360 }) }
                    ToolText {
                        visible: composer.selected >= 0 && composer.selected < composer.overlays.length && composer.overlays[composer.selected].type === "text"
                        text: "Edit"
                        tip: "Edit text"
                        onClicked: composer.startText(composer.selected)
                    }
                    IconButton {
                        iconName: "trash"
                        iconColor: Theme.storyChrome
                        tip: "Remove"
                        onClicked: composer.removeOverlay(composer.selected)
                    }
                }
                Row {
                    Layout.alignment: Qt.AlignHCenter
                    visible: composer.selectedSticker >= 0 && composer.selectedSticker < composer.stickers.length && !composer.stickerEditing
                    spacing: 6
                    ToolText { text: "A−"; tip: "Smaller sticker"; onClicked: composer.updateSticker(composer.selectedSticker, { scale: Math.max(0.6, composer.stickers[composer.selectedSticker].scale / 1.15) }) }
                    ToolText { text: "A+"; tip: "Bigger sticker"; onClicked: composer.updateSticker(composer.selectedSticker, { scale: Math.min(2.5, composer.stickers[composer.selectedSticker].scale * 1.15) }) }
                    ToolText { text: "⟲"; tip: "Rotate sticker left"; onClicked: composer.updateSticker(composer.selectedSticker, { rotation: (composer.stickers[composer.selectedSticker].rotation + 351) % 360 }) }
                    ToolText { text: "⟳"; tip: "Rotate sticker right"; onClicked: composer.updateSticker(composer.selectedSticker, { rotation: (composer.stickers[composer.selectedSticker].rotation + 9) % 360 }) }
                    ToolText { text: "Edit"; tip: "Edit sticker"; onClicked: composer.editSticker(composer.stickers[composer.selectedSticker].type, composer.selectedSticker) }
                    IconButton {
                        iconName: "trash"
                        iconColor: Theme.storyChrome
                        tip: "Remove sticker"
                        onClicked: composer.removeSticker(composer.selectedSticker)
                    }
                }
            }
        }

        // --- Adding or editing text ---
        Rectangle {
            anchors.fill: parent
            visible: composer.editingText
            color: Theme.storyScrim
            radius: Theme.radiusCard
            MouseArea { anchors.fill: parent }
            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width - 40
                spacing: 14
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 130
                    radius: Theme.radiusSmall
                    color: composer.editBoxed ? preview.colorOf(composer.editColor) : "transparent"
                    border.width: 1
                    border.color: Theme.storyTrack
                    TextArea {
                        id: textInput
                        anchors.fill: parent
                        wrapMode: TextEdit.Wrap
                        horizontalAlignment: TextEdit.AlignHCenter
                        verticalAlignment: TextEdit.AlignVCenter
                        color: composer.editBoxed ? preview.colorOf(Theme.storyLightColors.indexOf(composer.editColor) >= 0 ? "black" : "white")
                                                  : preview.colorOf(composer.editColor)
                        font.family: preview.fontFamily(composer.editFont)
                        font.weight: composer.editFont === "classic" ? Font.DemiBold : Font.Bold
                        font.capitalization: composer.editFont === "strong" ? Font.AllUppercase : Font.MixedCase
                        font.pixelSize: 24
                        background: null
                        Accessible.name: "Text to add"
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    Repeater {
                        model: composer.stories ? composer.stories.colors : []
                        delegate: AbstractButton {
                            id: swatch
                            required property string modelData
                            width: 26
                            height: 26
                            Accessible.name: swatch.modelData
                            checkable: true
                            checked: composer.editColor === swatch.modelData
                            onClicked: composer.editColor = swatch.modelData
                            background: Rectangle {
                                radius: 13
                                color: preview.colorOf(swatch.modelData)
                                border.width: swatch.checked ? 3 : 2
                                border.color: Theme.storyChrome
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    RvButton {
                        secondary: true
                        text: composer.fontLabels[composer.editFont]
                        onClicked: composer.editFont = composer.cycle(composer.stories.fonts, composer.editFont)
                    }
                    RvButton {
                        secondary: true
                        text: composer.editBoxed ? "Box on" : "Box off"
                        onClicked: composer.editBoxed = !composer.editBoxed
                    }
                    Item { Layout.fillWidth: true }
                    RvButton {
                        text: "Done"
                        onClicked: composer.saveText()
                    }
                }
            }
        }

        // --- Stickers: what to add ---
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Math.min(parent.height * 0.8, stickerChoicesColumn.implicitHeight + 32)
            visible: composer.stickerSheetOpen
            radius: Theme.radiusDialog
            color: Theme.surface
            Accessible.role: Accessible.Dialog
            Accessible.name: "Add a sticker"
            MouseArea { anchors.fill: parent }
            ColumnLayout {
                id: stickerChoicesColumn
                anchors.fill: parent
                anchors.margins: 16
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: "Stickers"; color: Theme.text; font.pixelSize: Theme.fontTitle; font.weight: Font.DemiBold }
                    IconButton { iconName: "close"; tip: "Close stickers"; onClicked: composer.stickerSheetOpen = false }
                }
                Text {
                    Layout.fillWidth: true
                    visible: !composer.interactiveOk
                    text: "Polls, quizzes, questions and sliders aren't available on this server."
                    wrapMode: Text.Wrap
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall
                }
                Text {
                    Layout.fillWidth: true
                    visible: composer.stickers.length >= 12
                    text: "At most 12 stickers on one story."
                    color: Theme.danger
                    font.pixelSize: Theme.fontSmall
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 8
                    rowSpacing: 8
                    Repeater {
                        model: composer.stickerChoices.filter(c => composer.interactiveOk || !c.interactive)
                        delegate: AbstractButton {
                            id: choice
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            implicitHeight: 58
                            enabled: composer.stickers.length < 12
                            Accessible.name: choice.modelData.label + ": " + choice.modelData.hint
                            background: Rectangle {
                                radius: Theme.radiusSmall
                                color: choice.hovered ? Theme.surface3 : Theme.surface2
                                border.width: 1
                                border.color: Theme.borderStrong
                                opacity: choice.enabled ? 1 : 0.5
                            }
                            contentItem: Column {
                                leftPadding: 10
                                topPadding: 8
                                spacing: 2
                                Text { textFormat: Text.PlainText; text: choice.modelData.label; color: Theme.text; font.pixelSize: Theme.fontSize; font.weight: Font.DemiBold }
                                Text { textFormat: Text.PlainText; text: choice.modelData.hint; width: choice.width - 20; elide: Text.ElideRight; color: Theme.text3; font.pixelSize: 11 }
                            }
                            onClicked: composer.editSticker(choice.modelData.type, -1)
                        }
                    }
                }
            }
        }

        StickerEditor {
            id: stickerEditor
            anchors.fill: parent
            stories: composer.stories
            visible: composer.stickerEditing
            onSaved: (sticker) => composer.saveSticker(sticker)
            onCanceled: composer.stickerEditing = false
        }

        // --- Choose people, one by one ---
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Math.min(parent.height * 0.8, peopleColumn.implicitHeight + 32)
            visible: composer.peopleOpen
            radius: Theme.radiusDialog
            color: Theme.surface
            Accessible.role: Accessible.Dialog
            Accessible.name: "Choose who sees this story"
            MouseArea { anchors.fill: parent }
            ColumnLayout {
                id: peopleColumn
                anchors.fill: parent
                anchors.margins: 16
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: "Choose people"; color: Theme.text; font.pixelSize: Theme.fontTitle; font.weight: Font.DemiBold }
                    IconButton { iconName: "close"; tip: "Done choosing"; onClicked: composer.peopleOpen = false }
                }
                Text {
                    Layout.fillWidth: true
                    text: "Only these friends get this story's key. Friends you add later won't see it."
                    wrapMode: Text.Wrap
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall
                }
                Text {
                    Layout.fillWidth: true
                    visible: composer.pickable.length === 0
                    text: "You have no friends to choose yet."
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(contentHeight, 240)
                    clip: true
                    model: composer.pickable
                    delegate: CheckBox {
                        id: person
                        required property var modelData
                        width: ListView.view.width
                        checked: composer.customIds.indexOf(person.modelData.userId) >= 0
                        text: person.modelData.handle
                        onToggled: composer.togglePerson(person.modelData.userId, checked)
                        Accessible.name: person.modelData.handle
                        contentItem: Text {
                            leftPadding: person.indicator.width + 8
                            text: person.modelData.handle
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.text
                            font.pixelSize: Theme.fontSize
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    RvButton { secondary: true; text: "Everyone"; onClicked: composer.customIds = composer.pickable.map(f => f.userId) }
                    RvButton { secondary: true; text: "No one"; onClicked: composer.customIds = [] }
                    Item { Layout.fillWidth: true }
                    RvButton { text: "Done"; onClicked: composer.peopleOpen = false }
                }
            }
        }

        // --- Who sees it, how long, and share ---
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: shareColumn.implicitHeight + 24
            visible: composer.mode !== "pick" && !composer.editingText && !composer.stickerEditing && !composer.peopleOpen && !composer.stickerSheetOpen
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: Theme.storyScrim }
            }
            ColumnLayout {
                id: shareColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    visible: composer.stories !== null && composer.stories.error.length > 0
                    text: composer.stories ? composer.stories.error : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.danger
                    font.pixelSize: Theme.fontSmall
                }
                Text {
                    Layout.fillWidth: true
                    visible: composer.notice.length > 0
                    text: composer.notice
                    textFormat: Text.PlainText
                    color: Theme.storyChrome
                    font.pixelSize: Theme.fontSmall
                    font.weight: Font.DemiBold
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Pill {
                        text: "Your story · " + (composer.stories ? composer.stories.audienceCount : 0)
                        active: composer.audience === "friends"
                        onClicked: composer.audience = "friends"
                    }
                    Pill {
                        text: "★ Close friends · " + (composer.stories ? composer.stories.closeFriendsCount : 0)
                        active: composer.audience === "close_friends"
                        close: true
                        enabled: composer.stories !== null && composer.stories.closeFriendsCount > 0
                        onClicked: composer.audience = "close_friends"
                    }
                    Pill {
                        text: "Choose people" + (composer.audience === "custom" ? " · " + composer.reach : "")
                        active: composer.audience === "custom"
                        enabled: composer.pickable.length > 0
                        onClicked: {
                            composer.audience = "custom"
                            composer.peopleOpen = true
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    CheckBox {
                        id: repliesBox
                        text: "Replies"
                        checked: composer.allowReplies
                        onToggled: composer.allowReplies = checked
                        contentItem: Text {
                            leftPadding: repliesBox.indicator.width + 4
                            text: "Replies"
                            color: Theme.storyChrome
                            font.pixelSize: Theme.fontSmall
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Text {
                        text: "Disappears after"
                        color: Theme.storyChrome
                        font.pixelSize: Theme.fontSmall
                    }
                    StoryCombo {
                        id: lifetimeCombo
                        objectName: "lifetimeCombo"
                        implicitWidth: 110
                        implicitHeight: 32
                        model: composer.stories ? composer.stories.lifetimes : []
                        textRole: "label"
                        currentIndex: {
                            const list = composer.stories ? composer.stories.lifetimes : []
                            for (let i = 0; i < list.length; i++) if (list[i].seconds === composer.ttlSeconds) return i
                            return -1
                        }
                        onActivated: (index) => composer.ttlSeconds = composer.stories.lifetimes[index].seconds
                        Accessible.name: "Disappears after"
                    }
                    Item { Layout.fillWidth: true }
                    RvButton {
                        text: composer.stories && composer.stories.postStage === "preparing" ? "Preparing…"
                            : composer.stories && composer.stories.postStage === "encrypting" ? "Sharing…" : "Share"
                        enabled: !composer.busy && (composer.mode !== "text" || composer.storyText.trim().length > 0) && (composer.audience !== "custom" || composer.reach > 0)
                        onClicked: composer.share()
                    }
                }
                Text {
                    Layout.fillWidth: true
                    objectName: "storySummary"
                    text: (composer.audience === "custom" ? "Only the " + composer.reach + (composer.reach === 1 ? " friend" : " friends") + " you chose"
                                                           : composer.reach + (composer.reach === 1 ? " friend" : " friends"))
                          + " can see this · gone after " + composer.lifetimeText + " · end-to-end encrypted. Anyone who sees it can screenshot it."
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.storyChrome
                    opacity: 0.8
                    font.pixelSize: Theme.fontSmall
                }
                Text {
                    Layout.fillWidth: true
                    visible: composer.stories !== null && composer.stories.closeFriendsCount === 0
                    text: "<a href=\"#\">Choose close friends</a>"
                    textFormat: Text.StyledText
                    linkColor: Theme.storyChrome
                    color: Theme.storyChrome
                    font.pixelSize: Theme.fontSmall
                    onLinkActivated: {
                        composer.stories.closeComposer()
                        composer.openSettings()
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: composer.mediaKind === "video" && composer.mode === "media"
                    text: "Videos are shared as they are, including any details your camera saved in the file."
                    wrapMode: Text.Wrap
                    color: Theme.storyChrome
                    opacity: 0.8
                    font.pixelSize: Theme.fontSmall
                }
            }
        }

        IconButton {
            visible: composer.mode === "pick"
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 10
            iconName: "close"
            tip: "Close"
            onClicked: composer.close()
        }
    }

    component ToolText: AbstractButton {
        id: tool
        property string tip
        implicitWidth: Math.max(34, toolLabel.implicitWidth + 16)
        implicitHeight: 32
        Accessible.name: tool.tip.length > 0 ? tool.tip : tool.text
        ToolTip.visible: hovered && tool.tip.length > 0
        ToolTip.text: tool.tip
        background: Rectangle {
            radius: height / 2
            color: tool.hovered ? Theme.storyScrim : "transparent"
        }
        contentItem: Text {
            textFormat: Text.PlainText
            id: toolLabel
            text: tool.text
            color: Theme.storyChrome
            font.pixelSize: Theme.fontSize
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    component Pill: AbstractButton {
        id: pill
        property bool active: false
        property bool close: false
        implicitHeight: 30
        implicitWidth: pillLabel.implicitWidth + 22
        opacity: enabled ? 1 : 0.5
        Accessible.role: Accessible.RadioButton
        Accessible.checked: pill.active
        background: Rectangle {
            radius: height / 2
            color: pill.active ? (pill.close ? Theme.storyRingClose : Theme.storyChrome) : Theme.storyScrim
            border.width: 1
            border.color: pill.close ? Theme.storyRingClose : Theme.storyTrack
        }
        contentItem: Text {
            textFormat: Text.PlainText
            id: pillLabel
            text: pill.text
            color: pill.active ? Theme.storyColors.black : Theme.storyChrome
            font.pixelSize: Theme.fontSmall
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
