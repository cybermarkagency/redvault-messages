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

        // Tor / call status (docs/tor-routing-plan.md), always visible while it matters.
        Item {
            Layout.fillWidth: true
            implicitHeight: statusChip.visible ? statusChip.implicitHeight + 12 : 0
            visible: statusChip.visible
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
                    onOpenPlans: {
                        settingsView.section = "account"
                        shell.page = "settings"
                    }
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
    // A community's page opened from its chat, or its chat opened from its page.
    Connections {
        target: App.communities
        function onShowRequested() { shell.page = "communities" }
        function onShowChatRequested() { shell.page = "chats" }
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
