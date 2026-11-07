pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Shapes
import QtMultimedia
import Redvault

// What a channel post carries inside its bubble, as the web app's PostContent: "Suggested by a follower", photos (at full resolution
// when opened), videos, round videos, podcasts, voice clips and files, the
// formatted text, a poll or quiz, or a checklist. Media is fetched through
// the app's own client, without the session cookie.
ColumnLayout {
    id: content
    required property var post
    required property ChannelsSession session
    property bool isOwner: false
    /// A scheduled post or a suggestion: shown, but not voted on or ticked.
    property bool preview: false
    /// The bubble's colour, for the corners around a round video.
    property color cardColor: Theme.selected
    property bool spoilersShown: false
    signal linkRequested(string url)
    signal photoRequested(var media)
    signal saveRequested(var media)
    spacing: 8

    readonly property var photos: post && post.photos ? post.photos : []
    readonly property var others: post && post.others ? post.others : []
    readonly property var poll: post && post.poll ? post.poll : ({})
    readonly property var checklist: post && post.checklist ? post.checklist : ({})
    readonly property bool hasPoll: poll.question !== undefined
    readonly property bool hasChecklist: checklist.title !== undefined
    /// Media, a poll or a checklist: these take the bubble's full width, where text alone sizes it.
    readonly property bool wide: photos.length > 0 || others.length > 0 || hasPoll || hasChecklist
    /// The owner ticks a checklist's items; so does anyone following when it's collaborative.
    readonly property bool canTick: !preview && (isOwner || checklist.collaborative === true)

    // Posts appear as the channel's: a suggested one says so, never who sent it.
    Rectangle {
        visible: content.post && content.post.suggested === true
        implicitWidth: suggestedLabel.implicitWidth + 16
        implicitHeight: suggestedLabel.implicitHeight + 6
        radius: height / 2
        color: Theme.accentSoft
        Text {
            id: suggestedLabel
            anchors.centerIn: parent
            text: "Suggested by a follower"
            color: Theme.accent
            font.pixelSize: Theme.fontSmall
            font.weight: Font.DemiBold
        }
    }

    // --- Photos: one wide, several in two columns; a click opens the original ---
    GridLayout {
        Layout.fillWidth: true
        visible: content.photos.length > 0
        columns: content.photos.length === 1 ? 1 : 2
        columnSpacing: 6
        rowSpacing: 6
        Repeater {
            model: content.photos
            delegate: Rectangle {
                id: photo
                required property var modelData
                Layout.fillWidth: true
                // The picture's own shape when the post says it (up to a tall-ish limit), else a 4:3 box.
                Layout.preferredHeight: {
                    const w = modelData.width > 0 ? modelData.width : 4
                    const h = modelData.height > 0 ? modelData.height : 3
                    return Math.min(content.photos.length === 1 ? 460 : 240, width * h / w)
                }
                radius: Theme.radiusControl
                color: Theme.surface2
                clip: true
                Image {
                    id: picture
                    anchors.fill: parent
                    asynchronous: true
                    cache: false
                    fillMode: content.photos.length === 1 ? Image.PreserveAspectFit : Image.PreserveAspectCrop
                    sourceSize: Qt.size(Math.max(1, parent.width * 2), Math.max(1, parent.height * 2))
                    source: "image://channelmedia/" + encodeURIComponent("fit|" + photo.modelData.url)
                    Accessible.name: photo.modelData.hasName ? photo.modelData.name : "Photo"
                }
                BusyIndicator {
                    anchors.centerIn: parent
                    running: picture.status === Image.Loading
                    visible: running
                }
                Text {
                    anchors.centerIn: parent
                    visible: picture.status === Image.Error
                    text: "Couldn't load the photo"
                    color: Theme.text3
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: content.photoRequested(photo.modelData)
                }
                ToolTip.visible: photoHover.hovered
                ToolTip.delay: 600
                ToolTip.text: "Open the full-resolution photo"
                HoverHandler { id: photoHover }
            }
        }
    }

    // --- Videos, round videos, podcasts, voice clips and files ---
    Repeater {
        model: content.others
        delegate: Item {
            id: other
            required property var modelData
            Layout.fillWidth: true
            implicitHeight: loader.height
            Loader {
                id: loader
                width: other.width
                sourceComponent: other.modelData.kind === "video" ? videoView
                               : other.modelData.kind === "round" ? roundView
                               : other.modelData.kind === "audio" || other.modelData.kind === "voice" ? audioView
                               : fileView
            }
            Component {
                id: videoView
                VideoPlayer { media: other.modelData }
            }
            Component {
                id: roundView
                RoundVideo { media: other.modelData }
            }
            Component {
                id: audioView
                AudioPlayer { media: other.modelData; podcast: other.modelData.kind === "audio" }
            }
            Component {
                id: fileView
                FileRow { media: other.modelData }
            }
        }
    }

    // --- The text, formatted (*bold*, _italic_, ~strike~, `code`, ||spoiler||, links) ---
    TextEdit {
        id: bodyText
        Layout.fillWidth: true
        visible: content.post && content.post.body.length > 0
        // Rendered from markup the core builds (everything the owner typed is escaped).
        text: !content.post ? "" : content.post.bodyRich.length === 0 ? content.post.body
              : content.spoilersShown ? content.post.bodyRevealed : content.post.bodyRich
        textFormat: content.post && content.post.bodyRich.length > 0 ? TextEdit.RichText : TextEdit.PlainText
        readOnly: true
        selectByMouse: true
        wrapMode: TextEdit.Wrap
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentContrast
        font.pixelSize: Theme.fontSize
        onLinkActivated: (link) => {
            if (link === "spoiler:") content.spoilersShown = true
            else content.linkRequested(link)
        }
        HoverHandler {
            cursorShape: bodyText.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.IBeamCursor
        }
    }

    // --- A poll or quiz: results once you've voted (or it's closed); a quiz shows whether you were right ---
    Rectangle {
        id: pollCard
        Layout.fillWidth: true
        visible: content.hasPoll
        implicitHeight: pollColumn.implicitHeight
        color: "transparent"
        property var picked: []
        readonly property bool canVote: !content.preview && content.hasPoll && !content.poll.closed && !content.poll.showResults
        function toggle(index) {
            const next = pollCard.picked.slice()
            const at = next.indexOf(index)
            if (at >= 0) next.splice(at, 1)
            else next.push(index)
            pollCard.picked = next
        }

        ColumnLayout {
            id: pollColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            spacing: 8
            Text {
                text: content.hasPoll ? content.poll.kindLabel : ""
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: content.hasPoll ? content.poll.question : ""
                textFormat: Text.PlainText
                color: Theme.text
                font.weight: Font.DemiBold
            }
            Repeater {
                model: content.hasPoll ? content.poll.options : []
                delegate: Item {
                    id: option
                    required property var modelData
                    required property int index
                    readonly property bool on: pollCard.picked.indexOf(index) >= 0
                    Layout.fillWidth: true
                    implicitHeight: 38

                    // Before voting: tap one (or tick several, then Vote).
                    Rectangle {
                        anchors.fill: parent
                        visible: !content.poll.showResults
                        radius: Theme.radiusControl
                        color: optionArea.containsMouse && pollCard.canVote ? Theme.surface3 : Theme.surface
                        border.color: option.on ? Theme.accent : Theme.border
                        Row {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8
                            Rectangle {
                                visible: content.poll.multiple === true
                                anchors.verticalCenter: parent.verticalCenter
                                width: 16
                                height: 16
                                radius: 4
                                color: option.on ? Theme.accent : "transparent"
                                border.color: option.on ? Theme.accent : Theme.borderStrong
                                Icon {
                                    anchors.centerIn: parent
                                    visible: option.on
                                    name: "check"
                                    size: 12
                                    color: Theme.accentContrast
                                }
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: option.modelData.label
                                textFormat: Text.PlainText
                                color: Theme.text
                            }
                        }
                        MouseArea {
                            id: optionArea
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: pollCard.canVote
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (content.poll.multiple) pollCard.toggle(option.index)
                                else content.session.vote(content.post.id, [option.index])
                            }
                        }
                    }

                    // After: the share of each option; a quiz marks the right answer (and yours if wrong).
                    Rectangle {
                        anchors.fill: parent
                        visible: content.poll.showResults === true
                        radius: Theme.radiusControl
                        color: Theme.surface
                        border.color: option.modelData.right ? Theme.accent : option.modelData.wrong ? Theme.danger : Theme.border
                        clip: true
                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: parent.width * option.modelData.percent / 100
                            radius: Theme.radiusControl
                            color: option.modelData.right ? Theme.accentSoft : option.modelData.wrong ? Theme.dangerSoft : Theme.selected
                        }
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.right: percentText.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            elide: Text.ElideRight
                            text: option.modelData.result
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.weight: option.modelData.chosen ? Font.DemiBold : Font.Normal
                        }
                        Text {
                            id: percentText
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: option.modelData.percent + "%"
                            textFormat: Text.PlainText
                            color: Theme.text2
                        }
                    }
                }
            }
            RvButton {
                visible: pollCard.canVote && content.poll.multiple === true
                text: "Vote"
                enabled: pollCard.picked.length > 0
                onClicked: {
                    content.session.vote(content.post.id, pollCard.picked)
                    pollCard.picked = []
                }
            }
            Text {
                Layout.fillWidth: true
                visible: content.hasPoll && content.poll.explanation.length > 0
                wrapMode: Text.Wrap
                text: content.hasPoll ? content.poll.explanation : ""
                textFormat: Text.PlainText
                color: Theme.text2
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: content.hasPoll ? content.poll.votesLabel : ""
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall
                }
                RvButton {
                    visible: !content.preview && content.hasPoll && content.poll.canTakeBack === true
                    primary: false
                    implicitHeight: 28
                    text: "Take back my vote"
                    onClicked: content.session.vote(content.post.id, [])
                }
                RvButton {
                    visible: !content.preview && content.isOwner && content.hasPoll && content.poll.closed !== true
                    primary: false
                    implicitHeight: 28
                    text: content.hasPoll ? content.poll.closeLabel : ""
                    enabled: !content.session.busy
                    onClicked: content.session.closePoll(content.post.id)
                }
            }
        }
    }

    // --- A checklist: the owner ticks items, and anyone following when it's collaborative ---
    Rectangle {
        Layout.fillWidth: true
        visible: content.hasChecklist
        implicitHeight: checkColumn.implicitHeight
        color: "transparent"

        ColumnLayout {
            id: checkColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            spacing: 6
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: content.hasChecklist ? content.checklist.title : ""
                textFormat: Text.PlainText
                color: Theme.text
                font.weight: Font.DemiBold
            }
            Text {
                text: content.hasChecklist ? content.checklist.doneLabel : ""
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
            Repeater {
                model: content.hasChecklist ? content.checklist.items : []
                delegate: Item {
                    id: item
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    implicitHeight: Math.max(28, itemText.implicitHeight + 8)
                    Rectangle {
                        id: box
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18
                        height: 18
                        radius: 4
                        color: item.modelData.done ? Theme.accent : "transparent"
                        border.color: item.modelData.done ? Theme.accent : Theme.borderStrong
                        opacity: content.canTick ? 1 : 0.6
                        Icon {
                            anchors.centerIn: parent
                            visible: item.modelData.done
                            name: "check"
                            size: 13
                            color: Theme.accentContrast
                        }
                    }
                    Text {
                        id: itemText
                        anchors.left: box.right
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        wrapMode: Text.Wrap
                        text: item.modelData.text
                        textFormat: Text.PlainText
                        color: item.modelData.done ? Theme.text3 : Theme.text
                        font.strikeout: item.modelData.done
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: content.canTick
                        cursorShape: Qt.PointingHandCursor
                        onClicked: content.session.tick(content.post.id, item.index)
                    }
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: item.modelData.text
                    Accessible.checked: item.modelData.done
                }
            }
        }
    }

    // --- The players ---

    // A video: nothing is fetched until it's played.
    component VideoPlayer: Rectangle {
        id: video
        property var media: ({})
        implicitHeight: Math.min(460, width * (media.width > 0 && media.height > 0 ? media.height / media.width : 9 / 16))
        radius: Theme.radiusControl
        color: "#000000"
        clip: true
        ChannelMediaPlayer {
            id: videoPlayer
            session: content.session
            url: video.media.url
            size: video.media.size
            mediaId: video.media.id
            knownDuration: video.media.durationMs
            videoOutput: videoOut
        }
        VideoOutput {
            id: videoOut
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
        }
        MouseArea {
            anchors.fill: parent
            onClicked: videoPlayer.toggle()
        }
        // Play, and what it is, before it starts.
        Rectangle {
            anchors.centerIn: parent
            visible: !videoPlayer.playing
            width: 56
            height: 56
            radius: 28
            color: "#B3000000"
            Icon {
                anchors.centerIn: parent
                name: "play"
                size: 26
                color: "#FFFFFF"
            }
        }
        BusyIndicator {
            anchors.centerIn: parent
            running: videoPlayer.loading
            visible: running
        }
        RowLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 8
            spacing: 8
            visible: videoPlayer.started || video.media.durationLabel.length > 0
            Slider {
                Layout.fillWidth: true
                visible: videoPlayer.started
                from: 0
                to: Math.max(1, videoPlayer.duration)
                value: videoPlayer.position
                onMoved: videoPlayer.seek(value)
            }
            Text {
                text: videoPlayer.started ? content.session.formatDuration(videoPlayer.position / 1000) + " / "
                                            + content.session.formatDuration(videoPlayer.duration / 1000)
                                          : video.media.durationLabel
                textFormat: Text.PlainText
                color: "#FFFFFF"
                font.pixelSize: Theme.fontSmall
            }
        }
        Text {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: 44
            visible: videoPlayer.error.length > 0
            text: videoPlayer.error
            textFormat: Text.PlainText
            color: "#FFFFFF"
        }
    }

    // A round video message: in a circle; a click plays it with sound, another pauses.
    component RoundVideo: Item {
        id: round
        property var media: ({})
        implicitHeight: 220
        ChannelMediaPlayer {
            id: roundPlayer
            session: content.session
            url: round.media.url
            size: round.media.size
            mediaId: round.media.id
            knownDuration: round.media.durationMs
            videoOutput: roundOut
        }
        Item {
            id: circle
            width: 220
            height: 220
            Rectangle {
                anchors.fill: parent
                color: "#000000"
            }
            VideoOutput {
                id: roundOut
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectCrop
            }
            // The corners painted over in the card's colour: a circle that works on any renderer.
            Shape {
                anchors.fill: parent
                ShapePath {
                    fillColor: content.cardColor
                    strokeColor: "transparent"
                    fillRule: ShapePath.OddEvenFill
                    PathSvg { path: "M0,0 H220 V220 H0 Z M110,0 A110,110 0 1,0 110,220 A110,110 0 1,0 110,0 Z" }
                }
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 18
                visible: !roundPlayer.playing
                width: roundBadge.implicitWidth + 16
                height: 26
                radius: 13
                color: "#B3000000"
                Row {
                    id: roundBadge
                    anchors.centerIn: parent
                    spacing: 4
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: "play"
                        size: 14
                        color: "#FFFFFF"
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: round.media.durationLabel
                        textFormat: Text.PlainText
                        color: "#FFFFFF"
                        font.pixelSize: Theme.fontSmall
                    }
                }
            }
            BusyIndicator {
                anchors.centerIn: parent
                running: roundPlayer.loading
                visible: running
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: roundPlayer.toggle()
            }
            Accessible.role: Accessible.Button
            Accessible.name: roundPlayer.playing ? "Pause video message" : "Play video message"
        }
        Text {
            anchors.left: circle.right
            anchors.leftMargin: 12
            anchors.verticalCenter: circle.verticalCenter
            visible: roundPlayer.error.length > 0
            text: roundPlayer.error
            textFormat: Text.PlainText
            color: Theme.danger
        }
    }

    // A podcast (resumes where you stopped, ±15 s, 1×/1.5×/2×) or a voice clip (the same, compact).
    component AudioPlayer: Rectangle {
        id: audio
        property var media: ({})
        property bool podcast: false
        implicitHeight: audioRow.implicitHeight + 16
        radius: Theme.radiusControl
        color: Theme.surface2
        border.color: Theme.border
        ChannelMediaPlayer {
            id: audioPlayer
            session: content.session
            url: audio.media.url
            size: audio.media.size
            mediaId: audio.media.id
            resume: audio.podcast
            knownDuration: audio.media.durationMs
        }
        RowLayout {
            id: audioRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 6
            RvButton {
                iconName: audioPlayer.playing ? "pause" : "play"
                implicitWidth: 40
                Accessible.name: audioPlayer.playing ? "Pause" : "Play"
                onClicked: audioPlayer.toggle()
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    visible: audio.podcast && audio.media.hasName === true
                    elide: Text.ElideRight
                    text: audio.media.name
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.weight: Font.DemiBold
                }
                Slider {
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, audioPlayer.duration)
                    value: audioPlayer.position
                    enabled: audioPlayer.started
                    onMoved: audioPlayer.seek(value)
                    Accessible.name: "Position"
                }
                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: content.session.formatDuration(audioPlayer.position / 1000) + " / "
                          + content.session.formatDuration(audioPlayer.duration / 1000)
                          + (audio.podcast && audioPlayer.position === 0 && audioPlayer.resumeAt > 5
                             ? " · resumes at " + content.session.formatDuration(audioPlayer.resumeAt) : "")
                          + (audioPlayer.error.length > 0 ? " · " + audioPlayer.error : "")
                    textFormat: Text.PlainText
                    color: audioPlayer.error.length > 0 ? Theme.danger : Theme.text3
                    font.pixelSize: Theme.fontSmall
                }
            }
            RvButton {
                visible: audio.podcast
                primary: false
                implicitHeight: 32
                leftPadding: 8
                rightPadding: 8
                text: "−15"
                Accessible.name: "Back 15 seconds"
                onClicked: audioPlayer.skip(-15)
            }
            RvButton {
                visible: audio.podcast
                primary: false
                implicitHeight: 32
                leftPadding: 8
                rightPadding: 8
                text: "+15"
                Accessible.name: "Forward 15 seconds"
                onClicked: audioPlayer.skip(15)
            }
            RvButton {
                primary: false
                implicitHeight: 32
                leftPadding: 8
                rightPadding: 8
                text: audioPlayer.speed + "×"
                Accessible.name: "Playback speed " + audioPlayer.speed + "×"
                onClicked: audioPlayer.cycleSpeed()
            }
        }
    }

    // A file: its name and size; saving streams it to disk.
    component FileRow: Rectangle {
        id: file
        property var media: ({})
        implicitHeight: 56
        radius: Theme.radiusControl
        color: fileArea.containsMouse ? Theme.surface3 : Theme.surface2
        border.color: Theme.border
        Icon {
            id: fileIcon
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            name: "file"
            size: 22
            color: Theme.text2
        }
        Column {
            anchors.left: fileIcon.right
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            Text {
                width: parent.width
                elide: Text.ElideMiddle
                text: file.media.name
                textFormat: Text.PlainText
                color: Theme.text
                font.weight: Font.DemiBold
            }
            Text {
                text: file.media.sizeLabel + " · Download"
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
        }
        MouseArea {
            id: fileArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: content.saveRequested(file.media)
        }
        Accessible.role: Accessible.Button
        Accessible.name: "Download " + file.media.name
    }
}
