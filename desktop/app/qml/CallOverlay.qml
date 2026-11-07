pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtMultimedia
import Redvault

// Everything about a call, over the rest of the window: the incoming
// pop-up (bottom right, doesn't block the app), the full-window voice or
// video call with its safety words, device pickers and in-call chat, and
// the closing banner. Minimized calls show CallBar instead.
Item {
    id: root
    required property CallController calls
    required property ChatSession chats
    signal openChat(string chatId)

    readonly property string phase: calls.phase
    readonly property bool video: calls.kind === "video"
    readonly property bool fullScreen: phase.length > 0 && phase !== "incoming" && !calls.minimized
    readonly property string title: calls.isLink && calls.linkName.length > 0 ? calls.linkName : calls.peerHandle
    property double now: Date.now()

    function duration() {
        if (!calls.connectedAt)
            return ""
        const s = Math.max(0, Math.floor((now - calls.connectedAt) / 1000))
        const hh = Math.floor(s / 3600), mm = Math.floor((s % 3600) / 60), ss = s % 60
        const two = (n) => (n < 10 ? "0" : "") + n
        return (hh > 0 ? hh + ":" + two(mm) : two(mm)) + ":" + two(ss)
    }
    function status() {
        if (phase === "ended") return calls.endedMessage
        if (phase === "outgoing") return calls.isLink ? "Asking to join…" : "Calling…"
        if (phase === "connecting") return "Connecting securely…"
        if (phase === "reconnecting") return "Reconnecting…"
        if (phase === "active") return duration()
        return ""
    }

    Timer {
        running: root.calls.connectedAt > 0 && root.phase !== "ended"
        interval: 1000
        repeat: true
        onTriggered: root.now = Date.now()
    }

    // --- Incoming pop-up ---
    Rectangle {
        id: incoming
        visible: root.phase === "incoming"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 24
        width: 380
        height: incomingColumn.implicitHeight + 40
        radius: Theme.radiusDialog
        color: Theme.surface
        border.color: Theme.borderStrong
        Accessible.role: Accessible.AlertMessage
        Accessible.name: "Incoming call from " + root.title

        ColumnLayout {
            id: incomingColumn
            anchors.fill: parent
            anchors.margins: 20
            spacing: 14

            RowLayout {
                spacing: 14
                Avatar {
                    size: 56
                    initial: root.calls.peerHandle.length > 0 ? root.calls.peerHandle.charAt(0).toUpperCase() : "?"
                    source: root.calls.peerAvatarUrl
                    tintKey: root.calls.peerUserId
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: root.video ? "Incoming video call" : "Incoming voice call"
                        textFormat: Text.PlainText
                        color: Theme.text3
                        font.pixelSize: 12
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.calls.peerHandle
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text
                        font.family: Theme.displayFamily
                        font.pixelSize: 22
                        font.weight: Font.Bold
                    }
                    Text {
                        visible: root.calls.isLink
                        Layout.fillWidth: true
                        text: "Through your link “" + root.calls.linkName + "”"
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text2
                        font.pixelSize: 12
                    }
                }
            }
            RowLayout {
                spacing: 6
                Icon {
                    name: "shield-check"
                    size: 14
                    color: Theme.accent
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.calls.isLink ? "Sealed with the link's secret · end-to-end encrypted"
                                            : "Signed by their identity key · end-to-end encrypted"
                    textFormat: Text.PlainText
                    color: Theme.accent
                    font.pixelSize: 12
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                RvButton {
                    Layout.fillWidth: true
                    danger: true
                    primary: true
                    iconName: "phone-off"
                    text: "Decline"
                    onClicked: root.calls.decline()
                }
                RvButton {
                    Layout.fillWidth: true
                    visible: root.video
                    secondary: true
                    iconName: "phone"
                    text: "Audio"
                    onClicked: root.calls.accept(true)
                }
                RvButton {
                    Layout.fillWidth: true
                    iconName: root.video ? "video" : "phone"
                    text: "Accept"
                    onClicked: root.calls.accept(false)
                }
            }
            RvButton {
                Layout.alignment: Qt.AlignHCenter
                visible: root.calls.conversationId.length > 0
                primary: false
                implicitHeight: 32
                text: "Message instead"
                onClicked: {
                    const chat = root.calls.conversationId
                    root.calls.decline()
                    root.openChat(chat)
                }
            }
        }
    }

    // --- Full-window call ---
    Rectangle {
        id: screen
        anchors.fill: parent
        visible: root.fullScreen
        color: root.video ? Theme.callBg : Theme.bg

        // Catch clicks so nothing underneath reacts.
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
        }

        RowLayout {
            anchors.fill: parent
            spacing: 0

            Item {
                id: stage
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Remote video (or their initial while the camera's off).
                VideoOutput {
                    id: remoteVideo
                    anchors.fill: parent
                    visible: root.video
                    fillMode: VideoOutput.PreserveAspectFit
                }

                // Voice call (and video before the picture arrives): who, and the state.
                ColumnLayout {
                    anchors.centerIn: parent
                    visible: !root.video || root.phase !== "active"
                    spacing: 14
                    Avatar {
                        Layout.alignment: Qt.AlignHCenter
                        size: 140
                        initial: root.calls.peerHandle.length > 0 ? root.calls.peerHandle.charAt(0).toUpperCase() : "?"
                        source: root.calls.peerAvatarUrl
                        tintKey: root.calls.peerUserId
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: root.title
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.family: Theme.displayFamily
                        font.pixelSize: 40
                        font.weight: Font.Bold
                        font.letterSpacing: -0.8
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: root.status()
                        textFormat: Text.PlainText
                        color: root.phase === "ended" ? Theme.danger : root.phase === "active" ? Theme.text2 : Theme.text3
                        font.family: root.phase === "active" ? Theme.monoFamily : Theme.fontFamily
                        font.pixelSize: 16
                    }
                }

                // Top bar: name, timer, encryption.
                RowLayout {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 24
                    spacing: 10
                    Rectangle {
                        implicitWidth: lockRow.implicitWidth + 24
                        implicitHeight: 34
                        radius: 17
                        color: Qt.alpha(Theme.accentSoft, 0.9)
                        border.color: Theme.accentBorder
                        RowLayout {
                            id: lockRow
                            anchors.centerIn: parent
                            spacing: 6
                            Icon {
                                name: "lock"
                                size: 14
                                color: Theme.accent
                            }
                            Text {
                                text: "End-to-end encrypted" + (root.calls.isLink ? " · call link" : "")
                                textFormat: Text.PlainText
                                color: Theme.accent
                                font.pixelSize: 12
                                font.weight: Font.Medium
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        visible: root.video && root.phase === "active"
                        text: root.title + " · " + root.duration()
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.family: Theme.monoFamily
                        font.pixelSize: 14
                    }
                }

                // Your camera, small, top right.
                Rectangle {
                    visible: root.video
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.rightMargin: 24
                    anchors.topMargin: 76
                    width: 220
                    height: 140
                    radius: 14
                    color: Theme.surface3
                    clip: true
                    border.color: Theme.borderStrong
                    VideoOutput {
                        id: localVideo
                        anchors.fill: parent
                        anchors.margins: 1
                        visible: !root.calls.cameraOff || root.calls.sharingScreen
                        fillMode: VideoOutput.PreserveAspectCrop
                    }
                    Column {
                        anchors.centerIn: parent
                        visible: root.calls.cameraOff && !root.calls.sharingScreen
                        spacing: 6
                        Icon {
                            anchors.horizontalCenter: parent.horizontalCenter
                            name: "video-off"
                            size: 22
                            color: Theme.text3
                        }
                        Text {
                            text: "Camera off"
                            color: Theme.text3
                            font.pixelSize: 12
                        }
                    }
                }

                // Screen sharing banners.
                Rectangle {
                    visible: root.calls.sharingScreen || root.calls.remoteSharing
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: 76
                    implicitWidth: shareRow.implicitWidth + 28
                    implicitHeight: 40
                    radius: 20
                    color: Theme.surface
                    border.color: Theme.borderStrong
                    RowLayout {
                        id: shareRow
                        anchors.centerIn: parent
                        spacing: 10
                        Icon {
                            name: "monitor-up"
                            size: 16
                            color: Theme.accent
                        }
                        Text {
                            text: root.calls.sharingScreen ? "You're sharing your screen" : root.calls.peerHandle + " is sharing their screen"
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: 13
                        }
                        RvButton {
                            visible: root.calls.sharingScreen
                            primary: false
                            danger: true
                            implicitHeight: 28
                            text: "Stop"
                            onClicked: root.calls.toggleScreenShare()
                        }
                    }
                }

                // Bottom: safety words, notices, devices, controls.
                ColumnLayout {
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottomMargin: 28
                    width: Math.min(parent.width - 48, 720)
                    spacing: 16

                    // Four words from both encryption fingerprints: the same on both screens.
                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter
                        visible: root.calls.sas.length > 0 && root.phase !== "ended"
                        implicitWidth: sasColumn.implicitWidth + 36
                        implicitHeight: sasColumn.implicitHeight + 24
                        radius: Theme.radiusCard
                        color: Qt.alpha(Theme.surface, 0.95)
                        border.color: Theme.accentBorder
                        ColumnLayout {
                            id: sasColumn
                            anchors.centerIn: parent
                            spacing: 6
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: "Say these words. " + root.calls.peerHandle + " should see the same ones."
                                textFormat: Text.PlainText
                                color: Theme.text3
                                font.pixelSize: 12
                            }
                            Row {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 8
                                Repeater {
                                    model: root.calls.sas
                                    delegate: Rectangle {
                                        required property string modelData
                                        width: word.implicitWidth + 20
                                        height: 30
                                        radius: 8
                                        color: Theme.accentSoft
                                        Text {
                                            id: word
                                            anchors.centerIn: parent
                                            text: parent.modelData
                                            textFormat: Text.PlainText
                                            color: Theme.accent
                                            font.family: Theme.monoFamily
                                            font.pixelSize: 15
                                            font.weight: Font.Medium
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        visible: root.phase !== "ended" && root.calls.endedMessage.length > 0
                        text: root.calls.endedMessage
                        textFormat: Text.PlainText
                        color: Theme.warn
                        font.pixelSize: 13
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        visible: root.phase !== "ended"
                        spacing: 10
                        DevicePicker {
                            iconName: "mic"
                            defaultLabel: "Default microphone"
                            devices: root.calls.microphones
                            currentId: root.calls.microphoneId
                            onPicked: (id) => root.calls.setMicrophone(id)
                        }
                        DevicePicker {
                            iconName: "speaker"
                            defaultLabel: "Default speaker"
                            devices: root.calls.speakers
                            currentId: root.calls.speakerId
                            onPicked: (id) => root.calls.setSpeaker(id)
                        }
                        DevicePicker {
                            visible: root.video
                            iconName: "video"
                            defaultLabel: "Default camera"
                            devices: root.calls.cameras
                            currentId: root.calls.cameraId
                            onPicked: (id) => root.calls.setCamera(id)
                        }
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 14
                        visible: root.phase !== "ended"
                        IconButton {
                            round: true; filled: true
                            implicitWidth: 56; implicitHeight: 56; iconSize: 22
                            iconName: root.calls.muted ? "mic-off" : "mic"
                            toggled: root.calls.muted
                            tip: root.calls.muted ? "Unmute" : "Mute"
                            onClicked: root.calls.toggleMute()
                        }
                        IconButton {
                            visible: root.video
                            round: true; filled: true
                            implicitWidth: 56; implicitHeight: 56; iconSize: 22
                            iconName: root.calls.cameraOff ? "video-off" : "video"
                            toggled: root.calls.cameraOff
                            tip: root.calls.cameraOff ? "Turn camera on" : "Turn camera off"
                            onClicked: root.calls.toggleCamera()
                        }
                        IconButton {
                            visible: root.video
                            round: true; filled: true
                            implicitWidth: 56; implicitHeight: 56; iconSize: 22
                            iconName: "monitor-up"
                            toggled: root.calls.sharingScreen
                            enabled: root.phase === "active"
                            tip: root.calls.sharingScreen ? "Stop sharing" : "Share your screen"
                            onClicked: {
                                const problem = root.calls.toggleScreenShare()
                                if (problem.length > 0) shareNotice.text = problem
                            }
                        }
                        IconButton {
                            visible: root.video && root.calls.conversationId.length > 0
                            round: true; filled: true
                            implicitWidth: 56; implicitHeight: 56; iconSize: 22
                            iconName: "chat"
                            toggled: root.calls.chatOpen
                            tip: root.calls.chatOpen ? "Hide chat" : "Show chat"
                            onClicked: {
                                if (!root.calls.chatOpen)
                                    root.chats.openChat(root.calls.conversationId)
                                root.calls.toggleChat()
                            }
                        }
                        IconButton {
                            round: true; filled: true
                            implicitWidth: 56; implicitHeight: 56; iconSize: 22
                            iconName: "chevron-down"
                            tip: "Back to the app (the call continues)"
                            onClicked: {
                                if (root.calls.conversationId.length > 0)
                                    root.openChat(root.calls.conversationId)
                                root.calls.setMinimized(true)
                            }
                        }
                        IconButton {
                            danger: true
                            round: true
                            implicitWidth: 64; implicitHeight: 56; iconSize: 24
                            iconName: "phone-off"
                            tip: "End call"
                            onClicked: root.calls.hangUp()
                        }
                    }
                    Text {
                        id: shareNotice
                        Layout.alignment: Qt.AlignHCenter
                        visible: text.length > 0
                        color: Theme.warn
                        font.pixelSize: 13
                        Timer {
                            running: shareNotice.text.length > 0
                            interval: 5000
                            onTriggered: shareNotice.text = ""
                        }
                    }

                    RvButton {
                        Layout.alignment: Qt.AlignHCenter
                        visible: root.phase === "ended"
                        primary: false
                        text: "Close"
                        onClicked: root.calls.dismiss()
                    }
                }
            }

            // In-call chat (video calls with a contact).
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 340
                visible: root.video && root.calls.chatOpen && root.calls.conversationId.length > 0
                         && root.chats.currentChatId === root.calls.conversationId
                color: Theme.panel
                Rectangle {
                    width: 1
                    height: parent.height
                    color: Theme.border
                }
                MiniChat {
                    anchors.fill: parent
                    anchors.leftMargin: 1
                    session: root.chats
                    onCloseRequested: root.calls.toggleChat()
                }
            }
        }
    }

    Component.onCompleted: calls.setVideoSinks(localVideo.videoSink, remoteVideo.videoSink)
}
