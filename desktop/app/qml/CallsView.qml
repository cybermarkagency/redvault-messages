pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Calls: history (kept only on this computer, encrypted) and call links,
// with the selected contact's calls or link on the right.
Item {
    id: root
    required property CallController calls
    required property ChatSession chats
    required property AccountSession account
    signal openChat(string chatId)

    property string filter: "all"   // "all", "missed", "links"
    property string selectedPeer: ""
    property string selectedLink: ""
    property string notice: ""

    function when(ms) {
        const d = new Date(ms)
        const today = new Date()
        if (d.toDateString() === today.toDateString())
            return Qt.formatTime(d, "hh:mm")
        const yesterday = new Date(today.getTime() - 86400000)
        if (d.toDateString() === yesterday.toDateString())
            return "Yesterday"
        return Qt.formatDate(d, "d MMM")
    }
    function length(sec) {
        if (sec < 60) return sec + " s"
        const m = Math.round(sec / 60)
        return m < 60 ? m + " min" : Math.floor(m / 60) + " h " + (m % 60) + " min"
    }
    function outcomeText(r) {
        if (r.outcome === "answered") return (r.direction === "incoming" ? "Incoming" : "Outgoing") + " · " + length(r.durationSec)
        if (r.outcome === "missed") return "Missed"
        if (r.outcome === "declined") return r.direction === "incoming" ? "Declined" : "Declined by them"
        if (r.outcome === "unanswered") return "No answer"
        return "Didn't connect"
    }
    function tryCall(chatId, kind) {
        root.notice = chatId.length > 0 ? root.calls.startCall(chatId, kind) : "You can only call contacts you have a chat with."
    }

    readonly property var shownHistory: {
        const out = []
        for (const r of root.calls.history) {
            if (root.filter === "missed" && r.outcome !== "missed") continue
            const q = search.text.trim().toLowerCase()
            if (q.length > 0 && (r.peerHandle + " " + (r.linkName || "")).toLowerCase().indexOf(q) < 0) continue
            out.push(r)
        }
        return out
    }
    readonly property var peerHistory: root.calls.history.filter(r => r.peerUserId === root.selectedPeer)
    readonly property var selectedLinkData: root.calls.links.find(l => l.id === root.selectedLink) || null

    ConfirmDialog {
        id: clearHistoryDialog
        title: "Clear call history?"
        message: "Every call in the list is deleted from this computer. The server never kept a record of them, so they're gone for good. Call links stay."
        confirmText: "Clear history"
        danger: true
        onConfirmed: {
            root.calls.clearHistory()
            root.selectedPeer = ""
        }
    }

    Connections {
        target: root.calls
        function onLinkCreated(url) {
            root.calls.copyText(url)
            root.notice = "Link created and copied. Anyone with it can ask to join; you'll ring when they do."
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // --- List ---
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.listWidth
            color: Theme.panel
            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: Theme.border
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.rightMargin: 1
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.headerHeight
                    Layout.leftMargin: 20
                    Layout.rightMargin: 12
                    Text {
                        Layout.fillWidth: true
                        text: "Calls"
                        color: Theme.text
                        font.family: Theme.displayFamily
                        font.pixelSize: Theme.fontDisplay
                        font.weight: Font.Bold
                    }
                    IconButton {
                        iconName: "trash"
                        tip: "Clear call history"
                        enabled: root.calls !== null && root.calls.history.length > 0
                        onClicked: clearHistoryDialog.open()
                    }
                    IconButton {
                        iconName: "link"
                        tip: "New call link"
                        onClicked: {
                            root.filter = "links"
                            root.selectedPeer = ""
                            root.selectedLink = ""
                        }
                    }
                }

                SearchField {
                    id: search
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    placeholderText: "Search calls"
                }

                Row {
                    Layout.leftMargin: 16
                    Layout.topMargin: 12
                    Layout.bottomMargin: 8
                    spacing: 6
                    Repeater {
                        model: [["all", "All"], ["missed", "Missed"], ["links", "Call links"]]
                        delegate: Chip {
                            required property var modelData
                            text: modelData[1]
                            checked: root.filter === modelData[0]
                            onClicked: {
                                root.filter = modelData[0]
                                if (root.filter === "links") root.selectedPeer = ""
                                else root.selectedLink = ""
                            }
                        }
                    }
                }

                // History
                ListView {
                    id: historyList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.filter !== "links"
                    clip: true
                    model: root.shownHistory
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    delegate: ItemDelegate {
                        id: callRow
                        required property var modelData
                        readonly property bool missed: modelData.outcome === "missed"
                        width: ListView.view.width
                        height: 68
                        leftInset: 8
                        rightInset: 8
                        leftPadding: 18
                        rightPadding: 18
                        onClicked: {
                            root.selectedLink = ""
                            root.selectedPeer = modelData.peerUserId
                        }
                        background: Rectangle {
                            radius: Theme.radiusControl
                            color: root.selectedPeer === callRow.modelData.peerUserId ? Theme.selected
                                 : callRow.hovered ? Qt.alpha(Theme.selected, 0.5) : "transparent"
                        }
                        contentItem: RowLayout {
                            spacing: 12
                            Avatar {
                                size: 42
                                initial: callRow.modelData.peerHandle.length > 0 ? callRow.modelData.peerHandle.charAt(0).toUpperCase() : "?"
                                tintKey: callRow.modelData.peerUserId
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: callRow.modelData.peerHandle + (callRow.modelData.linkName ? " · " + callRow.modelData.linkName : "")
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: callRow.missed ? Theme.danger : Theme.text
                                    font.pixelSize: 15
                                    font.weight: Font.Medium
                                }
                                RowLayout {
                                    spacing: 5
                                    Icon {
                                        name: callRow.modelData.direction === "incoming" ? "arrow-down-left" : "arrow-up-right"
                                        size: 14
                                        color: callRow.missed ? Theme.danger : Theme.text3
                                    }
                                    Icon {
                                        name: callRow.modelData.kind === "video" ? "video" : "phone"
                                        size: 13
                                        color: Theme.text3
                                    }
                                    Text {
                                        text: root.outcomeText(callRow.modelData)
                                        textFormat: Text.PlainText
                                        color: callRow.missed ? Theme.danger : Theme.text3
                                        font.pixelSize: 13
                                    }
                                }
                            }
                            Text {
                                text: root.when(callRow.modelData.startedAt)
                                textFormat: Text.PlainText
                                color: Theme.text3
                                font.pixelSize: 12
                            }
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 48
                        visible: historyList.count === 0
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: root.filter === "missed" ? "No missed calls." : "No calls yet. Your call history is kept only on this computer, encrypted."
                        textFormat: Text.PlainText
                        color: Theme.text3
                    }
                }

                // Links
                ListView {
                    id: linkList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.filter === "links"
                    clip: true
                    model: root.calls.links
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    delegate: ItemDelegate {
                        id: linkRow
                        required property var modelData
                        width: ListView.view.width
                        height: 64
                        leftInset: 8
                        rightInset: 8
                        leftPadding: 18
                        rightPadding: 18
                        onClicked: {
                            root.selectedPeer = ""
                            root.selectedLink = modelData.id
                        }
                        background: Rectangle {
                            radius: Theme.radiusControl
                            color: root.selectedLink === linkRow.modelData.id ? Theme.selected
                                 : linkRow.hovered ? Qt.alpha(Theme.selected, 0.5) : "transparent"
                        }
                        contentItem: RowLayout {
                            spacing: 12
                            Rectangle {
                                implicitWidth: 42
                                implicitHeight: 42
                                radius: 13
                                color: Theme.accentSoft
                                border.color: Theme.accentBorder
                                Icon {
                                    anchors.centerIn: parent
                                    name: "link"
                                    size: 18
                                    color: Theme.accent
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: linkRow.modelData.name
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: Theme.text
                                    font.pixelSize: 15
                                    font.weight: Font.Medium
                                }
                                Text {
                                    text: (linkRow.modelData.kind === "video" ? "Video" : "Voice") + " link"
                                    textFormat: Text.PlainText
                                    color: Theme.text3
                                    font.pixelSize: 13
                                }
                            }
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 48
                        visible: linkList.count === 0
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: "No call links yet. Create one on the right to share with someone."
                        color: Theme.text3
                    }
                }
            }
        }

        // --- Detail ---
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.bg

            Flickable {
                id: detail
                anchors.fill: parent
                clip: true
                contentWidth: width
                contentHeight: detailColumn.implicitHeight + 64
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                ColumnLayout {
                    id: detailColumn
                    x: Math.max(32, (detail.width - 640) / 2)
                    y: 32
                    width: Math.min(640, detail.width - 64)
                    spacing: 16

                    Banner {
                        Layout.fillWidth: true
                        error: root.notice.indexOf("copied") < 0
                        text: root.notice
                    }
                    Banner {
                        Layout.fillWidth: true
                        text: root.calls.linkError
                    }

                    // A contact's calls.
                    ColumnLayout {
                        id: peerPane
                        Layout.fillWidth: true
                        visible: root.selectedPeer.length > 0 && root.peerHistory.length > 0
                        spacing: 16
                        readonly property var first: root.peerHistory.length > 0 ? root.peerHistory[0] : null
                        readonly property string chatId: first && first.conversationId ? first.conversationId : root.chats.chatIdForUser(root.selectedPeer)

                        Avatar {
                            Layout.alignment: Qt.AlignHCenter
                            size: 88
                            initial: peerPane.first ? peerPane.first.peerHandle.charAt(0).toUpperCase() : "?"
                            tintKey: root.selectedPeer
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: peerPane.first ? peerPane.first.peerHandle : ""
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.family: Theme.displayFamily
                            font.pixelSize: 28
                            font.weight: Font.Bold
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 10
                            RvButton {
                                secondary: true
                                iconName: "phone"
                                text: "Voice"
                                enabled: peerPane.chatId.length > 0
                                onClicked: root.tryCall(peerPane.chatId, "voice")
                            }
                            RvButton {
                                secondary: true
                                iconName: "video"
                                text: "Video"
                                enabled: peerPane.chatId.length > 0
                                onClicked: root.tryCall(peerPane.chatId, "video")
                            }
                            RvButton {
                                secondary: true
                                iconName: "chat"
                                text: "Message"
                                enabled: peerPane.chatId.length > 0
                                onClicked: root.openChat(peerPane.chatId)
                            }
                        }
                        Card {
                            Layout.fillWidth: true
                            spacing: 0
                            Text {
                                Layout.bottomMargin: 8
                                text: "HISTORY WITH " + (peerPane.first ? peerPane.first.peerHandle.toUpperCase() : "")
                                textFormat: Text.PlainText
                                color: Theme.text3
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.8
                            }
                            Repeater {
                                model: root.peerHistory
                                delegate: RowLayout {
                                    id: historyRow
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 44
                                    spacing: 10
                                    Icon {
                                        name: historyRow.modelData.kind === "video" ? "video" : "phone"
                                        size: 16
                                        color: historyRow.modelData.outcome === "missed" ? Theme.danger : Theme.text3
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.outcomeText(historyRow.modelData)
                                        textFormat: Text.PlainText
                                        color: historyRow.modelData.outcome === "missed" ? Theme.danger : Theme.text2
                                    }
                                    Text {
                                        text: Qt.formatDateTime(new Date(historyRow.modelData.startedAt), "d MMM yyyy · hh:mm")
                                        textFormat: Text.PlainText
                                        color: Theme.text3
                                        font.family: Theme.monoFamily
                                        font.pixelSize: 12
                                    }
                                }
                            }
                        }
                        RowLayout {
                            spacing: 6
                            Icon {
                                name: "lock"
                                size: 14
                                color: Theme.text3
                            }
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: "The server never records calls. This history exists only on this computer, encrypted with your key."
                                color: Theme.text3
                                font.pixelSize: 12
                            }
                        }
                    }

                    // A call link.
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.selectedLinkData !== null
                        spacing: 14
                        Text {
                            text: root.selectedLinkData ? root.selectedLinkData.name : ""
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.family: Theme.displayFamily
                            font.pixelSize: 28
                            font.weight: Font.Bold
                        }
                        Text {
                            text: root.selectedLinkData ? (root.selectedLinkData.kind === "video" ? "Video call link" : "Voice call link") : ""
                            textFormat: Text.PlainText
                            color: Theme.text3
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: urlText.implicitHeight + 24
                            radius: Theme.radiusControl
                            color: Theme.surface2
                            border.color: Theme.borderStrong
                            TextEdit {
                                id: urlText
                                anchors.fill: parent
                                anchors.margins: 12
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.WrapAnywhere
                                text: root.selectedLinkData ? root.selectedLinkData.url : ""
                                color: Theme.text2
                                font.family: Theme.monoFamily
                                font.pixelSize: 13
                            }
                        }
                        RowLayout {
                            spacing: 10
                            RvButton {
                                iconName: "copy"
                                text: "Copy link"
                                onClicked: {
                                    root.calls.copyText(root.selectedLinkData.url)
                                    root.notice = "Link copied."
                                }
                            }
                            RvButton {
                                primary: false
                                danger: true
                                iconName: "trash"
                                text: "Delete link"
                                onClicked: {
                                    root.calls.deleteLink(root.selectedLink)
                                    root.selectedLink = ""
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: "The part after # is the link's secret: it never reaches the server, and it seals the call setup end to end. Anyone with the whole link can ask to join, and you decide when it rings."
                            color: Theme.text3
                            font.pixelSize: 13
                        }
                    }

                    // Nothing selected: make or join a link.
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.selectedLinkData === null && !(root.selectedPeer.length > 0 && root.peerHistory.length > 0)
                        spacing: 16

                        Text {
                            text: "Call links"
                            color: Theme.text
                            font.family: Theme.displayFamily
                            font.pixelSize: 28
                            font.weight: Font.Bold
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: "Share a link to talk with someone who isn't in your contacts yet. Calls through it are relayed and encrypted end to end, like every call."
                            color: Theme.text2
                        }

                        Card {
                            Layout.fillWidth: true
                            Text {
                                text: "Create a link"
                                color: Theme.text
                                font.weight: Font.DemiBold
                            }
                            RvTextField {
                                id: linkName
                                Layout.fillWidth: true
                                placeholderText: "Name (only you and people with the link see it)"
                                maximumLength: 60
                            }
                            RowLayout {
                                spacing: 8
                                Chip {
                                    id: voiceKind
                                    text: "Voice"
                                    checked: !videoKind.checked
                                    onClicked: videoKind.checked = false
                                }
                                Chip {
                                    id: videoKind
                                    text: "Video"
                                    onClicked: checked = true
                                }
                                Item { Layout.fillWidth: true }
                                RvButton {
                                    iconName: "link"
                                    text: "Create and copy"
                                    enabled: root.account.pro
                                    onClicked: {
                                        root.notice = ""
                                        root.calls.createLink(linkName.text, videoKind.checked ? "video" : "voice")
                                        linkName.text = ""
                                    }
                                }
                            }
                            Text {
                                visible: !root.account.pro
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: "Call links are part of Pro (voice) and Premium (video)."
                                color: Theme.warn
                                font.pixelSize: 13
                            }
                        }

                        Card {
                            Layout.fillWidth: true
                            Text {
                                text: "Join a call link"
                                color: Theme.text
                                font.weight: Font.DemiBold
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                RvTextField {
                                    id: joinField
                                    Layout.fillWidth: true
                                    placeholderText: "Paste the whole link"
                                    font.family: Theme.monoFamily
                                    onAccepted: joinButton.clicked()
                                }
                                RvButton {
                                    id: joinButton
                                    text: "Join"
                                    enabled: joinField.text.trim().length > 0
                                    onClicked: {
                                        // A group invite link pasted here opens the join screen, like a call link joins the call.
                                        root.notice = App.groupInvites && App.groupInvites.isInviteLink(joinField.text)
                                                ? App.groupInvites.openInvite(joinField.text)
                                                : root.calls.joinLink(joinField.text)
                                        if (root.notice.length === 0)
                                            joinField.text = ""
                                    }
                                }
                            }
                            Text {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: "Anyone can join a link someone shares with them; the call rings the link's owner. A group call link from a group chat (or the whole message) joins that call right here; anyone in the group can join, on any plan."
                                color: Theme.text3
                                font.pixelSize: 13
                            }
                        }
                    }
                }
            }
        }
    }
}
