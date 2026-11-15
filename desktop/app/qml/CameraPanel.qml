// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtMultimedia
import Redvault

// Taking a photo or a video for a story with this computer's camera (Qt Multimedia), where it has one. The picture
// goes to the app's private studio folder; a photo is then redrawn like any picked photo (its metadata stays behind),
// a video is shared as it is. Up to 60 seconds of video.
Rectangle {
    id: panel
    signal done(url file, string kind)
    signal cancelled()
    property bool active: false
    property int seconds: 0
    property string error: ""

    function close() {
        if (recorder.recorderState === MediaRecorder.RecordingState) recorder.stop()
        panel.active = false
        panel.cancelled()
    }

    color: Theme.storyStage
    Accessible.role: Accessible.Dialog
    Accessible.name: "Camera"
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }

    MediaDevices { id: devices }
    CaptureSession {
        id: session
        camera: Camera {
            id: camera
            active: panel.active && devices.videoInputs.length > 0
            cameraDevice: devices.videoInputs.length > camera.index ? devices.videoInputs[camera.index] : devices.defaultVideoInput
            property int index: 0
            onErrorOccurred: (error, errorString) => panel.error = errorString.length > 0 ? errorString : qsTr("The camera couldn't be started.")
        }
        audioInput: AudioInput {}
        imageCapture: ImageCapture {
            id: shot
            onImageSaved: (requestId, path) => {
                panel.active = false
                panel.done(StudioTools.fileUrl(path), "image")
            }
            onErrorOccurred: (requestId, error, message) => panel.error = message.length > 0 ? message : qsTr("The photo couldn't be taken.")
        }
        recorder: MediaRecorder {
            id: recorder
            onRecorderStateChanged: {
                if (recorder.recorderState === MediaRecorder.StoppedState && recorder.actualLocation.toString().length > 0 && panel.seconds > 0) {
                    panel.active = false
                    panel.done(recorder.actualLocation, "video")
                }
            }
            onErrorOccurred: (error, message) => panel.error = message.length > 0 ? message : qsTr("The video couldn't be recorded.")
        }
        videoOutput: viewfinder
    }
    Timer {
        interval: 1000
        repeat: true
        running: recorder.recorderState === MediaRecorder.RecordingState
        onTriggered: {
            panel.seconds++
            if (panel.seconds >= 60) recorder.stop()
        }
    }

    VideoOutput {
        id: viewfinder
        anchors.fill: parent
        anchors.bottomMargin: 90
        fillMode: VideoOutput.PreserveAspectFit
    }
    Text {
        anchors.centerIn: viewfinder
        width: parent.width - 48
        visible: devices.videoInputs.length === 0 || panel.error.length > 0
        text: panel.error.length > 0 ? panel.error : qsTr("No camera was found on this computer. Pick a photo or video instead.")
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        color: Theme.storyChrome
        font.pixelSize: Theme.fontSize
    }

    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        IconButton { iconName: "close"; iconColor: Theme.storyChrome; tip: qsTr("Close the camera"); onClicked: panel.close() }
        Item { Layout.fillWidth: true }
        Text {
            visible: recorder.recorderState === MediaRecorder.RecordingState
            text: "0:" + String(panel.seconds).padStart(2, "0") + " / 1:00"
            color: Theme.storyChrome
            font.pixelSize: Theme.fontSize
            font.weight: Font.DemiBold
        }
        RvButton {
            secondary: true
            visible: devices.videoInputs.length > 1
            text: qsTr("Switch camera")
            onClicked: camera.index = (camera.index + 1) % devices.videoInputs.length
        }
    }

    RowLayout {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        spacing: 16
        enabled: camera.active && panel.error.length === 0
        RvButton {
            text: qsTr("Take photo")
            enabled: shot.readyForCapture && recorder.recorderState !== MediaRecorder.RecordingState
            onClicked: shot.captureToFile(StudioTools.scratchPath("jpg"))
        }
        RvButton {
            danger: recorder.recorderState === MediaRecorder.RecordingState
            secondary: recorder.recorderState !== MediaRecorder.RecordingState
            text: recorder.recorderState === MediaRecorder.RecordingState ? qsTr("Stop") : qsTr("Record video")
            onClicked: {
                if (recorder.recorderState === MediaRecorder.RecordingState) {
                    recorder.stop()
                    return
                }
                panel.seconds = 0
                recorder.outputLocation = StudioTools.scratchFile("mp4")
                recorder.record()
            }
        }
    }
}
