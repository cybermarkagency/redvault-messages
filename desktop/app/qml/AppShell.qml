// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The signed-in app: nav rail, the four pages, and calls over everything.
Item {
    id: shell
    required property ChatSession session
    required property PeopleSession people
    required property AccountSession account
    property string page: "chats"

    function openChat(chatId) {
        if (chatId.length === 0) return
        shell.page = "chats"
        shell.session.openChat(chatId)
    }

    // A chat opened from elsewhere (a notification) shows the Chats page. Only
    // when the open chat changes: refreshes re-announce the same chat.
    property string shownChatId: ""
    Connections {
        target: shell.session
        function onCurrentChatChanged() {
            const id = shell.session.currentChatId
            if (id === shell.shownChatId) return
            shell.shownChatId = id
            if (id.length > 0 && !(App.calls && App.calls.chatOpen))
                shell.page = "chats"
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        CallBar {
            Layout.fillWidth: true
            calls: App.calls
        }

        GroupCallBar {
            Layout.fillWidth: true
            groupCalls: App.groupCalls
        }

        // Devices & encryption: this computer isn't linked to the account's devices (it stays until it is), or,
        // once, right after it was set up as the first device: make a recovery key.
        Rectangle {
            id: deviceKeysBanner
            readonly property bool link: App.deviceKeys !== null && App.deviceKeys.needsLink
            readonly property bool recovery: App.deviceKeys !== null && App.deviceKeys.recoveryNotice
            Layout.fillWidth: true
            visible: (link || recovery) && !(shell.page === "settings" && settingsView.section === "devicekeys")
            implicitHeight: visible ? deviceKeysRow.implicitHeight + 16 : 0
            color: link ? Theme.warnSoft : Theme.accentSoft
            RowLayout {
                id: deviceKeysRow
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 16
                spacing: 10
                Icon {
                    name: deviceKeysBanner.link ? "triangle-alert" : "key"
                    size: 16
                    color: deviceKeysBanner.link ? Theme.warn : Theme.accent
                }
                Text {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    color: deviceKeysBanner.link ? Theme.warn : Theme.accent
                    font.pixelSize: Theme.fontSmall + 1
                    text: deviceKeysBanner.link
                          ? qsTr("Link this computer to your other devices to use the Signal protocol here: pair it with a device you already use, or use your recovery key.")
                          : qsTr("Create a recovery key so you can restore your device keys on a new phone or computer.")
                }
                RvButton {
                    text: deviceKeysBanner.link ? qsTr("Link this computer") : qsTr("Create recovery key")
                    primary: true
                    implicitHeight: 30
                    onClicked: App.deviceKeys.showSettings()
                }
                RvButton {
                    visible: !deviceKeysBanner.link
                    text: qsTr("Not now")
                    implicitHeight: 30
                    onClicked: App.deviceKeys.dismissRecoveryNotice()
                }
            }
        }

        // Tor / call status (docs/tor-routing-plan.md), always visible while it matters.
        // An open chat shows the Tor part in its own header instead, so the strip
        // stays away from the Chats page then, unless a call is on.
        Item {
            readonly property bool chatShowsIt: shell.page === "chats" && shell.session.currentChatId.length > 0 && !statusChip.callActive
            Layout.fillWidth: true
            implicitHeight: visible ? statusChip.implicitHeight + 12 : 0
            visible: statusChip.visible && !chatShowsIt
            TorStatusChip {
                id: statusChip
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                tor: App.tor
                callActive: (App.calls !== null && (App.calls.phase === "active" || App.calls.phase === "reconnecting"))
                            || (App.groupCalls !== null && App.groupCalls.inCall)
                onOpenSettings: {
                    settingsView.section = "network"
                    shell.page = "settings"
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            NavRail {
                Layout.fillHeight: true
                current: shell.page
                chatsBadge: shell.session.unreadTotal
                contactsBadge: shell.people.incoming.length
                inboxBadge: App.inbox ? App.inbox.unread : 0
                channelsBadge: App.channels ? App.channels.unreadChannels : 0
                filesBadge: App.shares ? App.shares.incomingCount : 0
                avatarUrl: shell.account.avatarUrl
                showAdmin: App.superAdmin && App.communityAdmin !== null
                onNavigate: (page) => {
                    shell.page = page
                    if (page === "privacy") settingsView.section = "privacy"
                    if (page === "passwords" && App.devices) App.devices.reload()
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: ["chats", "calls", "contacts", "settings", "files", "passwords", "inbox", "channels", "communities", "admin", "earnings"].indexOf(shell.page === "privacy" ? "settings" : shell.page)

                ChatsView {
                    id: chatsView
                    session: shell.session
                    people: shell.people
                    account: shell.account
                    onOpenNetworkSettings: {
                        settingsView.section = "network"
                        shell.page = "settings"
                    }
                }
                CallsView {
                    calls: App.calls
                    chats: shell.session
                    account: shell.account
                    onOpenChat: (chatId) => shell.openChat(chatId)
                }
                ContactsView {
                    chats: shell.session
                    people: shell.people
                    onOpenChat: (chatId) => shell.openChat(chatId)
                    onVerify: (chatId) => {
                        shell.openChat(chatId)
                        Qt.callLater(chatsView.openVerify)
                    }
                    onOpenFriends: {
                        shell.page = "chats"
                        chatsView.openFriends()
                    }
                }
                SettingsView {
                    id: settingsView
                    account: shell.account
                    people: shell.people
                }
                FilesView {
                    files: App.files
                    chats: shell.session
                }
                PasswordsView {
                    vault: App.vault
                    chats: shell.session
                }
                InboxView {
                    inbox: App.inbox
                    active: shell.page === "inbox"
                }
                ChannelsView {
                    channels: App.channels
                    active: shell.page === "channels"
                    onOpenPlans: PlanGate.openPlans()
                }
                CommunitiesView {
                    communities: App.communities
                    active: shell.page === "communities"
                }
                // RedVault's super admins only (the server refuses everyone else as well).
                Loader {
                    active: App.communityAdmin !== null
                    sourceComponent: AdminView {
                        admin: App.communityAdmin
                        active: shell.page === "admin"
                    }
                }
                EarningsView {
                    earnings: App.earnings
                    active: shell.page === "earnings"
                }
            }
        }
    }

    // A channel link clicked in a chat opens the channel here.
    Connections {
        target: App.channels
        function onShowRequested() { shell.page = "channels" }
    }
    // Settings > Devices & encryption, asked for by the banner above or a chat that needs this computer linked.
    Connections {
        target: App.deviceKeys
        function onShowSettingsRequested() {
            settingsView.section = "devicekeys"
            shell.page = "settings"
        }
    }
    // A community's page opened from its chat, or its chat opened from its page.
    Connections {
        target: App.communities
        function onShowRequested() { if (App.communitiesEnabled) shell.page = "communities" }
        function onShowChatRequested() { shell.page = "chats" }
    }

    // A paid feature tapped on a plan without it (PlanGate.ask), and "See plans" anywhere.
    UpgradeDialog {
        id: upgradeDialog
        onSeePlans: PlanGate.openPlans()
    }
    Connections {
        target: PlanGate
        function onUpgradeRequested(tier, feature) { upgradeDialog.show(tier, feature) }
        function onPlansRequested() {
            settingsView.section = "account"
            shell.page = "settings"
            // From inside a call: shrink it to its bar so the plans are visible.
            if (App.calls !== null && App.calls.phase.length > 0) App.calls.setMinimized(true)
            if (App.groupCalls !== null && App.groupCalls.present) App.groupCalls.setMinimized(true)
        }
    }

    // A group invite link, pasted or clicked in a chat.
    JoinGroupDialog {
        invites: App.groupInvites
    }

    StoryViewer {
        anchors.fill: parent
        stories: App.stories
        onOpenSettings: {
            settingsView.section = "stories"
            shell.page = "settings"
        }
    }
    StoryComposer {
        anchors.fill: parent
        stories: App.stories
        onOpenSettings: {
            settingsView.section = "stories"
            shell.page = "settings"
        }
    }

    CallOverlay {
        anchors.fill: parent
        calls: App.calls
        chats: shell.session
        onOpenChat: (chatId) => shell.openChat(chatId)
    }

    GroupCallOverlay {
        anchors.fill: parent
        groupCalls: App.groupCalls
        chats: shell.session
        onOpenChat: (chatId) => shell.openChat(chatId)
    }
}
