pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// The open chat: header, messages, composer.
Item {
    id: root
    required property ChatSession session
    required property PeopleSession people
    // Sending files is Pro-only, like on the web and Android; the server refuses it too.
    required property AccountSession account
    property string replyToId: ""
    property string replyPreview: ""
    // Editing one of your messages: the composer holds its new text.
    property string editingId: ""
    // Putting a chat's draft back in the composer isn't typing.
    property bool restoringDraft: false
    // The file message a menu or save dialog is acting on.
    property string fileMessageId: ""
    property string fileName: ""
    property string savedNotice: ""
    property string planNotice: ""
    property bool detailsOpen: true
    signal toggleDetails()
    // Device keys (preview): this chat is in per-device-encryption mode (pinned, or switched on).
    readonly property bool v3Mode: App.deviceKeys !== null && App.deviceKeys.chatActive
    // The file in progress, whichever way this chat sends them.
    readonly property string fileActivity: root.v3Mode ? App.deviceKeys.chatFileActivity : root.session.fileActivity
    // The chat a device-key message is being edited in (those messages aren't in root.session).
    property string editingV3Chat: ""
    // Replies don't exist on device keys, and an edit in progress belongs to the other list.
    onV3ModeChanged: {
        root.replyToId = ""
        root.replyPreview = ""
        if (root.editingId.length > 0) root.cancelEdit()
    }

    function syncDeviceKeys() {
        if (!App.deviceKeys) return
        const id = root.session.currentChatId
        if (id.length === 0) App.deviceKeys.closeChat()
        else App.deviceKeys.openChat(id, root.session.currentUserId, root.session.currentIsGroup, root.session.currentCommunityId.length === 0)
    }
    Component.onCompleted: root.syncDeviceKeys()
    Component.onDestruction: if (App.deviceKeys) App.deviceKeys.closeChat()
    Connections {
        target: root.session
        function onCurrentChatChanged() { root.syncDeviceKeys() }
    }
    Connections {
        target: App.deviceKeys
        function onChatSent(ok) {
            if (!ok) return
            composer.text = ""
            root.editingId = ""
            root.editingV3Chat = ""
        }
        function onChatFileDone(fileUrl) {
            root.session.discardPasted(fileUrl)  // only ever removes a pasted picture's temporary copy
        }
        function onChatFileSaved(path) {
            root.savedNotice = "Saved to " + path
            savedNoticeTimer.restart()
        }
    }

    ConfirmDialog {
        id: deviceKeysDialog
        title: "Use device keys in this chat?"
        message: "This chat will use per-device encryption (preview). Both of you need device keys set up. Messages sent this way live only on your devices. Replies, forwards, pins and polls aren't available here yet. Older messages stay in the normal view, and you can go back to the older protocol at any time."
        confirmText: "Use device keys"
        onConfirmed: App.deviceKeys.useDeviceKeys()
    }
    ConfirmDialog {
        id: olderProtocolDialog
        title: "Go back to the older protocol?"
        message: "New messages in this chat will use the current encryption again. Messages already sent with device keys stay on your devices."
        confirmText: "Use the older protocol"
        onConfirmed: App.deviceKeys.useOlderProtocol()
    }


    readonly property string otherUserId: session.currentUserId
    // A paid or free community: its page has the members and settings, and a channel's subscribers only read.
    readonly property string communityId: session.currentCommunityId
    readonly property bool readOnly: communityId.length > 0 && !session.currentCanPost
    readonly property bool online: otherUserId.length > 0 && App.live !== null && App.live.online.indexOf(otherUserId) >= 0
    readonly property var typers: App.live !== null && App.live.typingRevision >= 0 ? App.live.typingIn(session.currentChatId) : []

    // Selecting messages to forward or delete several at once: id -> true.
    property bool selecting: false
    property var selectedIds: ({})
    readonly property var selectedList: Object.keys(selectedIds)

    function startSelect(messageId) {
        root.selecting = true
        root.selectedIds = ({})
        if (messageId.length > 0) root.toggleSelected(messageId)
    }
    function stopSelect() {
        root.selecting = false
        root.selectedIds = ({})
    }
    function toggleSelected(messageId) {
        const next = Object.assign({}, root.selectedIds)
        if (next[messageId]) delete next[messageId]
        else next[messageId] = true
        root.selectedIds = next
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.visible && root.selecting
        onActivated: root.stopSelect()
    }

    // Search in this chat: the matching message ids (oldest first) and which one is shown.
    property bool searchOpen: false
    property var searchHits: []
    property int searchIndex: -1
    readonly property string highlightId: searchOpen && searchIndex >= 0 && searchIndex < searchHits.length
                                          ? searchHits[searchIndex] : ""

    function openSearch() {
        root.searchOpen = true
        searchField.forceActiveFocus()
        searchField.selectAll()
    }
    function closeSearch() {
        root.searchOpen = false
        searchField.text = ""
        root.searchHits = []
        root.searchIndex = -1
        composer.forceActiveFocus()
    }
    function runSearch() {
        const hits = root.session.messages.search(searchField.text)
        root.searchHits = hits
        root.searchIndex = hits.length - 1  // newest match first
        root.showHit()
    }
    // step: -1 for an older match, +1 for a newer one.
    function stepSearch(step) {
        if (root.searchHits.length === 0) return
        root.searchIndex = (root.searchIndex + step + root.searchHits.length) % root.searchHits.length
        root.showHit()
    }
    function showHit() {
        if (root.highlightId.length === 0) return
        const row = root.session.messages.indexOf(root.highlightId)
        if (row < 0) return
        messageList.pinnedToEnd = false
        messageList.positionViewAtIndex(row, ListView.Center)
    }

    Shortcut {
        sequence: StandardKey.Find
        enabled: root.visible && root.session.currentChatId.length > 0
        onActivated: root.openSearch()
    }

    function openVerify() { safetyDialog.open() }
    function openGroupInfo() { groupDialog.open() }
    function call(kind) {
        if (!App.calls)
            return
        const problem = App.calls.startCall(root.session.currentChatId, kind)
        root.planNotice = problem
    }
    // Group calls run in the app: only a Premium member starts one (anyone in the group can join one that's
    // going on), and the call's link is posted in this chat once it's connected.
    function groupCall(kind) {
        if (!App.groupCalls)
            return
        const going = Object.keys(App.groupCalls.callIn(root.session.currentChatId)).length > 0
        if (!going && !root.account.premium) {
            root.planNotice = "Only Premium members can start group " + kind + " calls. Anyone in the group can join one."
            return
        }
        root.planNotice = App.groupCalls.start(root.session.currentChatId, kind)
    }

    Connections {
        target: root.account
        function onAccountChanged() {
            if (root.account.pro) root.planNotice = ""
        }
    }

    function showFileMenu(messageId, name) {
        root.fileMessageId = messageId
        root.fileName = name
        fileMenu.popup()
    }
    function showV3FileMenu(messageId, name, canOpen) {
        root.fileMessageId = messageId
        root.fileName = name
        v3FileMenu.canOpen = canOpen
        v3FileMenu.popup()
    }

    SendFilesDialog {
        id: sendFilesDialog
        chats: root.session
        deviceKeys: root.v3Mode
    }
    PollDialog {
        id: pollDialog
        chats: root.session
    }

    // "+" in a message's reaction bar: react with any emoji.
    EmojiPicker {
        id: reactionPicker
        property string messageId: ""
        property bool deviceKeys: false
        function show(messageId, deviceKeys) {
            reactionPicker.messageId = messageId
            reactionPicker.deviceKeys = deviceKeys === true
            reactionPicker.open()
        }
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        onPicked: (emoji) => {
            reactionPicker.close()
            if (reactionPicker.deviceKeys) App.deviceKeys.reactChat(reactionPicker.messageId, emoji)
            else root.session.react(reactionPicker.messageId, emoji)
        }
    }

    Shortcut {
        sequence: "Ctrl+Shift+J"
        enabled: root.visible && root.session.currentChatId.length > 0
        onActivated: emojiButton.togglePicker()
    }
    // Which pinned message the bar shows (it cycles through them on click).
    property int pinIndex: 0
    function showMessage(messageId) {
        const row = root.session.messages.indexOf(messageId)
        if (row < 0) {
            root.session.loadOlder()
            return
        }
        messageList.pinnedToEnd = false
        messageList.positionViewAtIndex(row, ListView.Center)
        flashTimer.messageId = messageId
        flashTimer.restart()
    }
    // The message a pinned-bar click jumped to is outlined for a moment.
    Timer {
        id: flashTimer
        property string messageId: ""
        interval: 1600
        onTriggered: messageId = ""
    }

    // Dropped or pasted files: only on plans that can send files (the server refuses otherwise too).
    function offerFiles(urls) {
        const local = urls.filter(u => String(u).startsWith("file:"))
        if (local.length === 0) return
        if (!root.account.pro) {
            for (const url of local) root.session.discardPasted(String(url))
            root.planNotice = "Sending files isn't included in your plan. Upgrade to Pro in Settings to send files."
            return
        }
        root.planNotice = ""
        sendFilesDialog.openFor(local.map(u => String(u)))
    }

    // Drop files anywhere on the chat to send them.
    DropArea {
        id: fileDrop
        anchors.fill: parent
        z: 10
        enabled: !root.readOnly
        keys: ["text/uri-list"]
        onDropped: (drop) => {
            if (!drop.hasUrls) return
            drop.acceptProposedAction()
            root.offerFiles(drop.urls)
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 10
            visible: fileDrop.containsDrag
            radius: Theme.radiusCard
            color: Qt.alpha(Theme.bg, 0.85)
            border.color: Theme.accent
            border.width: 2
            Column {
                anchors.centerIn: parent
                spacing: 8
                Icon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    name: "upload"
                    size: 32
                    color: Theme.accent
                }
                Text {
                    text: "Drop to send to " + root.session.currentTitle
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    ImageViewer {
        id: imageViewer
        canOpen: root.session.canOpenFile(imageViewer.messageId)
        onSaveRequested: (messageId) => {
            imageViewer.close()
            root.fileMessageId = messageId
            root.fileName = imageViewer.title
            saveDialog.selectedFile = saveDialog.currentFolder + "/" + encodeURIComponent(imageViewer.title)
            App.lock.expectExternalReturn()
            saveDialog.open()
        }
        onOpenRequested: (messageId) => {
            App.lock.expectExternalReturn()
            root.session.openFile(messageId)
        }
    }

    ForwardDialog {
        id: forwardDialog
        chats: root.session
        canSendFiles: root.account.pro
        onForwarded: root.stopSelect()
    }

    SafetyNumberDialog {
        id: safetyDialog
        chats: root.session
    }
    CommunityReportDialog {
        id: communityReport
        communities: App.communities
    }

    GroupInfoDialog {
        id: groupDialog
        chats: root.session
        people: root.people
    }

    FileDialog {
        id: attachDialog
        title: "Send a file"
        fileMode: FileDialog.OpenFile
        onAccepted: {
            if (root.v3Mode) App.deviceKeys.sendFile(selectedFile)
            else root.session.attachFile(selectedFile)
        }
    }
    FileDialog {
        id: saveDialog
        title: "Save file"
        fileMode: FileDialog.SaveFile
        currentFolder: StandardPaths.writableLocation(StandardPaths.DownloadLocation)
        onAccepted: {
            if (root.v3Mode) App.deviceKeys.saveFile(root.fileMessageId, selectedFile)
            else root.session.saveFile(root.fileMessageId, selectedFile)
        }
    }

    // Only for its hex() helper, to color the menu's icons.
    Icon {
        id: menuIcon
        visible: false
    }

    Menu {
        id: fileMenu
        MenuItem {
            icon.source: "image://icon/external-link/" + menuIcon.hex(Theme.text)
            text: root.session.canOpenFile(root.fileMessageId) ? "Open" : "Open (not allowed for programs)"
            enabled: root.session.canOpenFile(root.fileMessageId) && root.session.fileActivity.length === 0
            onTriggered: {
                App.lock.expectExternalReturn()
                root.session.openFile(root.fileMessageId)
            }
        }
        MenuItem {
            icon.source: "image://icon/download/" + menuIcon.hex(Theme.text)
            text: "Save as…"
            enabled: root.session.fileActivity.length === 0
            onTriggered: {
                saveDialog.selectedFile = saveDialog.currentFolder + "/" + encodeURIComponent(root.fileName)
                App.lock.expectExternalReturn()
                saveDialog.open()
            }
        }
    }

    // A device-key file. Programs can only be saved, like in the normal list.
    Menu {
        id: v3FileMenu
        property bool canOpen: false
        MenuItem {
            icon.source: "image://icon/external-link/" + menuIcon.hex(Theme.text)
            text: v3FileMenu.canOpen ? "Open" : "Open (not allowed for programs)"
            enabled: v3FileMenu.canOpen && root.fileActivity.length === 0
            onTriggered: {
                App.lock.expectExternalReturn()
                App.deviceKeys.openFile(root.fileMessageId)
            }
        }
        MenuItem {
            icon.source: "image://icon/download/" + menuIcon.hex(Theme.text)
            text: "Save as…"
            enabled: root.fileActivity.length === 0
            onTriggered: {
                saveDialog.selectedFile = saveDialog.currentFolder + "/" + encodeURIComponent(root.fileName)
                App.lock.expectExternalReturn()
                saveDialog.open()
            }
        }
    }

    // What you can do with one device-key message. Reply, forward and pin have no envelope on device keys yet.
    Menu {
        id: v3Menu
        property string messageId: ""
        property string text: ""
        property bool canReact: false
        property bool canEdit: false
        property bool canDelete: false

        function show(messageId, text, canReact, canEdit, canDelete) {
            v3Menu.messageId = messageId
            v3Menu.text = text
            v3Menu.canReact = canReact
            v3Menu.canEdit = canEdit
            v3Menu.canDelete = canDelete
            popup()
        }

        MenuItem {
            text: "React…"
            icon.source: "image://icon/smile/" + menuIcon.hex(Theme.text)
            enabled: v3Menu.canReact
            onTriggered: reactionPicker.show(v3Menu.messageId, true)
        }
        MenuItem {
            text: "Edit"
            icon.source: "image://icon/pen/" + menuIcon.hex(Theme.text)
            visible: v3Menu.canEdit
            height: visible ? implicitHeight : 0
            onTriggered: root.startV3Edit(v3Menu.messageId, v3Menu.text)
        }
        MenuItem {
            text: "Copy text"
            icon.source: "image://icon/copy/" + menuIcon.hex(Theme.text)
            visible: v3Menu.canReact && v3Menu.text.length > 0
            height: visible ? implicitHeight : 0
            onTriggered: App.copySecret(v3Menu.text)
        }
        MenuItem {
            text: "Reply"
            icon.source: "image://icon/reply/" + menuIcon.hex(Theme.text)
            enabled: false
        }
        MenuItem {
            text: "Forward…"
            icon.source: "image://icon/share/" + menuIcon.hex(Theme.text)
            enabled: false
        }
        MenuItem {
            text: "Pin"
            icon.source: "image://icon/pin/" + menuIcon.hex(Theme.text)
            enabled: false
        }
        MenuSeparator {
            visible: v3Menu.canDelete
            height: visible ? implicitHeight : 0
        }
        MenuItem {
            text: "Delete for everyone"
            icon.source: "image://icon/trash/" + menuIcon.hex(Theme.danger)
            visible: v3Menu.canDelete
            height: visible ? implicitHeight : 0
            onTriggered: v3DeleteDialog.open()
        }
    }

    // Everything you can do with one message (the "More" button or a right-click).
    Menu {
        id: messageMenu
        property string messageId: ""
        property bool fromMe: false
        property string kind: ""
        property string text: ""
        readonly property bool readable: kind !== "undecryptable"

        function show(messageId, fromMe, kind, text) {
            messageMenu.messageId = messageId
            messageMenu.fromMe = fromMe
            messageMenu.kind = kind
            messageMenu.text = text
            popup()
        }

        MenuItem {
            text: "Reply"
            icon.source: "image://icon/reply/" + menuIcon.hex(Theme.text)
            enabled: messageMenu.readable
            visible: !root.readOnly
            height: visible ? implicitHeight : 0
            onTriggered: root.startReply(messageMenu.messageId)
        }
        MenuItem {
            text: "Edit"
            icon.source: "image://icon/pen/" + menuIcon.hex(Theme.text)
            visible: messageMenu.fromMe && messageMenu.kind === "text" && root.session.canEdit(messageMenu.messageId)
            height: visible ? implicitHeight : 0
            onTriggered: root.startEdit(messageMenu.messageId)
        }
        MenuItem {
            readonly property bool pinned: messageMenu.messageId.length > 0 && root.session.isPinned(messageMenu.messageId)
            text: pinned ? "Unpin" : "Pin"
            icon.source: "image://icon/pin/" + menuIcon.hex(Theme.text)
            enabled: messageMenu.readable
            visible: !root.readOnly
            height: visible ? implicitHeight : 0
            onTriggered: pinned ? root.session.unpin(messageMenu.messageId) : root.session.pin(messageMenu.messageId)
        }
        MenuItem {
            text: "Select"
            icon.source: "image://icon/check/" + menuIcon.hex(Theme.text)
            onTriggered: root.startSelect(messageMenu.messageId)
        }
        MenuItem {
            text: "Forward…"
            icon.source: "image://icon/share/" + menuIcon.hex(Theme.text)
            enabled: messageMenu.readable
            onTriggered: forwardDialog.openFor([messageMenu.messageId], messageMenu.kind === "file")
        }
        MenuItem {
            text: "Copy text"
            icon.source: "image://icon/copy/" + menuIcon.hex(Theme.text)
            visible: messageMenu.kind === "text"
            height: visible ? implicitHeight : 0
            onTriggered: App.copySecret(messageMenu.text)
        }
        MenuItem {
            // Communities: the reporter chooses whether to share the decrypted text.
            text: "Report…"
            icon.source: "image://icon/flag/" + menuIcon.hex(Theme.danger)
            visible: root.communityId.length > 0 && !messageMenu.fromMe && App.communities !== null
            height: visible ? implicitHeight : 0
            onTriggered: communityReport.reportMessage(root.communityId, messageMenu.messageId, root.session.senderOf(messageMenu.messageId),
                                                       messageMenu.readable ? root.session.textOf(messageMenu.messageId) : "")
        }
        MenuSeparator {}
        MenuItem {
            text: "Delete for me"
            icon.source: "image://icon/trash/" + menuIcon.hex(Theme.text)
            onTriggered: deleteForMeDialog.open()
        }
        MenuItem {
            text: "Delete for everyone"
            icon.source: "image://icon/trash/" + menuIcon.hex(Theme.danger)
            visible: messageMenu.fromMe && messageMenu.readable
            height: visible ? implicitHeight : 0
            onTriggered: deleteForEveryoneDialog.open()
        }
    }

    // Every version of an edited message.
    RvDialog {
        id: historyDialog
        property var versions: []
        title: "Edit history"
        width: 460

        function show(messageId) {
            historyDialog.versions = root.session.editHistory(messageId)
            open()
        }

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 10
            Repeater {
                model: historyDialog.versions
                delegate: ColumnLayout {
                    id: version
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: (version.modelData.current ? "Latest · " : "") + version.modelData.time
                        textFormat: Text.PlainText
                        color: version.modelData.current ? Theme.accent : Theme.text3
                        font.pixelSize: Theme.fontSmall
                    }
                    Text {
                        Layout.fillWidth: true
                        text: version.modelData.text
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: version.modelData.current ? Theme.text : Theme.text2
                    }
                }
            }
        }
    }

    // Links open only after you've seen the whole address.
    ConfirmDialog {
        id: linkDialog
        property string url: ""
        title: "Open this link?"
        message: linkDialog.url + "\n\nIt opens in your browser. Check the address first: a link can look like one site and go to another."
        confirmText: "Open link"
        function ask(link) {
            // This app's own group call link joins the call here, not in a browser.
            if (App.groupCalls && App.groupCalls.isOwnCallLink(link)) {
                root.planNotice = App.groupCalls.joinLink(link)
                return
            }
            // So does a group invite link (the join screen opens), a channel's link,
            // and a community's (its page opens, with the invitation the link carries).
            if (App.groupInvites && App.groupInvites.isOwnInviteLink(link)) {
                App.groupInvites.openInvite(link)
                return
            }
            if (App.channels && App.channels.isOwnChannelLink(link)) {
                App.channels.openChannelLink(link)
                return
            }
            if (App.communities && App.communities.isOwnCommunityLink(link)) {
                App.communities.openLink(link)
                return
            }
            if (!/^https?:\/\//i.test(link)) return
            linkDialog.url = link
            open()
        }
        onConfirmed: {
            App.lock.expectExternalReturn()
            Qt.openUrlExternally(linkDialog.url)
        }
    }

    // Deleting several selected messages: for everyone only when they're all yours.
    RvDialog {
        id: deleteManyDialog
        readonly property bool allMine: root.session.allFromMe(root.selectedList)
        title: root.selectedList.length === 1 ? "Delete 1 message?" : "Delete " + root.selectedList.length + " messages?"
        width: 460
        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 16
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: deleteManyDialog.allMine
                      ? "Delete them from this computer only, or have the server erase them for everyone in the chat (this can't be undone)."
                      : "They're removed from this computer only; the others in the chat keep them. (Only your own messages can be deleted for everyone.)"
                textFormat: Text.PlainText
                color: Theme.text2
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                RvButton {
                    primary: false
                    text: "Cancel"
                    onClicked: deleteManyDialog.close()
                }
                RvButton {
                    primary: !deleteManyDialog.allMine
                    danger: true
                    text: "Delete for me"
                    onClicked: {
                        root.session.deleteManyForMe(root.selectedList)
                        deleteManyDialog.close()
                        root.stopSelect()
                    }
                }
                RvButton {
                    visible: deleteManyDialog.allMine
                    danger: true
                    text: "Delete for everyone"
                    onClicked: {
                        root.session.deleteManyForEveryone(root.selectedList)
                        deleteManyDialog.close()
                        root.stopSelect()
                    }
                }
            }
        }
    }

    ConfirmDialog {
        id: deleteForMeDialog
        title: "Delete for me?"
        message: root.session.currentIsGroup
                 ? "This message is removed from this computer only. Everyone else in the group still has it."
                 : "This message is removed from this computer only. " + root.session.currentTitle + " still has it."
        confirmText: "Delete for me"
        danger: true
        onConfirmed: root.session.deleteForMe(messageMenu.messageId)
    }
    ConfirmDialog {
        id: deleteForEveryoneDialog
        title: "Delete for everyone?"
        message: "The server erases this message (and any file in it) for everyone in the chat. This can't be undone."
        confirmText: "Delete for everyone"
        danger: true
        onConfirmed: root.session.deleteForEveryone(messageMenu.messageId)
    }

    ConfirmDialog {
        id: v3DeleteDialog
        title: "Delete for everyone?"
        message: "This message (and any file in it) is deleted for everyone in the chat, on every device that has it. This can't be undone."
        confirmText: "Delete for everyone"
        danger: true
        onConfirmed: App.deviceKeys.deleteChat(v3Menu.messageId)
    }

    Timer {
        id: savedNoticeTimer
        interval: 6000
        onTriggered: root.savedNotice = ""
    }

    function startReply(messageId) {
        if (root.editingId.length > 0) root.cancelEdit()
        root.replyToId = messageId
        root.replyPreview = root.session.previewOf(messageId)
        composer.forceActiveFocus()
    }

    function startEdit(messageId) {
        if (messageId.length === 0 || !root.session.canEdit(messageId)) return
        root.replyToId = ""
        root.replyPreview = ""
        root.editingId = messageId
        composer.text = root.session.textOf(messageId)
        composer.forceActiveFocus()
        composer.cursorPosition = composer.length
    }
    function startV3Edit(messageId, text) {
        root.replyToId = ""
        root.replyPreview = ""
        root.editingId = messageId
        root.editingV3Chat = root.session.currentChatId
        composer.text = text
        composer.forceActiveFocus()
        composer.cursorPosition = composer.length
    }
    function cancelEdit() {
        root.editingId = ""
        root.editingV3Chat = ""
        composer.text = ""
    }

    // @mentions in a group: what's typed after "@" before the cursor, and the members it matches.
    property int mentionStart: -1
    property var mentionMatches: []
    property int mentionIndex: 0
    function updateMentions() {
        if (!root.session.currentIsGroup || composer.selectionStart !== composer.selectionEnd) return clearMentions()
        const before = composer.text.substring(0, composer.cursorPosition)
        const at = before.lastIndexOf("@")
        if (at < 0 || (at > 0 && !/\s|[(\[{"']/.test(before.charAt(at - 1)))) return clearMentions()
        const query = before.substring(at + 1).toLowerCase()
        if (query.indexOf("\n") >= 0 || query.length > 64) return clearMentions()
        const matches = root.session.currentMembers.filter(m => !m.isSelf && m.handle.toLowerCase().startsWith(query))
        if (matches.length === 0) return clearMentions()
        root.mentionStart = at
        root.mentionMatches = matches.slice(0, 6)
        root.mentionIndex = Math.min(root.mentionIndex, root.mentionMatches.length - 1)
    }
    function clearMentions() {
        root.mentionStart = -1
        root.mentionMatches = []
        root.mentionIndex = 0
    }
    function pickMention(index) {
        const member = root.mentionMatches[index]
        if (!member || root.mentionStart < 0) return
        const start = root.mentionStart
        composer.remove(start, composer.cursorPosition)
        composer.insert(start, "@" + member.handle + " ")
        composer.cursorPosition = start + member.handle.length + 2
        clearMentions()
    }

    // Ctrl+B and friends: wrap the selection (or the cursor) in a formatting marker.
    function wrapSelection(marker) {
        const start = composer.selectionStart
        const end = composer.selectionEnd
        const selected = composer.selectedText
        composer.remove(start, end)
        composer.insert(start, marker + selected + marker)
        if (selected.length === 0)
            composer.cursorPosition = start + marker.length
        else
            composer.select(start, start + selected.length + 2 * marker.length)
    }

    function send() {
        const text = composer.text.trim()
        if (text.length === 0 || root.session.sending)
            return
        if (root.v3Mode) {
            if (root.editingId.length > 0) App.deviceKeys.editChat(root.editingId, text)
            else App.deviceKeys.sendChat(text)
            return
        }
        if (root.editingId.length > 0) {
            root.session.editMessage(root.editingId, text)
            return
        }
        // To someone who charges for messages: the paid message (or free safety contact) this send uses.
        const paid = App.earnings ? App.earnings.sendOptions() : ({ ok: true })
        if (!paid.ok)
            return  // pay first: the bar above the composer says how
        root.session.sendMessage(text, root.replyToId, paid.entitlementId || "", paid.safetyContact === true)
    }

    Connections {
        target: root.session
        function onFileSaved(path) {
            root.savedNotice = "Saved to " + path
            savedNoticeTimer.restart()
        }
        function onSent(ok) {
            if (ok) {
                composer.text = ""
                root.replyToId = ""
                root.replyPreview = ""
            }
        }
        function onEdited(ok) {
            if (ok) {
                composer.text = ""
                root.editingId = ""
            }
        }
        function onCurrentChatChanged() {
            // A group: is a call going on there? (The Join button below.)
            if (root.session.currentIsGroup && App.groupCalls) App.groupCalls.refreshActive()
            // A different chat: drop a reply target that isn't in it, and start at its latest message.
            if (root.replyToId.length > 0 && root.session.previewOf(root.replyToId).length === 0) {
                root.replyToId = ""
                root.replyPreview = ""
            }
            if (root.editingId.length > 0 && (root.editingV3Chat.length > 0 ? root.editingV3Chat !== root.session.currentChatId
                                                                            : root.session.textOf(root.editingId).length === 0)) root.cancelEdit()
            if (root.session.currentChatId !== messageList.shownChatId) {
                // Keep what was typed in the chat being left, and bring back this one's.
                if (messageList.shownChatId.length > 0 && root.editingId.length === 0)
                    root.session.setDraft(messageList.shownChatId, composer.text)
                root.editingId = ""
                root.replyToId = ""
                root.replyPreview = ""
                root.restoringDraft = true
                composer.text = root.session.draftFor(root.session.currentChatId)
                composer.cursorPosition = composer.length
                root.restoringDraft = false
                messageList.shownChatId = root.session.currentChatId
                messageList.pinnedToEnd = true
                messageList.anchorId = ""
                if (root.searchOpen) root.closeSearch()
                root.stopSelect()
                Qt.callLater(messageList.positionViewAtEnd)
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- Header ---
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.headerHeight
            color: Theme.bg

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 16
                spacing: 12

                Avatar {
                    initial: root.session.currentTitle.length > 0 ? root.session.currentTitle.charAt(0).toUpperCase() : "?"
                    group: root.session.currentIsGroup
                    source: root.session.currentAvatarUrl
                    communityImage: root.session.currentCommunityImage
                    tintKey: root.session.currentIsGroup ? root.session.currentChatId : root.otherUserId
                    online: root.online
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        text: root.session.currentTitle
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                    }
                    RowLayout {
                        spacing: 5
                        Icon {
                            visible: root.typers.length === 0 && (root.session.currentVerified || root.session.currentKeyChanged)
                            name: root.session.currentKeyChanged ? "triangle-alert" : "shield-check"
                            size: 13
                            color: root.session.currentKeyChanged ? Theme.warn : Theme.accent
                        }
                        Text {
                            text: root.typers.length > 0 ? "typing…"
                                : root.session.currentKeyChanged ? "Safety number changed"
                                : root.online ? "Online · " + root.session.currentSubtitle
                                : root.session.currentSubtitle
                            textFormat: Text.PlainText
                            color: root.typers.length > 0 || root.online ? Theme.accent
                                 : root.session.currentKeyChanged ? Theme.warn
                                 : root.session.currentVerified ? Theme.accent : Theme.text3
                            font.pixelSize: Theme.fontSmall + 1
                        }
                    }
                }

                IconButton {
                    visible: !root.session.currentIsGroup && App.calls !== null && App.calls.callsAvailable
                    iconName: "phone"
                    tip: "Voice call"
                    onClicked: root.call("voice")
                }
                IconButton {
                    visible: !root.session.currentIsGroup && App.calls !== null && App.calls.callsAvailable
                    iconName: "video"
                    tip: "Video call"
                    onClicked: root.call("video")
                }
                IconButton {
                    visible: root.session.currentIsGroup && !root.readOnly && App.groupCalls !== null && App.groupCalls.available
                    iconName: "phone"
                    tip: root.account.premium ? "Group voice call" : "Only Premium members can start group calls"
                    onClicked: root.groupCall("voice")
                }
                IconButton {
                    visible: root.session.currentIsGroup && !root.readOnly && App.groupCalls !== null && App.groupCalls.available
                    iconName: "video"
                    tip: root.account.premium ? "Group video call" : "Only Premium members can start group calls"
                    onClicked: root.groupCall("video")
                }
                IconButton {
                    iconName: "search"
                    toggled: root.searchOpen
                    tip: "Search this chat (Ctrl+F)"
                    onClicked: root.searchOpen ? root.closeSearch() : root.openSearch()
                }
                IconButton {
                    visible: !root.readOnly
                    iconName: "timer"
                    toggled: root.session.currentDisappearing > 0
                    tip: "Disappearing messages: " + root.session.currentDisappearingLabel
                    onClicked: timerMenu.popup(this, 0, height)

                    Menu {
                        id: timerMenu
                        Repeater {
                            model: root.session.disappearingOptions
                            delegate: MenuItem {
                                required property var modelData
                                text: modelData.label
                                checkable: true
                                checked: modelData.seconds === root.session.currentDisappearing
                                onTriggered: root.session.setDisappearing(modelData.seconds)
                            }
                        }
                    }
                }
                IconButton {
                    visible: !root.session.currentIsGroup
                    iconName: "shield-check"
                    iconColor: root.session.currentVerified ? Theme.accent : Theme.text2
                    tip: root.session.currentVerified ? "Verified: view safety number" : "Verify safety number"
                    onClicked: safetyDialog.open()
                }
                IconButton {
                    visible: root.session.currentIsGroup && root.communityId.length === 0
                    iconName: "info"
                    tip: "Group info"
                    onClicked: groupDialog.open()
                }
                IconButton {
                    visible: root.communityId.length > 0 && App.communities !== null
                    iconName: "info"
                    tip: "Community page"
                    onClicked: {
                        App.communities.open("community", root.communityId)
                        App.communities.showRequested()
                    }
                }
                IconButton {
                    iconName: "panel-right"
                    tip: root.detailsOpen ? "Hide chat details" : "Show chat details"
                    toggled: root.detailsOpen
                    onClicked: root.toggleDetails()
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.border
            }
        }

        // A key that failed its check isn't used: the chat says why, whatever else is going on.
        Rectangle {
            Layout.fillWidth: true
            visible: root.session.currentKeyProblem.length > 0
            implicitHeight: keyProblemRow.implicitHeight + 20
            color: Theme.warnSoft
            RowLayout {
                id: keyProblemRow
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 16
                spacing: 10
                Icon {
                    name: "triangle-alert"
                    size: 18
                    color: Theme.warn
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.session.currentKeyProblem
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall + 1
                }
            }
        }

        // Their key changed: say so until the new number is verified.
        Rectangle {
            Layout.fillWidth: true
            visible: root.session.currentKeyChanged
            implicitHeight: keyRow.implicitHeight + 20
            color: Theme.warnSoft
            RowLayout {
                id: keyRow
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 16
                spacing: 10
                Icon {
                    name: "triangle-alert"
                    size: 18
                    color: Theme.warn
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.session.currentTitle + "'s safety number changed. Someone may be intercepting. Sending is paused until you verify it."
                    textFormat: Text.PlainText
                    color: Theme.warn
                    font.pixelSize: 13
                }
                RvButton {
                    primary: false
                    text: "Verify"
                    implicitHeight: 32
                    onClicked: safetyDialog.open()
                }
            }
        }

        // Pinned messages: the bar shows one at a time; clicking jumps to it, then to the next.
        Rectangle {
            id: pinnedBar
            Layout.fillWidth: true
            readonly property var pins: root.session.currentPinned
            readonly property var shown: pins.length > 0 ? pins[Math.min(root.pinIndex, pins.length - 1)] : null
            visible: pins.length > 0 && !root.selecting
            implicitHeight: 48
            color: Theme.surface
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.border
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (!pinnedBar.shown) return
                    root.showMessage(pinnedBar.shown.messageId)
                    root.pinIndex = (root.pinIndex + 1) % pinnedBar.pins.length
                }
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 8
                spacing: 10
                // One bar per pin, the shown one lit.
                Column {
                    spacing: 2
                    Repeater {
                        model: pinnedBar.pins.length
                        delegate: Rectangle {
                            required property int index
                            width: 3
                            height: Math.max(6, 30 / pinnedBar.pins.length - 2)
                            radius: 1.5
                            color: index === Math.min(root.pinIndex, pinnedBar.pins.length - 1) ? Theme.accent : Theme.borderStrong
                        }
                    }
                }
                Icon {
                    name: "pin"
                    size: 15
                    color: Theme.accent
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        text: pinnedBar.pins.length > 1 ? "Pinned message " + (Math.min(root.pinIndex, pinnedBar.pins.length - 1) + 1) + " of " + pinnedBar.pins.length
                                                        : "Pinned message"
                        textFormat: Text.PlainText
                        color: Theme.accent
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: pinnedBar.shown ? pinnedBar.shown.preview : ""
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text2
                        font.pixelSize: Theme.fontSmall + 1
                    }
                }
                IconButton {
                    iconName: "x"
                    tip: "Unpin for everyone"
                    onClicked: if (pinnedBar.shown) root.session.unpin(pinnedBar.shown.messageId)
                }
            }
        }

        // Selecting: what you can do with the ticked messages.
        Rectangle {
            Layout.fillWidth: true
            visible: root.selecting
            implicitHeight: 52
            color: Theme.accentSoft
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.accentBorder
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 12
                spacing: 6
                IconButton {
                    iconName: "x"
                    tip: "Stop selecting (Esc)"
                    onClicked: root.stopSelect()
                }
                Text {
                    Layout.fillWidth: true
                    text: root.selectedList.length === 0 ? "Select messages" : root.selectedList.length + " selected"
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.weight: Font.DemiBold
                }
                RvButton {
                    primary: false
                    iconName: "share"
                    text: "Forward"
                    enabled: root.session.forwardable(root.selectedList).length > 0
                    onClicked: {
                        const ids = root.session.forwardable(root.selectedList)
                        forwardDialog.openFor(ids, root.session.anyFile(ids))
                    }
                }
                RvButton {
                    primary: false
                    danger: true
                    iconName: "trash"
                    text: "Delete"
                    enabled: root.selectedList.length > 0
                    onClicked: deleteManyDialog.open()
                }
            }
        }

        // Search this chat: Enter for the next older match, Shift+Enter for a newer one.
        Rectangle {
            Layout.fillWidth: true
            visible: root.searchOpen
            implicitHeight: 56
            color: Theme.bg
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.border
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 12
                spacing: 6
                SearchField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: "Search this chat"
                    onTextChanged: searchDebounce.restart()
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            event.accepted = true
                            if (searchDebounce.running) {
                                searchDebounce.stop()
                                root.runSearch()
                            } else {
                                root.stepSearch(event.modifiers & Qt.ShiftModifier ? 1 : -1)
                            }
                        } else if (event.key === Qt.Key_Escape) {
                            event.accepted = true
                            root.closeSearch()
                        }
                    }
                    Timer {
                        id: searchDebounce
                        interval: 200
                        onTriggered: root.runSearch()
                    }
                }
                Text {
                    visible: searchField.text.trim().length > 0
                    text: root.searchHits.length === 0 ? "No matches"
                        : (root.searchIndex + 1) + " of " + root.searchHits.length
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.pixelSize: Theme.fontSmall + 1
                }
                IconButton {
                    iconName: "chevron-down"
                    rotation: 180
                    tip: "Older match (Enter)"
                    enabled: root.searchHits.length > 1
                    onClicked: root.stepSearch(-1)
                }
                IconButton {
                    iconName: "chevron-down"
                    tip: "Newer match (Shift+Enter)"
                    enabled: root.searchHits.length > 1
                    onClicked: root.stepSearch(1)
                }
                RvButton {
                    visible: root.session.hasOlder && searchField.text.trim().length > 0
                    primary: false
                    text: root.session.loadingOlder ? "Loading…" : "Search older"
                    enabled: !root.session.loadingOlder
                    implicitHeight: 32
                    ToolTip.visible: hovered
                    ToolTip.text: "Only the messages loaded so far are searched. This loads older ones too."
                    onClicked: root.session.loadOlder()
                }
                IconButton {
                    iconName: "x"
                    tip: "Close search (Esc)"
                    onClicked: root.closeSearch()
                }
            }
        }

        // Your timer is on: say so, since it changes what happens to everything you send.
        Rectangle {
            Layout.fillWidth: true
            visible: root.session.currentDisappearing > 0
            implicitHeight: 30
            color: Theme.surface
            Row {
                anchors.centerIn: parent
                spacing: 6
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "timer"
                    size: 13
                    color: Theme.text3
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Messages you send disappear " + (root.session.currentDisappearing === 1
                          ? "right after sending" : root.session.currentDisappearingLabel.toLowerCase() + " after sending")
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.family: Theme.monoFamily
                    font.pixelSize: 12
                }
            }
        }

        // --- Device keys (preview): per-device encryption for this chat ---
        Rectangle {
            Layout.fillWidth: true
            visible: App.deviceKeys !== null && App.deviceKeys.trusted && root.session.currentCommunityId.length === 0
            implicitHeight: dkRow.implicitHeight + 14
            color: root.v3Mode ? Theme.accentSoft : Theme.surface
            RowLayout {
                id: dkRow
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 16
                spacing: 10
                Icon {
                    name: "shield-check"
                    size: 16
                    color: root.v3Mode ? Theme.accent : Theme.text3
                }
                Text {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    color: root.v3Mode ? Theme.accent : Theme.text3
                    font.pixelSize: Theme.fontSmall + 1
                    text: root.v3Mode
                          ? (App.deviceKeys && App.deviceKeys.chatProtocol === "signal"
                             ? "Signal sessions (development build), with post-quantum key agreement, to every device in this chat. "
                             : App.deviceKeys && App.deviceKeys.chatProtocol === "mixed"
                               ? "Device keys (preview): some devices in this chat use Signal sessions, others the device-key protocol. "
                               : "")
                            + "Device keys (preview). Messages here are encrypted to each device and kept only on your devices. Replies, forwards, pins and polls aren't available here yet; older messages are in the normal view."
                          : App.deviceKeys && App.deviceKeys.chatContact === "yes"
                            ? "Device keys (preview): " + (root.session.currentIsGroup ? "members of this group have" : root.session.currentTitle + " has")
                              + " per-device encryption set up. Send this chat with it."
                            : App.deviceKeys && App.deviceKeys.chatContact === "no"
                              ? "Device keys (preview): " + (root.session.currentIsGroup ? "no other member has" : root.session.currentTitle + " hasn't set up")
                                + " device keys yet, so this chat stays on the current encryption."
                              : "Device keys (preview): checking whether they have device keys…"
                }
                RvButton {
                    text: root.v3Mode ? "Use the older protocol" : "Use device keys"
                    primary: !root.v3Mode && App.deviceKeys !== null && App.deviceKeys.chatContact === "yes"
                    enabled: root.v3Mode || (App.deviceKeys !== null && App.deviceKeys.chatContact === "yes")
                    implicitHeight: 30
                    onClicked: root.v3Mode ? olderProtocolDialog.open() : deviceKeysDialog.open()
                }
            }
        }
        Banner {
            Layout.fillWidth: true
            text: root.v3Mode && App.deviceKeys ? App.deviceKeys.chatError : ""
        }
        // M5: messages held on the server because a contact's keys changed (they open once the change is accepted).
        Banner {
            Layout.fillWidth: true
            text: root.v3Mode && App.deviceKeys && App.deviceKeys.chatHeld > 0
                  ? (App.deviceKeys.chatHeld === 1 ? "A message is" : App.deviceKeys.chatHeld + " messages are")
                    + " on hold because a contact's device keys changed. Compare safety numbers (Verify) before you trust the new keys."
                  : ""
        }
        // M5: some of their devices got no copy of the last message.
        Banner {
            Layout.fillWidth: true
            error: false
            text: root.v3Mode && App.deviceKeys ? App.deviceKeys.chatNotice : ""
        }

        // --- Device-key messages (shown instead of the normal list while the chat is in device-key mode) ---
        ListView {
            id: v3List
            objectName: "v3List"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.v3Mode
            clip: true
            model: App.deviceKeys ? App.deviceKeys.chat : null
            spacing: 6
            topMargin: 12
            bottomMargin: 12
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            onCountChanged: Qt.callLater(positionViewAtEnd)
            delegate: Item {
                id: bubble
                required property string messageId
                required property string text
                required property bool fromMe
                required property string time
                required property string status
                required property bool deleted
                required property bool edited
                required property var reactions
                required property bool hasFile
                required property string fileName
                required property string fileSize
                required property bool canOpen
                required property bool canReact
                required property bool canEdit
                required property bool canDelete
                width: v3List.width
                implicitHeight: bubbleBody.implicitHeight + 4 + (bubbleReactions.visible ? bubbleReactions.implicitHeight + 2 : 0)

                function showMenu() {
                    if (bubble.status === "failed") return
                    v3Menu.show(bubble.messageId, bubble.deleted ? "" : bubble.text, bubble.canReact, bubble.canEdit, bubble.canDelete)
                }

                Rectangle {
                    id: bubbleBody
                    anchors.right: bubble.fromMe ? parent.right : undefined
                    anchors.left: bubble.fromMe ? undefined : parent.left
                    anchors.margins: 24
                    width: Math.min(bubble.width * 0.7, Math.max(bubbleText.implicitWidth, bubbleMeta.implicitWidth, bubbleFile.implicitWidth) + 24)
                    implicitHeight: bubbleColumn.implicitHeight + 16
                    radius: Theme.radiusCard
                    color: bubble.fromMe ? Theme.accentSoft : Theme.surface2
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        onClicked: bubble.showMenu()
                    }
                    Column {
                        id: bubbleColumn
                        anchors.fill: parent
                        anchors.margins: 8
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 2
                        Text {
                            id: bubbleText
                            width: parent.width
                            visible: !bubble.hasFile
                            text: bubble.text
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: bubble.deleted ? Theme.text3 : Theme.text
                            font.italic: bubble.deleted
                            font.pixelSize: Theme.fontSize + 1
                        }
                        // A file: click for Open and Save as.
                        Item {
                            id: bubbleFile
                            visible: bubble.hasFile
                            width: parent.width
                            implicitWidth: bubbleFileRow.implicitWidth
                            implicitHeight: bubbleFileRow.implicitHeight
                            Row {
                                id: bubbleFileRow
                                spacing: 8
                                Icon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "file"
                                    size: 20
                                    color: Theme.text2
                                }
                                Column {
                                    Text {
                                        width: Math.min(implicitWidth, bubble.width * 0.5)
                                        text: bubble.fileName
                                        textFormat: Text.PlainText
                                        elide: Text.ElideMiddle
                                        color: Theme.text
                                        font.pixelSize: Theme.fontSize + 1
                                    }
                                    Text {
                                        text: bubble.fileSize
                                        textFormat: Text.PlainText
                                        color: Theme.text3
                                        font.pixelSize: Theme.fontSmall
                                    }
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                enabled: bubble.status !== "failed"
                                Accessible.role: Accessible.Button
                                Accessible.name: bubble.fileName
                                onClicked: root.showV3FileMenu(bubble.messageId, bubble.fileName, bubble.canOpen)
                            }
                        }
                        Row {
                            id: bubbleMeta
                            spacing: 8
                            Text {
                                text: bubble.time
                                textFormat: Text.PlainText
                                color: Theme.text3
                                font.pixelSize: Theme.fontSmall
                            }
                            Text {
                                visible: bubble.edited
                                text: "edited"
                                color: Theme.text3
                                font.pixelSize: Theme.fontSmall
                            }
                            Text {
                                visible: bubble.status === "failed"
                                text: "Not sent"
                                color: Theme.danger
                                font.pixelSize: Theme.fontSmall
                            }
                            Text {
                                visible: bubble.status === "failed"
                                text: "Retry"
                                color: Theme.accent
                                font.pixelSize: Theme.fontSmall
                                font.underline: true
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: App.deviceKeys.retryChat(bubble.messageId)
                                }
                            }
                            IconButton {
                                visible: bubble.status !== "failed"
                                implicitWidth: 22
                                implicitHeight: 18
                                iconName: "dots"
                                iconSize: 14
                                tip: "More"
                                onClicked: bubble.showMenu()
                            }
                        }
                    }
                }
                // Reactions under the bubble; click one to add or remove yours.
                Flow {
                    id: bubbleReactions
                    visible: bubble.reactions.length > 0
                    anchors.top: bubbleBody.bottom
                    anchors.topMargin: 2
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 24
                    anchors.rightMargin: 24
                    layoutDirection: bubble.fromMe ? Qt.RightToLeft : Qt.LeftToRight
                    spacing: 4
                    Repeater {
                        model: bubble.reactions
                        delegate: Rectangle {
                            id: v3Chip
                            required property var modelData
                            implicitWidth: v3ChipText.implicitWidth + 14
                            implicitHeight: 24
                            radius: 12
                            color: modelData.mine ? Theme.accentSoft : Theme.surface
                            border.color: modelData.mine ? Theme.accent : Theme.border
                            Text {
                                id: v3ChipText
                                anchors.centerIn: parent
                                text: v3Chip.modelData.emoji + (v3Chip.modelData.count > 1 ? " " + v3Chip.modelData.count : "")
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: 13
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: App.deviceKeys.reactChat(bubble.messageId, v3Chip.modelData.emoji)
                            }
                        }
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: v3List.count === 0
                text: "No device-key messages yet. Say hello."
                color: Theme.text3
            }
        }

        // --- Messages ---
        ListView {
            id: messageList
            objectName: "messageList"
            visible: !root.v3Mode
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.session.messages
            spacing: 4
            topMargin: 12
            bottomMargin: 12
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                onPressedChanged: if (!pressed) messageList.settle()
            }

            // Follow new messages only while already at the bottom, so reading history isn't interrupted.
            property bool pinnedToEnd: true
            property string shownChatId: ""
            // The message at the top when older ones were asked for: kept in place once they arrive.
            property string anchorId: ""
            onMovementEnded: settle()
            onCountChanged: if (pinnedToEnd && anchorId.length === 0) Qt.callLater(positionViewAtEnd)
            // A bar appearing above (search, the timer notice) shrinks the list: stay on the newest message.
            onHeightChanged: if (pinnedToEnd) Qt.callLater(positionViewAtEnd)
            // A picture finishing loading makes its bubble taller: still the newest message.
            onContentHeightChanged: if (pinnedToEnd && !moving) Qt.callLater(positionViewAtEnd)

            function settle() {
                pinnedToEnd = atYEnd
                if (atYBeginning && !atYEnd) loadOlder()
            }
            function loadOlder() {
                if (!root.session.hasOlder || root.session.loadingOlder) return
                const top = indexAt(width / 2, contentY + (headerItem ? headerItem.height : 0) + 8)
                anchorId = root.session.messages.idAt(Math.max(0, top))
                pinnedToEnd = false
                root.session.loadOlder()
            }

            Connections {
                target: root.session
                function onHistoryChanged() {
                    // Older messages arrived while searching: search them too, staying on the current match.
                    if (!root.session.loadingOlder && root.searchOpen && searchField.text.trim().length > 0) {
                        const current = root.highlightId
                        const hits = root.session.messages.search(searchField.text)
                        root.searchHits = hits
                        root.searchIndex = hits.indexOf(current) >= 0 ? hits.indexOf(current) : hits.length - 1
                    }
                    if (root.session.loadingOlder || messageList.anchorId.length === 0) return
                    const row = root.session.messages.indexOf(messageList.anchorId)
                    messageList.anchorId = ""
                    if (row >= 0) Qt.callLater(() => messageList.positionViewAtIndex(row, ListView.Beginning))
                }
            }

            header: Item {
                width: messageList.width
                height: root.session.hasOlder ? 44 : 0
                visible: root.session.hasOlder
                RvButton {
                    anchors.centerIn: parent
                    visible: !root.session.loadingOlder
                    primary: false
                    text: "Load older messages"
                    implicitHeight: 30
                    onClicked: messageList.loadOlder()
                }
                BusyIndicator {
                    anchors.centerIn: parent
                    width: 26
                    height: 26
                    running: root.session.loadingOlder
                    visible: running
                }
            }

            delegate: MessageDelegate {
                width: messageList.width
                highlighted: messageId === root.highlightId || messageId === flashTimer.messageId
                onVoteRequested: (messageId, options) => root.session.vote(messageId, options)
                onEndPollRequested: (messageId) => root.session.endPoll(messageId)
                selectMode: root.selecting
                selected: root.selectedIds[messageId] === true
                onSelectToggled: (messageId) => root.toggleSelected(messageId)
                imageInfo: isImage && root.session.imagesRevision >= 0 ? root.session.imageInfo(messageId) : ({})
                onImageLoadRequested: (messageId, force) => root.session.loadImage(messageId, force)
                onImageOpened: (messageId) => imageViewer.show(messageId, text, sender + " · " + timeText)
                searchTerm: root.searchOpen ? searchField.text.trim() : ""
                onReplyRequested: (messageId) => root.startReply(messageId)
                onReactRequested: (messageId, emoji) => root.session.react(messageId, emoji)
                onReactPickerRequested: (messageId) => reactionPicker.show(messageId)
                onFileRequested: (messageId, name) => root.showFileMenu(messageId, name)
                onMenuRequested: (messageId, fromMe, kind, text) => messageMenu.show(messageId, fromMe, kind, text)
                onHistoryRequested: (messageId) => historyDialog.show(messageId)
                onRetryQueuedRequested: (messageId) => root.session.retryQueued(messageId)
                onDiscardQueuedRequested: (messageId) => root.session.discardQueued(messageId)
                onLinkRequested: (url) => linkDialog.ask(url)
            }

            Text {
                anchors.centerIn: parent
                visible: messageList.count === 0
                text: "No messages yet. Say hello."
                color: Theme.text3
            }
        }

        // --- Someone's typing ---
        Item {
            Layout.fillWidth: true
            implicitHeight: root.typers.length > 0 ? 28 : 0
            visible: root.typers.length > 0
            Row {
                anchors.left: parent.left
                anchors.leftMargin: 24
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                Rectangle {
                    width: 44
                    height: 24
                    radius: 12
                    color: Theme.selected
                    Row {
                        anchors.centerIn: parent
                        spacing: 4
                        Repeater {
                            model: 3
                            delegate: Rectangle {
                                id: dot
                                required property int index
                                width: 6
                                height: 6
                                radius: 3
                                color: Theme.text3
                                SequentialAnimation on opacity {
                                    running: root.typers.length > 0
                                    loops: Animation.Infinite
                                    PauseAnimation { duration: dot.index * 150 }
                                    NumberAnimation { from: 0.3; to: 1; duration: 300 }
                                    NumberAnimation { from: 1; to: 0.3; duration: 300 }
                                    PauseAnimation { duration: (2 - dot.index) * 150 }
                                }
                            }
                        }
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.session.currentIsGroup ? "Someone is typing" : root.session.currentTitle + " is typing"
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.pixelSize: 12
                }
            }
        }

        // --- Composer ---
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: composerColumn.implicitHeight + 20
            color: Theme.bg

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.border
            }

            ColumnLayout {
                id: composerColumn
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8

                // Tor routing (docs/tor-routing-plan.md): while Tor is down, what's sent waits, sealed on this computer.
                Banner {
                    Layout.fillWidth: true
                    text: App.tor && App.tor.enabled && App.tor.state === "unavailable"
                          ? "Tor connection unavailable. Your private messages and media will remain queued until the secure connection is restored."
                          : ""
                }
                Banner {
                    Layout.fillWidth: true
                    error: false
                    text: App.tor && App.tor.enabled && App.tor.state === "connecting" && root.session.queuedCount > 0
                          ? "Connecting to Tor… Queued messages go out as soon as it's ready."
                          : ""
                }

                // Problems, file progress and confirmations.
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.session.error.length > 0
                    Banner {
                        Layout.fillWidth: true
                        text: root.session.error
                    }
                    RvButton {
                        primary: false
                        iconName: "x"
                        implicitHeight: 30
                        ToolTip.visible: hovered
                        ToolTip.text: "Dismiss"
                        onClicked: root.session.clearError()
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.fileActivity.length > 0
                    spacing: 8
                    BusyIndicator {
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 22
                        running: parent.visible
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.fileActivity
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        color: Theme.text2
                        font.pixelSize: Theme.fontSmall + 1
                    }
                }
                Banner {
                    Layout.fillWidth: true
                    error: false
                    text: root.savedNotice
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.planNotice.length > 0
                    Banner {
                        Layout.fillWidth: true
                        text: root.planNotice
                    }
                    RvButton {
                        primary: false
                        iconName: "x"
                        implicitHeight: 30
                        ToolTip.visible: hovered
                        ToolTip.text: "Dismiss"
                        onClicked: root.planNotice = ""
                    }
                }

                // Someone who charges for messages: what it costs, Pay, and the free safety contact.
                PaidMessageBar {
                    Layout.fillWidth: true
                    earnings: App.earnings
                    name: root.session.currentTitle
                }

                // A call is going on in this group: join it (any plan).
                Rectangle {
                    id: joinBanner
                    readonly property var going: {
                        const revision = App.groupCalls ? App.groupCalls.activeRevision : 0
                        return App.groupCalls && root.session.currentIsGroup && revision >= 0
                               ? App.groupCalls.callIn(root.session.currentChatId) : ({})
                    }
                    Layout.fillWidth: true
                    visible: going.callId !== undefined && App.groupCalls !== null && !App.groupCalls.inCall
                    implicitHeight: 48
                    radius: Theme.radiusControl
                    color: Theme.accentSoft
                    border.color: Theme.accentBorder
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 8
                        spacing: 10
                        Icon {
                            name: joinBanner.going.kind === "video" ? "video" : "phone"
                            size: 18
                            color: Theme.accent
                        }
                        Text {
                            Layout.fillWidth: true
                            text: "A group " + (joinBanner.going.kind === "video" ? "video" : "voice") + " call is going on"
                                  + (joinBanner.going.participants > 0 ? " · " + joinBanner.going.participants + " in it" : "")
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.accent
                            font.weight: Font.Medium
                        }
                        RvButton {
                            implicitHeight: 34
                            text: "Join"
                            onClicked: root.planNotice = App.groupCalls.start(root.session.currentChatId, joinBanner.going.kind)
                        }
                    }
                }

                // Members to mention (typing "@" in a group).
                Rectangle {
                    Layout.fillWidth: true
                    visible: root.mentionMatches.length > 0
                    implicitHeight: mentionColumn.implicitHeight + 8
                    radius: Theme.radiusControl
                    color: Theme.surface
                    border.color: Theme.border
                    ColumnLayout {
                        id: mentionColumn
                        anchors.fill: parent
                        anchors.margins: 4
                        spacing: 0
                        Repeater {
                            model: root.mentionMatches
                            delegate: ItemDelegate {
                                id: mentionRow
                                required property var modelData
                                required property int index
                                Layout.fillWidth: true
                                implicitHeight: 40
                                leftPadding: 10
                                onClicked: root.pickMention(mentionRow.index)
                                background: Rectangle {
                                    radius: Theme.radiusSmall
                                    color: mentionRow.index === root.mentionIndex ? Theme.selected : mentionRow.hovered ? Theme.surface2 : "transparent"
                                }
                                contentItem: RowLayout {
                                    spacing: 10
                                    Avatar {
                                        size: 26
                                        initial: mentionRow.modelData.handle.length > 0 ? mentionRow.modelData.handle.charAt(0).toUpperCase() : "?"
                                        source: mentionRow.modelData.avatarUrl
                                        tintKey: mentionRow.modelData.userId
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: "@" + mentionRow.modelData.handle
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        color: Theme.text
                                    }
                                }
                            }
                        }
                    }
                }

                // Editing…
                Rectangle {
                    Layout.fillWidth: true
                    visible: root.editingId.length > 0
                    implicitHeight: 40
                    radius: Theme.radiusControl
                    color: Theme.surface2
                    Icon {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        name: "pen"
                        size: 15
                        color: Theme.accent
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 36
                        anchors.right: cancelEditButton.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Editing message · Enter saves, Esc cancels"
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text2
                        font.pixelSize: Theme.fontSmall + 1
                    }
                    RvButton {
                        id: cancelEditButton
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        primary: false
                        iconName: "x"
                        implicitHeight: 30
                        ToolTip.visible: hovered
                        ToolTip.text: "Cancel editing"
                        onClicked: root.cancelEdit()
                    }
                }

                // Replying to…
                Rectangle {
                    Layout.fillWidth: true
                    visible: root.replyToId.length > 0
                    implicitHeight: 40
                    radius: Theme.radiusControl
                    color: Theme.surface2

                    Rectangle {
                        width: 3
                        height: parent.height - 12
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        radius: 2
                        color: Theme.accent
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 20
                        anchors.right: cancelReply.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Replying to: " + root.replyPreview
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.text2
                        font.pixelSize: Theme.fontSmall + 1
                    }
                    RvButton {
                        id: cancelReply
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        primary: false
                        iconName: "x"
                        implicitHeight: 30
                        ToolTip.visible: hovered
                        ToolTip.text: "Cancel reply"
                        onClicked: {
                            root.replyToId = ""
                            root.replyPreview = ""
                        }
                    }
                }

                // A channel's subscribers read; the creator and staff post.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.bottomMargin: 4
                    visible: root.readOnly
                    spacing: 8
                    Icon {
                        name: "lock"
                        size: 14
                        color: Theme.accent
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: "Only the creator and staff post in this channel."
                        color: Theme.text2
                        font.pixelSize: Theme.fontSmall + 1
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: !root.readOnly
                    spacing: 8

                    IconButton {
                        id: emojiButton
                        Layout.alignment: Qt.AlignBottom
                        implicitHeight: 42
                        iconName: "smile"
                        tip: "Emoji (Ctrl+Shift+J)"
                        function togglePicker() {
                            if (emojiPicker.visible) emojiPicker.close()
                            else emojiPicker.open()
                        }
                        onClicked: togglePicker()

                        EmojiPicker {
                            id: emojiPicker
                            x: 0
                            y: -height - 8
                            onPicked: (emoji) => {
                                composer.insert(composer.cursorPosition, emoji)
                            }
                            onClosed: composer.forceActiveFocus()
                        }
                    }
                    IconButton {
                        Layout.alignment: Qt.AlignBottom
                        implicitHeight: 42
                        iconName: "chart-bar"
                        enabled: !root.v3Mode
                        tip: "Create a poll"
                        onClicked: pollDialog.open()
                    }
                    IconButton {
                        Layout.alignment: Qt.AlignBottom
                        implicitHeight: 42
                        iconName: "paperclip"
                        enabled: root.fileActivity.length === 0
                        tip: root.account.pro ? "Send a file (encrypted on this computer first)"
                                              : "Send a file (Pro)"
                        onClicked: {
                            if (!root.account.pro) {
                                root.planNotice = "Sending files isn't included in your plan. Upgrade to Pro in Settings to send files."
                                return
                            }
                            root.planNotice = ""
                            App.lock.expectExternalReturn()
                            attachDialog.open()
                        }
                    }

                    ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(140, Math.max(40, composer.implicitHeight))

                        TextArea {
                            id: composer
                            placeholderText: "Message"
                            Accessible.name: "Message"
                            onCursorPositionChanged: root.updateMentions()
                            onTextChanged: {
                                root.updateMentions()
                                if (root.restoringDraft) return
                                if (text.length > 0 && App.live) App.live.notifyTyping(root.session.currentChatId)
                                // Saved under the chat this composer belongs to (it can lag the current one while switching).
                                if (root.editingId.length === 0 && messageList.shownChatId.length > 0) root.session.setDraft(messageList.shownChatId, text)
                            }
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
                                border.color: composer.activeFocus ? Theme.accentBorder : Theme.surface2
                            }

                            ToolTip.visible: hovered && composer.length === 0
                            ToolTip.delay: 1200
                            ToolTip.text: "*bold*  _italic_  ~strike~  `code`  ||spoiler||   (Ctrl+B, Ctrl+I, Ctrl+Shift+X, Ctrl+E, Ctrl+Shift+B)"

                            // Enter sends; Shift+Enter starts a new line.
                            Keys.onPressed: (event) => {
                                // Picking a member to mention.
                                if (root.mentionMatches.length > 0) {
                                    if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
                                        event.accepted = true
                                        const n = root.mentionMatches.length
                                        root.mentionIndex = (root.mentionIndex + (event.key === Qt.Key_Down ? 1 : n - 1)) % n
                                        return
                                    }
                                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Tab) {
                                        event.accepted = true
                                        root.pickMention(root.mentionIndex)
                                        return
                                    }
                                    if (event.key === Qt.Key_Escape) {
                                        event.accepted = true
                                        root.clearMentions()
                                        return
                                    }
                                }
                                // Pasting a copied file or picture offers to send it; text pastes as usual.
                                if (event.matches(StandardKey.Paste)) {
                                    const pasted = root.session.pastedAttachments()
                                    if (pasted.length > 0) {
                                        event.accepted = true
                                        root.offerFiles(pasted)
                                        return
                                    }
                                }
                                if (event.modifiers & Qt.ControlModifier) {
                                    const shift = event.modifiers & Qt.ShiftModifier
                                    const marker = event.key === Qt.Key_B ? (shift ? "||" : "*")
                                                 : event.key === Qt.Key_I && !shift ? "_"
                                                 : event.key === Qt.Key_X && shift ? "~"
                                                 : event.key === Qt.Key_E && !shift ? "`" : ""
                                    if (marker.length > 0) {
                                        event.accepted = true
                                        root.wrapSelection(marker)
                                        return
                                    }
                                }
                                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                        && !(event.modifiers & Qt.ShiftModifier)) {
                                    event.accepted = true
                                    root.send()
                                } else if (event.key === Qt.Key_Escape && root.editingId.length > 0) {
                                    event.accepted = true
                                    root.cancelEdit()
                                } else if (event.key === Qt.Key_Escape && root.replyToId.length > 0) {
                                    event.accepted = true
                                    root.replyToId = ""
                                    root.replyPreview = ""
                                } else if (event.key === Qt.Key_Up && composer.length === 0 && root.editingId.length === 0 && !root.v3Mode) {
                                    // Up in an empty composer edits your last message, like Signal.
                                    const last = root.session.lastEditableId()
                                    if (last.length > 0) {
                                        event.accepted = true
                                        root.startEdit(last)
                                    }
                                }
                            }
                        }
                    }

                    IconButton {
                        Layout.alignment: Qt.AlignBottom
                        iconName: "send"
                        tip: root.session.sending ? "Sending…" : root.editingId.length > 0 ? "Save edit" : "Send"
                        round: true
                        implicitWidth: 42
                        implicitHeight: 42
                        toggled: false
                        iconColor: Theme.accentContrast
                        enabled: !root.session.sending && composer.text.trim().length > 0 && !(root.v3Mode && App.deviceKeys.chatSending)
                        onClicked: root.send()
                        background: Rectangle {
                            radius: 21
                            color: Theme.accent
                            opacity: parent.enabled ? 1 : 0.4
                        }
                    }
                }
            }
        }
    }
}
