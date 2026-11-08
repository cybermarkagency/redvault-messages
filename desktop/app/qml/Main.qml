// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import Redvault

ApplicationWindow {
    id: window
    width: 1360
    height: 860
    minimumWidth: 980
    minimumHeight: 620
    visible: true
    // Qt appends the application display name ("redvaultmessages") itself.
    title: App.state === App.SignedIn ? App.handle : ""
    color: Theme.bg
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize

    // For any Basic-style control not restyled by hand.
    palette.window: Theme.bg
    palette.windowText: Theme.text
    palette.base: Theme.surface
    palette.text: Theme.text
    palette.button: Theme.surface2
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.accentContrast
    palette.placeholderText: Theme.placeholder
    palette.mid: Theme.border
    palette.toolTipBase: Theme.surface3
    palette.toolTipText: Theme.text

    readonly property bool signedIn: App.state === App.SignedIn && App.chats !== null
    // A chat to open once the app is unlocked (from a notification clicked while locked).
    property string pendingChatId: ""

    Component.onCompleted: Shell.attachWindow(window)

    // Never over the lock screen: nothing about the app shows while it's locked.
    header: UpdateBanner {
        visible: active && !(window.signedIn && App.lock.locked)
    }

    // Closing keeps a signed-in app running in the tray (notifications keep
    // coming); otherwise, or with that turned off, closing quits.
    onClosing: (close) => {
        if (window.signedIn && Shell.trayAvailable && Shell.closeToTray) {
            close.accepted = false
            Shell.hideToTray()
        } else {
            close.accepted = false
            App.quit()
        }
    }

    Shortcut {
        sequence: "Ctrl+L"
        enabled: window.signedIn && App.lock.enabled
        onActivated: App.lock.lockNow()
    }

    Connections {
        target: Shell
        // A notification for a followed channel's new post was clicked.
        function onOpenChannelRequested(channelId) {
            if (window.signedIn && App.channels) App.channels.showChannel(channelId)
        }
        function onOpenChatRequested(chatId) {
            if (!window.signedIn)
                return
            if (App.lock.locked)
                window.pendingChatId = chatId
            else
                App.chats.openChat(chatId)
        }
    }

    Connections {
        target: App.lock
        function onLockedChanged() {
            if (!App.lock.locked && window.pendingChatId.length > 0 && window.signedIn) {
                App.chats.openChat(window.pendingChatId)
                window.pendingChatId = ""
            }
        }
    }

    // While locked, the chats aren't loaded at all: nothing renders
    // underneath, and any open dialog goes away with them.
    Loader {
        anchors.fill: parent
        sourceComponent: window.signedIn ? (App.lock.locked ? lockView : chatsView)
                       : App.state === App.ShowRecoveryKey ? recoveryKeyView
                       : loginView
    }

    Component {
        id: loginView
        LoginView {}
    }

    Component {
        id: recoveryKeyView
        RecoveryKeyView {}
    }

    Component {
        id: lockView
        LockView {}
    }

    Component {
        id: chatsView
        AppShell {
            session: App.chats
            people: App.people
            account: App.account
        }
    }
}
