// Stand-in for ChannelRecorder: never records.
import QtQml

QtObject {
    property string kind: "voice"
    property var videoOutput: null
    property bool recording: false
    property int seconds: 0
    property int maxSeconds: 60
    property string error: ""
    signal finished(url file, int durationMs, string mime)
    function start() {}
    function stop() {}
    function cancel() {}
}
