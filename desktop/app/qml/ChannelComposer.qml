pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import QtMultimedia
import Redvault

// The owner's composer, pinned under the thread and built like a chat's
// (ThreadView): attach, a text box that grows, and a round send button. As the
// web app's Composer it posts formatted text (the marker around the selection,
// Ctrl+B and friends or the formatting bar) and one of photos, videos, files,
// audio and recordings (voice clips and round videos), a poll, a quiz or a
// checklist, now or scheduled. The attach menu holds those; what's chosen opens
// in a panel over the text box. Enter posts; Shift+Enter starts a new line.
// Nothing here says "encrypted": channels aren't.
Rectangle {
    id: composer
    required property ChannelsSession channels
    /// How tall the panel over the text box may get.
    property int maxPanelHeight: 320
    /// "", "poll", "quiz" or "checklist".
    property string extra: ""
    /// "", "voice" or "round": recording one now.
    property string recording: ""
    property bool scheduling: false
    property bool formatting: false
    property var options: ["", ""]
    property bool multiple: false
    property int correct: 0
    property var items: [""]
    property bool collaborative: false
    readonly property ChannelUploads uploads: channels ? channels.postUploads : null
    readonly property bool hasMedia: uploads !== null && uploads.count > 0
    readonly property bool canAttach: extra.length === 0 && uploads !== null && !uploads.full
    readonly property bool tooLong: body.text.length > channels.maxPostLength
    readonly property bool canPost: channels !== null && !channels.posting && uploads !== null && !uploads.busy && !tooLong
                                    && (body.text.trim().length > 0 || extra.length > 0 || uploads.readyCount > 0)
                                    && !(hasMedia && uploads.readyCount === 0 && body.text.trim().length === 0)
                                    && (!scheduling || schedule.iso.length > 0 || !schedule.filled)
    readonly property bool panelOpen: recording.length > 0 || hasMedia || extra.length > 0 || scheduling

    implicitHeight: column.implicitHeight + 20
    color: Theme.bg

    function reset() {
        body.text = ""
        extra = ""
        question.text = ""
        options = ["", ""]
        multiple = false
        correct = 0
        explanation.text = ""
        listTitle.text = ""
        items = [""]
        collaborative = false
        scheduling = false
        schedule.clear()
        if (uploads) uploads.clear()
    }
    function chooseExtra(next) {
        extra = extra === next ? "" : next
    }
    function toggleSchedule() {
        scheduling = !scheduling
        if (scheduling) schedule.reset()
    }
    function startRecording(kind) {
        recording = kind
        Qt.callLater(recorder.start)
    }
    /// The toolbar and Ctrl+B and friends: the marker around the selection (or an empty pair at the cursor).
    function format(marker) {
        const start = body.selectionStart
        const end = body.selectionEnd
        const t = body.text
        body.text = t.slice(0, start) + marker + t.slice(start, end) + marker + t.slice(end)
        body.forceActiveFocus()
        body.select(start + marker.length, end + marker.length)
    }
    function setAt(list, index, value) {
        const next = list.slice()
        next[index] = value
        return next
    }
    function submit() {
        if (!canPost) return
        const post = { type: extra }
        if (extra === "poll" || extra === "quiz") {
            post.question = question.text
            post.options = options
            post.multiple = extra === "poll" && multiple
            post.correct = correct
            post.explanation = explanation.text
        } else if (extra === "checklist") {
            post.title = listTitle.text
            post.items = items
            post.collaborative = collaborative
        }
        channels.submitPost(body.text, post, scheduling ? schedule.iso : "")
    }

    Connections {
        target: composer.channels
        function onPosted() { composer.reset() }
    }

    FileDialog {
        id: mediaDialog
        title: "Add photos, videos or audio"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Photos, videos and audio (*.jpg *.jpeg *.png *.gif *.webp *.avif *.heic *.heif *.bmp *.mp4 *.m4v *.mov *.webm *.mkv *.avi *.mp3 *.m4a *.aac *.ogg *.oga *.opus *.wav *.flac)", "All files (*)"]
        currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        onAccepted: composer.uploads.addFiles(selectedFiles)
    }
    FileDialog {
        id: fileDialog
        title: "Add files"
        fileMode: FileDialog.OpenFiles
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: composer.uploads.addFiles(selectedFiles, "file")
    }
    ChannelRecorder {
        id: recorder
        kind: composer.recording === "round" ? "round" : "voice"
        videoOutput: recordPreview
        onFinished: (file, durationMs, mime) => {
            composer.uploads.addRecording(file, recorder.kind, durationMs, mime)
            composer.recording = ""
        }
    }

    // Only for its hex() helper, to color the menu's icons.
    Icon {
        id: menuIcon
        visible: false
    }

    // --- What to add ---
    Menu {
        id: attachMenu
        MenuItem {
            text: "Photo, video or audio…"
            icon.source: "image://icon/image/" + menuIcon.hex(Theme.text)
            enabled: composer.canAttach
            onTriggered: mediaDialog.open()
        }
        MenuItem {
            text: "File…"
            icon.source: "image://icon/file/" + menuIcon.hex(Theme.text)
            enabled: composer.canAttach
            onTriggered: fileDialog.open()
        }
        MenuItem {
            text: "Voice clip"
            icon.source: "image://icon/mic/" + menuIcon.hex(Theme.text)
            enabled: composer.canAttach && composer.recording.length === 0
            onTriggered: composer.startRecording("voice")
        }
        MenuItem {
            text: "Round video"
            icon.source: "image://icon/video/" + menuIcon.hex(Theme.text)
            enabled: composer.canAttach && composer.recording.length === 0
            onTriggered: composer.startRecording("round")
        }
        MenuSeparator {}
        MenuItem {
            text: "Poll"
            icon.source: "image://icon/chart-bar/" + menuIcon.hex(Theme.text)
            enabled: !composer.hasMedia
            onTriggered: composer.chooseExtra("poll")
        }
        MenuItem {
            text: "Quiz"
            icon.source: "image://icon/info/" + menuIcon.hex(Theme.text)
            enabled: !composer.hasMedia
            onTriggered: composer.chooseExtra("quiz")
        }
        MenuItem {
            text: "Checklist"
            icon.source: "image://icon/check/" + menuIcon.hex(Theme.text)
            enabled: !composer.hasMedia
            onTriggered: composer.chooseExtra("checklist")
        }
        MenuSeparator {}
        MenuItem {
            text: composer.scheduling ? "Post now instead" : "Schedule…"
            icon.source: "image://icon/clock/" + menuIcon.hex(Theme.text)
            onTriggered: composer.toggleSchedule()
        }
    }

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.border
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 10
        spacing: 8

        // --- What's been added, over the text box ---
        Flickable {
            id: panel
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(panelColumn.implicitHeight, composer.maxPanelHeight)
            visible: composer.panelOpen
            clip: true
            contentWidth: width
            contentHeight: panelColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: panelColumn
                width: panel.width
                spacing: 8

                // --- Recording a voice clip or a round video ---
                Rectangle {
                    Layout.fillWidth: true
                    visible: composer.recording.length > 0
                    implicitHeight: recordColumn.implicitHeight + 20
                    radius: Theme.radiusControl
                    color: Theme.surface2
                    border.color: Theme.border
                    ColumnLayout {
                        id: recordColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 10
                        spacing: 8
                        Item {
                            Layout.alignment: Qt.AlignHCenter
                            visible: composer.recording === "round" && recorder.error.length === 0
                            implicitWidth: 180
                            implicitHeight: 180
                            VideoOutput {
                                id: recordPreview
                                anchors.fill: parent
                                fillMode: VideoOutput.PreserveAspectCrop
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Rectangle {
                                visible: recorder.recording
                                implicitWidth: 10
                                implicitHeight: 10
                                radius: 5
                                color: Theme.danger
                            }
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: recorder.error.length > 0 ? recorder.error
                                    : composer.recording === "round"
                                      ? composer.channels.formatDuration(recorder.seconds) + " / " + composer.channels.formatDuration(recorder.maxSeconds)
                                      : "Recording " + composer.channels.formatDuration(recorder.seconds)
                                textFormat: Text.PlainText
                                color: recorder.error.length > 0 ? Theme.danger : Theme.text
                            }
                            RvButton {
                                visible: recorder.error.length === 0
                                text: "Stop and attach"
                                enabled: recorder.recording
                                onClicked: recorder.stop()
                            }
                            RvButton {
                                secondary: true
                                text: "Cancel"
                                onClicked: {
                                    recorder.cancel()
                                    composer.recording = ""
                                }
                            }
                        }
                    }
                }

                ChannelUploadList {
                    Layout.fillWidth: true
                    uploads: composer.uploads
                }

                // --- A poll, quiz or checklist: what it is, and taking it off again ---
                RowLayout {
                    Layout.fillWidth: true
                    visible: composer.extra.length > 0
                    spacing: 8
                    Text {
                        Layout.fillWidth: true
                        text: composer.extra === "poll" ? "New poll" : composer.extra === "quiz" ? "New quiz" : "New checklist"
                        textFormat: Text.PlainText
                        color: Theme.text2
                        font.pixelSize: Theme.fontSmall + 1
                        font.weight: Font.DemiBold
                    }
                    RvButton {
                        primary: false
                        iconName: "x"
                        implicitHeight: 28
                        Accessible.name: "Remove"
                        ToolTip.visible: hovered
                        ToolTip.text: "Remove"
                        onClicked: composer.extra = ""
                    }
                }

                // --- A poll or quiz ---
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: composer.extra === "poll" || composer.extra === "quiz"
                    spacing: 6
                    RvTextField {
                        id: question
                        Layout.fillWidth: true
                        placeholderText: composer.extra === "quiz" ? "Quiz question" : "Poll question"
                        maximumLength: 300
                        Accessible.name: "Question"
                    }
                    Repeater {
                        model: composer.options.length
                        delegate: RowLayout {
                            id: optionRow
                            required property int index
                            Layout.fillWidth: true
                            spacing: 6
                            // A quiz: which option is right.
                            RadioButton {
                                visible: composer.extra === "quiz"
                                checked: composer.correct === optionRow.index
                                onClicked: composer.correct = optionRow.index
                                Accessible.name: "Option " + (optionRow.index + 1) + " is the right answer"
                            }
                            RvTextField {
                                Layout.fillWidth: true
                                placeholderText: "Option " + (optionRow.index + 1)
                                maximumLength: 100
                                text: composer.options[optionRow.index]
                                Accessible.name: "Option " + (optionRow.index + 1)
                                onTextEdited: composer.options = composer.setAt(composer.options, optionRow.index, text)
                            }
                            RvButton {
                                visible: composer.options.length > 2
                                primary: false
                                implicitHeight: 32
                                text: "Remove"
                                onClicked: {
                                    const next = composer.options.slice()
                                    next.splice(optionRow.index, 1)
                                    composer.options = next
                                    if (composer.correct >= optionRow.index && composer.correct > 0) composer.correct = composer.correct - 1
                                }
                            }
                        }
                    }
                    RvButton {
                        visible: composer.options.length < composer.channels.maxPollOptions
                        primary: false
                        implicitHeight: 32
                        text: "Add an option"
                        onClicked: composer.options = composer.options.concat([""])
                    }
                    CheckBox {
                        visible: composer.extra === "poll"
                        text: "Let people pick more than one"
                        checked: composer.multiple
                        onToggled: composer.multiple = checked
                    }
                    RvTextField {
                        id: explanation
                        Layout.fillWidth: true
                        visible: composer.extra === "quiz"
                        placeholderText: "Why it's right (optional, shown after answering)"
                        maximumLength: 200
                        Accessible.name: "Explanation"
                    }
                }

                // --- A checklist ---
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: composer.extra === "checklist"
                    spacing: 6
                    RvTextField {
                        id: listTitle
                        Layout.fillWidth: true
                        placeholderText: "Checklist title"
                        maximumLength: 200
                        Accessible.name: "Checklist title"
                    }
                    Repeater {
                        model: composer.items.length
                        delegate: RowLayout {
                            id: itemRow
                            required property int index
                            Layout.fillWidth: true
                            spacing: 6
                            RvTextField {
                                Layout.fillWidth: true
                                placeholderText: "Item " + (itemRow.index + 1)
                                maximumLength: 200
                                text: composer.items[itemRow.index]
                                Accessible.name: "Item " + (itemRow.index + 1)
                                onTextEdited: composer.items = composer.setAt(composer.items, itemRow.index, text)
                            }
                            RvButton {
                                visible: composer.items.length > 1
                                primary: false
                                implicitHeight: 32
                                text: "Remove"
                                onClicked: {
                                    const next = composer.items.slice()
                                    next.splice(itemRow.index, 1)
                                    composer.items = next
                                }
                            }
                        }
                    }
                    RvButton {
                        visible: composer.items.length < composer.channels.maxChecklistItems
                        primary: false
                        implicitHeight: 32
                        text: "Add an item"
                        onClicked: composer.items = composer.items.concat([""])
                    }
                    CheckBox {
                        text: "Followers can tick items too"
                        checked: composer.collaborative
                        onToggled: composer.collaborative = checked
                    }
                }

                ChannelScheduleField {
                    id: schedule
                    Layout.fillWidth: true
                    visible: composer.scheduling
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: composer.channels.composerError.length > 0
            wrapMode: Text.Wrap
            text: composer.channels.composerError
            textFormat: Text.PlainText
            color: Theme.danger
        }

        // --- Formatting ---
        Row {
            visible: composer.formatting
            spacing: 4
            Repeater {
                model: [["B", "Bold (*text*, Ctrl+B)", "*"], ["I", "Italic (_text_, Ctrl+I)", "_"], ["S", "Strikethrough (~text~, Ctrl+Shift+X)", "~"],
                        ["</>", "Code (`text`, Ctrl+E)", "`"], ["||", "Spoiler (||text||, Ctrl+Shift+B)", "||"]]
                delegate: RvButton {
                    required property var modelData
                    secondary: true
                    // The text box keeps the focus, and with it the selection to wrap.
                    focusPolicy: Qt.NoFocus
                    implicitHeight: 30
                    leftPadding: 10
                    rightPadding: 10
                    text: modelData[0]
                    font.bold: modelData[0] === "B"
                    font.italic: modelData[0] === "I"
                    font.strikeout: modelData[0] === "S"
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: modelData[1]
                    Accessible.name: modelData[1]
                    onClicked: composer.format(modelData[2])
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                leftPadding: 8
                text: "Links are made automatically"
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
        }

        // --- The composer itself, as a chat's ---
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            IconButton {
                id: attachButton
                objectName: "channelAttachButton"
                Layout.alignment: Qt.AlignBottom
                implicitHeight: 42
                iconName: "paperclip"
                tip: "Attach, poll, quiz, checklist or schedule"
                onClicked: attachMenu.popup(attachButton, 0, -attachMenu.implicitHeight - 6)
            }
            IconButton {
                Layout.alignment: Qt.AlignBottom
                implicitHeight: 42
                iconName: "pen"
                tip: composer.formatting ? "Hide formatting" : "Formatting"
                toggled: composer.formatting
                onClicked: composer.formatting = !composer.formatting
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(140, Math.max(40, body.implicitHeight))

                TextArea {
                    id: body
                    objectName: "channelPostInput"
                    placeholderText: "Write a post for everyone following this channel"
                    Accessible.name: "New post"
                    persistentSelection: true
                    wrapMode: TextArea.Wrap
                    textFormat: TextEdit.PlainText
                    color: Theme.text
                    placeholderTextColor: Theme.placeholder
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.accentContrast
                    font.pixelSize: Theme.fontSize + 1
                    leftPadding: 14
                    rightPadding: 12
                    topPadding: 10
                    bottomPadding: 10
                    background: Rectangle {
                        radius: Theme.radiusCard
                        color: Theme.surface2
                        border.width: 1
                        border.color: composer.tooLong ? Theme.danger : body.activeFocus ? Theme.accentBorder : Theme.surface2
                    }

                    ToolTip.visible: hovered && body.length === 0
                    ToolTip.delay: 1200
                    ToolTip.text: "*bold*  _italic_  ~strike~  `code`  ||spoiler||   (Ctrl+B, Ctrl+I, Ctrl+Shift+X, Ctrl+E, Ctrl+Shift+B)"

                    // Enter posts; Shift+Enter starts a new line.
                    Keys.onPressed: (event) => {
                        if (event.modifiers & Qt.ControlModifier) {
                            const shift = event.modifiers & Qt.ShiftModifier
                            const marker = event.key === Qt.Key_B ? (shift ? "||" : "*")
                                         : event.key === Qt.Key_I && !shift ? "_"
                                         : event.key === Qt.Key_X && shift ? "~"
                                         : event.key === Qt.Key_E && !shift ? "`" : ""
                            if (marker.length > 0) {
                                event.accepted = true
                                composer.format(marker)
                                return
                            }
                        }
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier)) {
                            event.accepted = true
                            composer.submit()
                        }
                    }
                }
            }

            IconButton {
                objectName: "channelSendButton"
                Layout.alignment: Qt.AlignBottom
                iconName: composer.scheduling && schedule.iso.length > 0 ? "clock" : "send"
                tip: composer.channels.posting ? "Posting…" : composer.scheduling && schedule.iso.length > 0 ? "Schedule post" : "Post"
                round: true
                implicitWidth: 42
                implicitHeight: 42
                toggled: false
                iconColor: Theme.accentContrast
                enabled: composer.canPost
                onClicked: composer.submit()
                background: Rectangle {
                    radius: 21
                    color: Theme.accent
                    opacity: parent.enabled ? 1 : 0.4
                }
            }
        }

        // How long it is and who it goes out as, once there's something to post.
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 4
            visible: body.text.length > 0 || (composer.uploads !== null && composer.uploads.busy)
            elide: Text.ElideRight
            text: body.text.length + "/" + composer.channels.maxPostLength + " · posted as "
                  + (composer.channels.channel.name || "") + (composer.uploads && composer.uploads.busy ? " · uploading…" : "")
            textFormat: Text.PlainText
            color: composer.tooLong ? Theme.danger : Theme.text3
            font.pixelSize: Theme.fontSmall
        }
    }
}
