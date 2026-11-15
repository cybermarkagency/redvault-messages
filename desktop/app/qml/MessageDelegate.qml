// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// One message: optional day header, then a bubble (right for yours, left for
// theirs) with the quote it replies to, the text, time and delivery state,
// and reactions. Everything shown from a message is plain text.
Item {
    id: root
    required property string messageId
    required property string text
    required property bool fromMe
    required property string sender
    required property bool showSender
    required property string timeText
    required property string dayHeader
    required property string kind
    required property string fileSize
    required property string fileStatus
    required property string status
    /// Tor outbox: why it hasn't gone yet, and whether it waits for the person (Retry / Discard).
    required property string queueError
    required property bool queueHeld
    required property string replyPreview
    required property bool replyFromMe
    required property var reactions
    required property string verification
    required property bool edited
    required property bool forwarded
    required property bool unverifiedSender
    required property string storyReply
    required property string richText
    required property string richTextRevealed
    required property bool isImage
    required property bool isAudio
    required property var poll
    required property string noticeText
    // Forwarded from a channel (all empty otherwise).
    required property string channelId
    required property string channelName
    required property string channelPostId
    // A forwarded channel post counts as a view of the original (each computer counts a post once).
    onChannelPostIdChanged: countChannelView()
    Component.onCompleted: countChannelView()
    function countChannelView() {
        if (root.channelPostId.length > 0 && App.channels) App.channels.countViews([root.channelPostId])
    }
    // For pictures: {state, width, height} from the chat session (the thread passes it in).
    property var imageInfo: ({})
    readonly property string imageState: imageInfo && imageInfo.state ? imageInfo.state : ""
    // For voice messages: {state, url} from the chat session.
    property var audioInfo: ({})
    signal audioLoadRequested(string messageId)
    // A spoiler in this message was clicked: show them all.
    property bool spoilersShown: false

    /// The current search match: outlined, with the matching words selected.
    property bool highlighted: false
    /// Selecting messages: a click anywhere on the row ticks it.
    property bool selectMode: false
    property bool selected: false
    signal selectToggled(string messageId)
    property string searchTerm: ""

    signal replyRequested(string messageId)
    signal reactRequested(string messageId, string emoji)
    /// "+" in the reaction bar: react with any emoji.
    signal reactPickerRequested(string messageId)
    signal fileRequested(string messageId, string name)
    /// "edited" was clicked: show the earlier versions.
    signal historyRequested(string messageId)
    /// A link in the text was clicked (the thread asks before opening it).
    signal linkRequested(string url)
    /// A poll option was picked: the new set of option indexes (empty takes the vote back).
    signal voteRequested(string messageId, var options)
    signal endPollRequested(string messageId)
    /// A picture should load (it came into view, or the user asked for a big one).
    signal imageLoadRequested(string messageId, bool force)
    /// A loaded picture was clicked: show it full size.
    signal imageOpened(string messageId)
    /// The "more" button or a right-click: the thread shows its message menu.
    signal menuRequested(string messageId, bool fromMe, string kind, string text)
    /// A queued (outbox) message: send again now, or drop it (it never reached the server).
    signal retryQueuedRequested(string messageId)
    signal discardQueuedRequested(string messageId)

    // The quick reactions (model/rich_message.hpp kReactionEmoji); "+" offers any other emoji.
    readonly property var reactionChoices: ["👍", "❤️", "😂", "😮", "😢", "🙏"]
    readonly property int maxBubbleWidth: Math.min(560, width * 0.72)

    implicitHeight: column.implicitHeight + 4

    HoverHandler {
        id: hover
    }

    // Selected: the row is tinted and ticked.
    Rectangle {
        anchors.fill: parent
        visible: root.selectMode && root.selected
        color: Qt.alpha(Theme.accent, 0.08)
    }
    Rectangle {
        visible: root.selectMode
        x: 16
        y: column.y + bubbleRow.y + bubbleRow.height / 2 - height / 2
        width: 22
        height: 22
        radius: 11
        color: root.selected ? Theme.accentFill : "transparent"
        border.color: root.selected ? Theme.accentFill : Theme.borderStrong
        border.width: 2
        Icon {
            anchors.centerIn: parent
            visible: root.selected
            name: "check"
            size: 14
            color: Theme.accentContrast
        }
    }
    // Above everything else while selecting, so text selection and menus don't get the click.
    MouseArea {
        anchors.fill: parent
        z: 100
        visible: root.selectMode
        cursorShape: Qt.PointingHandCursor
        onClicked: root.selectToggled(root.messageId)
    }

    ColumnLayout {
        id: column
        width: parent.width
        spacing: 4

        // Day header
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 10
            Layout.bottomMargin: 6
            visible: root.dayHeader.length > 0
            implicitWidth: dayText.implicitWidth + 20
            implicitHeight: 24
            radius: 12
            color: Theme.surface2
            Text {
                id: dayText
                anchors.centerIn: parent
                text: root.dayHeader
                textFormat: Text.PlainText
                color: Theme.text2
                font.pixelSize: Theme.fontSmall
            }
        }

        // Something that happened (a pin): a small line, not a bubble.
        Text {
            visible: root.kind === "notice"
            Layout.fillWidth: true
            Layout.leftMargin: 40
            Layout.rightMargin: 40
            Layout.topMargin: 2
            Layout.bottomMargin: 2
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("\u{1F4CC} ") + root.noticeText + qsTr(" \u00B7 ") + root.timeText
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.text3
            font.pixelSize: Theme.fontSmall
        }

        RowLayout {
            id: bubbleRow
            visible: root.kind !== "notice"
            Layout.fillWidth: true
            Layout.leftMargin: root.selectMode ? 52 : 18
            Layout.rightMargin: 18
            layoutDirection: root.fromMe ? Qt.RightToLeft : Qt.LeftToRight
            spacing: 6

            Rectangle {
                id: bubble
                Layout.maximumWidth: root.maxBubbleWidth
                implicitWidth: Math.min(root.maxBubbleWidth, bubbleColumn.implicitWidth + 24)
                implicitHeight: bubbleColumn.implicitHeight + 16
                radius: Theme.radiusBubble
                // Burgundy for yours, charcoal for theirs; a warning tint for one that
                // couldn't be decrypted.
                readonly property bool onFill: root.fromMe && root.kind !== "undecryptable"
                color: root.kind === "undecryptable" ? Theme.warnSoft
                     : root.fromMe ? Theme.bubbleOut : Theme.bubbleIn
                border.width: root.highlighted ? 2 : (bubble.onFill ? 0 : 1)
                border.color: root.highlighted ? (bubble.onFill ? Theme.bubbleOutText : Theme.focusRing)
                            : root.kind === "undecryptable" ? Theme.warnBorder : Theme.bubbleInBorder

                readonly property color ink: root.kind === "undecryptable" ? Theme.warn
                                           : bubble.onFill ? Theme.bubbleOutText : Theme.text
                readonly property color inkSoft: bubble.onFill ? Theme.bubbleOutMeta : Theme.text3
                /// Links, mentions and accents inside the bubble: white on burgundy.
                readonly property color accentInk: bubble.onFill ? Theme.bubbleOutText : Theme.accent
                readonly property color dangerInk: bubble.onFill ? "#FFD9CF" : Theme.danger

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: root.menuRequested(root.messageId, root.fromMe, root.kind, root.text)
                }

                ColumnLayout {
                    id: bubbleColumn
                    anchors.fill: parent
                    anchors.margins: 8
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 4

                    Text {
                        visible: root.showSender
                        text: root.sender
                        textFormat: Text.PlainText
                        color: bubble.accentInk
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.DemiBold
                    }

                    // No signature where their apps now sign: an older app of theirs, or someone else made it.
                    Text {
                        visible: root.unverifiedSender
                        text: qsTr("⚠ Sender not verified")
                        textFormat: Text.PlainText
                        color: bubble.dangerInk
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.DemiBold
                        Accessible.name: "Sender not verified: this message has no signature, so it may come from an older app of theirs or from someone else"
                    }

                    // Forwarded from another chat, or from a channel (click to open it).
                    Row {
                        visible: root.forwarded || root.channelName.length > 0
                        spacing: 4
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: "share"
                            size: 12
                            color: bubble.inkSoft
                        }
                        Text {
                            text: root.channelName.length > 0 ? qsTr("Forwarded from ") + root.channelName : qsTr("Forwarded")
                            textFormat: Text.PlainText
                            color: bubble.inkSoft
                            font.pixelSize: Theme.fontSmall
                            font.italic: true
                            font.underline: root.channelName.length > 0 && channelLinkArea.containsMouse
                            Accessible.role: root.channelName.length > 0 ? Accessible.Link : Accessible.StaticText
                            MouseArea {
                                id: channelLinkArea
                                anchors.fill: parent
                                enabled: root.channelId.length > 0 && !root.selectMode
                                hoverEnabled: true
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: if (App.channels) App.channels.showChannel(root.channelId)
                            }
                        }
                    }

                    // A reply or reaction to a story: which one (its id and a label, never its key).
                    Row {
                        visible: root.storyReply.length > 0
                        spacing: 4
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: "stories"
                            size: 12
                            color: bubble.inkSoft
                        }
                        Text {
                            text: root.storyReply
                            textFormat: Text.PlainText
                            color: bubble.inkSoft
                            font.pixelSize: Theme.fontSmall
                            font.italic: true
                        }
                    }

                    // The message this replies to.
                    Rectangle {
                        visible: root.replyPreview.length > 0
                        Layout.fillWidth: true
                        implicitWidth: quote.implicitWidth + 18
                        implicitHeight: quote.implicitHeight + 10
                        radius: 6
                        color: root.fromMe ? Qt.alpha(Theme.bg, 0.35) : Theme.surface2
                        Rectangle {
                            width: 3
                            height: parent.height
                            radius: 2
                            color: bubble.accentInk
                        }
                        Text {
                            id: quote
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: (root.replyFromMe ? qsTr("You: ") : "") + root.replyPreview
                            textFormat: Text.PlainText
                            maximumLineCount: 2
                            wrapMode: Text.Wrap
                            elide: Text.ElideRight
                            color: bubble.inkSoft
                            font.pixelSize: Theme.fontSmall
                        }
                    }

                    // Text: selectable so it can be copied, never interpreted as markup.
                    // A poll: pick an option to vote (again to change it); the creator can end it.
                    ColumnLayout {
                        id: pollCard
                        visible: root.kind === "poll"
                        readonly property var p: root.poll || ({})
                        readonly property int total: p.voters || 0
                        Layout.preferredWidth: Math.min(360, root.maxBubbleWidth - 24)
                        spacing: 6
                        Row {
                            spacing: 6
                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                name: "chart-bar"
                                size: 14
                                color: bubble.inkSoft
                            }
                            Text {
                                text: pollCard.p.closed ? qsTr("Poll · ended") : pollCard.p.multiple ? qsTr("Poll · pick any") : qsTr("Poll · pick one")
                                textFormat: Text.PlainText
                                color: bubble.inkSoft
                                font.pixelSize: Theme.fontSmall
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: pollCard.p.question || ""
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: bubble.ink
                            font.pixelSize: Theme.fontSize + 1
                            font.weight: Font.DemiBold
                        }
                        Repeater {
                            model: pollCard.p.options || []
                            delegate: AbstractButton {
                                id: pollOption
                                required property var modelData
                                required property int index
                                Layout.fillWidth: true
                                implicitHeight: 40
                                hoverEnabled: true
                                enabled: !pollCard.p.closed
                                onClicked: {
                                    const opts = pollCard.p.options
                                    let picked = []
                                    for (let i = 0; i < opts.length; ++i) if (opts[i].mine) picked.push(i)
                                    if (pollOption.modelData.mine) picked = picked.filter(i => i !== pollOption.index)
                                    else picked = pollCard.p.multiple ? picked.concat([pollOption.index]) : [pollOption.index]
                                    root.voteRequested(root.messageId, picked)
                                }
                                background: Rectangle {
                                    radius: 8
                                    color: Qt.alpha(Theme.bg, pollOption.hovered && pollOption.enabled ? 0.55 : 0.35)
                                    border.color: pollOption.modelData.mine ? bubble.accentInk : "transparent"
                                    // How much of the vote this option has.
                                    Rectangle {
                                        height: parent.height
                                        width: pollCard.total > 0 ? parent.width * pollOption.modelData.count / pollCard.total : 0
                                        radius: 8
                                        color: Qt.alpha(bubble.accentInk, 0.18)
                                    }
                                }
                                contentItem: RowLayout {
                                    spacing: 8
                                    Rectangle {
                                        Layout.leftMargin: 10
                                        implicitWidth: 16
                                        implicitHeight: 16
                                        radius: pollCard.p.multiple ? 4 : 8
                                        color: pollOption.modelData.mine ? bubble.accentInk : "transparent"
                                        border.color: pollOption.modelData.mine ? bubble.accentInk : bubble.inkSoft
                                        border.width: 2
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: pollOption.modelData.label
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        color: bubble.ink
                                    }
                                    Text {
                                        Layout.rightMargin: 10
                                        text: pollOption.modelData.count
                                        textFormat: Text.PlainText
                                        color: bubble.inkSoft
                                        font.pixelSize: Theme.fontSmall
                                    }
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: pollCard.total === 1 ? qsTr("1 person voted") : pollCard.total + qsTr(" people voted")
                                textFormat: Text.PlainText
                                color: bubble.inkSoft
                                font.pixelSize: Theme.fontSmall
                            }
                            RvButton {
                                visible: root.fromMe && !pollCard.p.closed
                                primary: false
                                text: qsTr("End poll")
                                implicitHeight: 28
                                onClicked: root.endPollRequested(root.messageId)
                            }
                        }
                    }

                    TextEdit {
                        id: bodyText
                        visible: root.kind !== "file" && root.kind !== "poll"
                        // The current search match selects the words it matched (in the text as shown, markers gone).
                        readonly property int matchAt: root.highlighted && root.searchTerm.length > 0
                                                       ? getText(0, length).toLowerCase().indexOf(root.searchTerm.toLowerCase()) : -1
                        onMatchAtChanged: matchAt >= 0 ? select(matchAt, matchAt + root.searchTerm.length) : deselect()
                        Component.onCompleted: if (matchAt >= 0) select(matchAt, matchAt + root.searchTerm.length)
                        Layout.fillWidth: true
                        Layout.maximumWidth: root.maxBubbleWidth - 24
                        // Formatting is rendered from markup the core builds (everything the sender typed is escaped).
                        text: root.richText.length === 0 ? root.text : Theme.themedHtml(root.spoilersShown ? root.richTextRevealed : root.richText, bubble.onFill ? Theme.bubbleOutText : undefined)
                        textFormat: root.richText.length === 0 ? TextEdit.PlainText : TextEdit.RichText
                        readOnly: true
                        onLinkActivated: (link) => {
                            if (link === "spoiler:") root.spoilersShown = true
                            else root.linkRequested(link)
                        }
                        HoverHandler {
                            cursorShape: bodyText.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.IBeamCursor
                        }
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        color: bubble.ink
                        selectionColor: bubble.onFill ? Theme.bubbleOutText : Theme.accentFill
                        selectedTextColor: bubble.onFill ? Theme.bubbleOut : Theme.accentContrast
                        font.pixelSize: Theme.fontSize
                        font.italic: root.kind === "undecryptable"
                    }

                    // A picture, shown inline once it's decrypted (in memory only).
                    Item {
                        id: picture
                        visible: root.isImage
                        readonly property real maxW: Math.min(320, root.maxBubbleWidth - 24)
                        readonly property real maxH: 320
                        readonly property real natW: root.imageState === "ready" && root.imageInfo.width > 0 ? root.imageInfo.width : 240
                        readonly property real natH: root.imageState === "ready" && root.imageInfo.height > 0 ? root.imageInfo.height : 160
                        readonly property real scale: Math.min(1, maxW / natW, maxH / natH)
                        Layout.preferredWidth: Math.max(120, natW * scale)
                        Layout.preferredHeight: Math.max(80, natH * scale)
                        implicitWidth: Layout.preferredWidth
                        implicitHeight: Layout.preferredHeight

                        Component.onCompleted: if (root.isImage && root.imageState === "") root.imageLoadRequested(root.messageId, false)

                        Rectangle {
                            anchors.fill: parent
                            radius: 10
                            color: Qt.alpha(Theme.bg, 0.45)
                            visible: root.imageState !== "ready"
                            ColumnLayout {
                                anchors.centerIn: parent
                                width: parent.width - 24
                                spacing: 6
                                BusyIndicator {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.preferredWidth: 26
                                    Layout.preferredHeight: 26
                                    visible: root.imageState === "loading" || root.imageState === ""
                                    running: visible
                                }
                                Icon {
                                    Layout.alignment: Qt.AlignHCenter
                                    visible: root.imageState === "failed" || root.imageState === "large"
                                    name: "image"
                                    size: 22
                                    color: bubble.inkSoft
                                }
                                Text {
                                    Layout.fillWidth: true
                                    horizontalAlignment: Text.AlignHCenter
                                    text: root.imageState === "large" ? root.fileSize + qsTr(" · click to load")
                                        : root.imageState === "failed" ? qsTr("Couldn't show this picture. Click to try again.")
                                        : qsTr("Decrypting…")
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    color: bubble.inkSoft
                                    font.pixelSize: Theme.fontSmall
                                }
                            }
                        }
                        Image {
                            id: pictureImage
                            anchors.fill: parent
                            visible: root.imageState === "ready"
                            asynchronous: true
                            cache: false
                            fillMode: Image.PreserveAspectCrop
                            sourceSize.width: picture.width * 2
                            source: root.imageState === "ready" ? "image://chatimage/thumb/" + root.messageId : ""
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: 10
                            color: "transparent"
                            border.color: Theme.border
                            visible: root.imageState === "ready"
                        }
                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            onTapped: {
                                if (root.imageState === "ready") root.imageOpened(root.messageId)
                                else if (root.imageState === "large" || root.imageState === "failed") root.imageLoadRequested(root.messageId, true)
                            }
                        }
                    }

                    // A voice message (or other audio): played right here.
                    VoicePlayer {
                        visible: root.isAudio
                        info: root.audioInfo
                        ink: bubble.ink
                        inkSoft: bubble.inkSoft
                        onLoadRequested: root.audioLoadRequested(root.messageId)
                    }

                    // File: a card with its type, (sanitized) name and size; click for Open / Save as.
                    Rectangle {
                        id: fileCard
                        visible: root.kind === "file" && !root.isAudio
                        Layout.fillWidth: true
                        Layout.minimumWidth: Math.min(240, root.maxBubbleWidth - 24)
                        implicitWidth: fileRow.implicitWidth + 20
                        implicitHeight: fileRow.implicitHeight + 16
                        radius: 10
                        color: bubble.onFill ? Qt.alpha("#000000", fileHover.hovered ? 0.30 : 0.22)
                                             : (fileHover.hovered ? Theme.surface3 : Theme.surface)
                        border.width: 1
                        border.color: bubble.onFill ? Qt.alpha("#FFFFFF", 0.18) : Theme.border
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("File %1, %2. Open or save").arg(root.text).arg(root.fileSize)

                        TapHandler {
                            onTapped: root.fileRequested(root.messageId, root.text)
                        }
                        HoverHandler {
                            id: fileHover
                            cursorShape: Qt.PointingHandCursor
                        }
                    RowLayout {
                        id: fileRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 10

                        Rectangle {
                            implicitWidth: 36
                            implicitHeight: 36
                            radius: 8
                            color: bubble.onFill ? Qt.alpha("#FFFFFF", 0.14) : Theme.accentSoft
                            Icon {
                                anchors.centerIn: parent
                                name: "file"
                                size: 18
                                color: bubble.onFill ? Theme.bubbleOutText : Theme.accent
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                Layout.maximumWidth: root.maxBubbleWidth - 110
                                text: root.text
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: bubble.ink
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: root.fileSize + (root.fileStatus.length > 0 && root.fileStatus !== "available"
                                                       ? " · " + root.fileStatus : "")
                                textFormat: Text.PlainText
                                color: bubble.inkSoft
                                font.pixelSize: Theme.fontSmall
                            }
                        }
                        Icon {
                            name: "download"
                            size: 16
                            color: bubble.inkSoft
                        }
                    }
                    }

                    // Time, delivery and verification.
                    RowLayout {
                        Layout.alignment: Qt.AlignRight
                        spacing: 4
                        Icon {
                            visible: root.verification === "failed"
                            name: "triangle-alert"
                            size: 13
                            color: bubble.dangerInk
                        }
                        Text {
                            visible: root.verification === "failed"
                            text: qsTr("Server's copy differs")
                            color: bubble.dangerInk
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }
                        // Edited: click for the earlier versions.
                        Text {
                            visible: root.edited
                            text: "edited"
                            color: bubble.inkSoft
                            font.pixelSize: 11
                            font.italic: true
                            font.underline: editedHover.hovered
                            HoverHandler {
                                id: editedHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                onTapped: root.historyRequested(root.messageId)
                            }
                        }
                        Text {
                            text: root.timeText
                            textFormat: Text.PlainText
                            color: bubble.inkSoft
                            font.pixelSize: 11
                        }
                        // Queued (Tor outbox): a clock and the word, never a tick before the server has it.
                        // Sent: one tick. Delivered: two. Seen: two, and the word.
                        Icon {
                            visible: root.fromMe && root.status === "queued"
                            name: root.queueHeld ? "alert-circle" : "clock"
                            size: 14
                            color: bubble.inkSoft
                        }
                        Text {
                            visible: root.fromMe && root.status === "queued"
                            text: root.queueHeld ? qsTr("Not sent") : qsTr("Queued")
                            textFormat: Text.PlainText
                            color: bubble.inkSoft
                            font.pixelSize: 11
                            HoverHandler { id: queueHover }
                            ToolTip.visible: queueHover.hovered && root.queueError.length > 0
                            ToolTip.text: root.queueError
                        }
                        Text {
                            visible: root.fromMe && root.status === "queued"
                            text: root.queueHeld ? qsTr("Retry") : ""
                            textFormat: Text.PlainText
                            color: bubble.inkSoft
                            font.pixelSize: 11
                            font.underline: true
                            TapHandler { onTapped: root.retryQueuedRequested(root.messageId) }
                        }
                        Text {
                            visible: root.fromMe && root.status === "queued"
                            text: qsTr("Discard")
                            color: bubble.inkSoft
                            font.pixelSize: 11
                            font.underline: true
                            TapHandler { onTapped: root.discardQueuedRequested(root.messageId) }
                        }
                        Icon {
                            visible: root.fromMe && root.status !== "queued"
                            name: root.status === "sent" ? "check" : "check-check"
                            size: 14
                            color: bubble.inkSoft
                        }
                        Text {
                            visible: root.fromMe && root.status === "seen"
                            text: qsTr("Seen")
                            color: bubble.inkSoft
                            font.pixelSize: 11
                        }
                    }
                }
            }

            // Hover actions.
            Row {
                spacing: 2
                visible: !root.selectMode
                opacity: hover.hovered || reactMenu.visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 90 } }

                RvButton {
                    visible: root.kind !== "undecryptable"
                    primary: false
                    iconName: "reply"
                    iconSize: 16
                    implicitHeight: 30
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Reply")
                    onClicked: root.replyRequested(root.messageId)
                }
                RvButton {
                    visible: root.kind !== "undecryptable"
                    primary: false
                    iconName: "smile-plus"
                    iconSize: 16
                    implicitHeight: 30
                    ToolTip.visible: hovered && !reactMenu.visible
                    ToolTip.text: qsTr("React")
                    onClicked: reactMenu.open()

                    Popup {
                        id: reactMenu
                        y: -height - 4
                        padding: 4
                        background: Rectangle {
                            radius: 18
                            color: Theme.surface
                            border.color: Theme.border
                        }
                        contentItem: Row {
                            spacing: 2
                            Repeater {
                                model: root.reactionChoices
                                delegate: RvButton {
                                    required property string modelData
                                    primary: false
                                    text: modelData
                                    implicitHeight: 32
                                    leftPadding: 6
                                    rightPadding: 6
                                    font.pixelSize: 18
                                    onClicked: {
                                        reactMenu.close()
                                        root.reactRequested(root.messageId, modelData)
                                    }
                                }
                            }
                            // Any other emoji, from the full picker.
                            RvButton {
                                primary: false
                                text: "+"
                                implicitHeight: 32
                                leftPadding: 10
                                rightPadding: 10
                                font.pixelSize: 18
                                Accessible.name: "More reactions"
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("More reactions")
                                onClicked: {
                                    reactMenu.close()
                                    root.reactPickerRequested(root.messageId)
                                }
                            }
                        }
                    }
                }
                RvButton {
                    primary: false
                    iconName: "dots"
                    iconSize: 16
                    implicitHeight: 30
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("More")
                    Accessible.name: "More actions"
                    onClicked: root.menuRequested(root.messageId, root.fromMe, root.kind, root.text)
                }
            }

            Item { Layout.fillWidth: true }
        }

        // Reactions under the bubble; click one to add or remove yours.
        Flow {
            visible: root.reactions.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            layoutDirection: root.fromMe ? Qt.RightToLeft : Qt.LeftToRight
            spacing: 4

            Repeater {
                model: root.reactions
                delegate: Rectangle {
                    id: reactionChip
                    required property var modelData
                    implicitWidth: chip.implicitWidth + 14
                    implicitHeight: 24
                    radius: 12
                    color: modelData.mine ? Theme.accentSoft : Theme.surface
                    border.color: modelData.mine ? Theme.accent : Theme.border
                    Text {
                        id: chip
                        anchors.centerIn: parent
                        text: reactionChip.modelData.emoji
                              + (reactionChip.modelData.count > 1 ? " " + reactionChip.modelData.count : "")
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: 13
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.reactRequested(root.messageId, reactionChip.modelData.emoji)
                    }
                }
            }
        }
    }
}
