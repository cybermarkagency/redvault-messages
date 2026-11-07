// Stand-in for DeviceKeysSession.
import QtQml

QtObject {
    property bool chatActive: false
    property string pinnedChat: ""

    function chatPinned(chatId) {
        return pinnedChat.length > 0 && chatId === pinnedChat
    }
}
