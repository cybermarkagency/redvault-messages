// Stand-in for ChannelMediaPlayer: never plays.
import QtQml

QtObject {
    property var session: null
    property string url: ""
    property real size: 0
    property string mediaId: ""
    property bool resume: false
    property var videoOutput: null
    property bool playing: false
    property bool loading: false
    property bool started: false
    property string error: ""
    property real position: 0
    property real duration: 0
    property real knownDuration: 0
    property real speed: 1
    property real resumeAt: 0
    function toggle() {}
    function seek(ms) {}
    function skip(seconds) {}
    function cycleSpeed() {}
}
