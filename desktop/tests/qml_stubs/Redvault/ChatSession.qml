// Stand-in for ChatSession: records what would have gone out.
import QtQuick

QtObject {
    property bool sending: false
    property string currentChatId: "chat-1"
    property ListModel messages: ListModel {}
    property string lastSent: ""
    signal sent(bool ok)

    function sendMessage(text, replyTo) {
        lastSent = text
        sent(true)
    }
}
