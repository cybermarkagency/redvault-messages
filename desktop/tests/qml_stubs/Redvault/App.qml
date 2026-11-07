// Stand-in for the app singleton: only what MiniChat.qml reads.
pragma Singleton
import QtQml

QtObject {
    property DeviceKeys deviceKeys: DeviceKeys {}
    property QtObject live: QtObject {
        function notifyTyping(chatId) {}
    }
}
