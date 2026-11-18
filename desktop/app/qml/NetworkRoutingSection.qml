// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Settings › Privacy & Security › Network routing (docs/tor-by-default.md):
// how each kind of traffic leaves this computer. Read-only on purpose: there
// is no switch, and nothing but calls ever leaves outside Tor.
ColumnLayout {
    id: root
    /// The app's Tor gateway (App.tor).
    property TorController tor: null
    spacing: 12

    readonly property bool torOn: root.tor !== null && root.tor.enabled

    component Caption: Text {
        Layout.topMargin: 8
        color: Theme.text3
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
    }

    component PolicyRow: RowLayout {
        id: row
        required property string feature
        required property string transport
        required property bool viaTor
        Layout.fillWidth: true
        spacing: 12
        Icon {
            name: row.viaTor ? "shield-check" : "phone"
            size: 18
            color: row.viaTor ? Theme.accent : Theme.text2
        }
        Text {
            Layout.fillWidth: true
            text: row.feature
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: 14
        }
        Text {
            text: row.transport
            textFormat: Text.PlainText
            color: row.viaTor ? Theme.accent : Theme.text2
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }
    }

    Text {
        text: qsTr("Network routing")
        color: Theme.text
        font.family: Theme.displayFamily
        font.pixelSize: 26
        font.weight: Font.Bold
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: root.torOn
              ? qsTr("Everything this app sends, except calls, travels through Tor to RedVault's onion service. If Tor can't connect, messages wait, encrypted on this computer, until it's back: there is no direct connection to fall back to.")
              : qsTr("This copy was built for testing and isn't connected through Tor: it talks to its server directly. Messages are still end-to-end encrypted.")
        textFormat: Text.PlainText
        color: Theme.text2
        font.pixelSize: 14
    }

    Caption { text: qsTr("ROUTING") }
    Card {
        Layout.fillWidth: true
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10
            PolicyRow {
                feature: "Messages, files and account"
                transport: root.torOn ? "through Tor" : "direct (test copy)"
                viaTor: root.torOn
            }
            PolicyRow { feature: "Calls"; transport: "encrypted relay (not Tor)"; viaTor: false }
        }
    }

    Caption { text: qsTr("TOR") }
    Card {
        Layout.fillWidth: true
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Rectangle {
                width: 10
                height: 10
                radius: 5
                color: !root.torOn ? Theme.text3
                       : root.tor.state === "connected" ? Theme.ok
                       : root.tor.state === "connecting" ? Theme.warn : Theme.danger
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    text: !root.torOn ? qsTr("Not in use")
                          : root.tor.state === "connected" && root.tor.backupRoute ? qsTr("Connected through Tor (backup route)")
                          : root.tor.state === "connected" ? qsTr("Connected through Tor")
                          : root.tor.state === "connecting" ? qsTr("Connecting through Tor… ") + root.tor.progress + "%"
                          : qsTr("Can't reach Tor")
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.torOn && text.length > 0
                    text: root.torOn ? root.tor.detail : ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.text3
                    font.pixelSize: 12
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.torOn
                    text: root.torOn ? qsTr("Onion service: ") + root.tor.onionHost : ""
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: Theme.text3
                    font.pixelSize: 12
                }
            }
        }
    }
    // Bridges (tor-core/src/bridges.rs): a hidden way into Tor for networks that block it. Still Tor.
    Caption { visible: root.torOn; text: qsTr("BRIDGES") }
    Card {
        id: bridgesCard
        Layout.fillWidth: true
        visible: root.torOn
        readonly property var modes: [
            { value: "off", label: qsTr("Off: connect to Tor directly") },
            { value: "snowflake", label: qsTr("Snowflake (looks like a video call)") },
            { value: "obfs4", label: qsTr("obfs4 (built-in bridges)") },
            { value: "custom", label: qsTr("Bridges I enter myself") }
        ]
        property string pickedMode: root.torOn ? root.tor.bridgeMode : "off"
        property string problem: ""
        property bool saved: false
        Connections {
            target: root.tor
            function onBridgesChanged() { bridgesCard.pickedMode = root.tor.bridgeMode }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("If Tor is blocked where you are, bridges are hidden entrances to it, disguised as ordinary traffic. Your messages still travel through Tor; only the way in changes. Try Snowflake first: it works in most places that block Tor.")
                color: Theme.text2
                font.pixelSize: 13
            }
            Text {
                Layout.fillWidth: true
                visible: root.torOn && !root.tor.bridgesAvailable
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("This copy of the app doesn't include bridge support.")
                color: Theme.warn
                font.pixelSize: 13
            }
            RvChoice {
                id: modeChoice
                objectName: "bridgeMode"
                Layout.fillWidth: true
                enabled: root.torOn && root.tor.bridgesAvailable
                model: bridgesCard.modes
                textRole: "label"
                currentIndex: Math.max(0, bridgesCard.modes.findIndex(m => m.value === bridgesCard.pickedMode))
                onActivated: (index) => {
                    bridgesCard.pickedMode = bridgesCard.modes[index].value
                    bridgesCard.problem = ""
                    bridgesCard.saved = false
                }
                Accessible.name: qsTr("How to connect to Tor")
            }
            Text {
                Layout.fillWidth: true
                visible: bridgesCard.pickedMode === "custom"
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("Paste bridge lines, one per line. Get them from bridges.torproject.org, by emailing bridges@torproject.org, or from someone you trust. obfs4, WebTunnel and Snowflake bridges work.")
                color: Theme.text2
                font.pixelSize: 12
            }
            TextArea {
                id: bridgeLinesField
                objectName: "bridgeLines"
                Layout.fillWidth: true
                Layout.preferredHeight: 110
                visible: bridgesCard.pickedMode === "custom"
                wrapMode: TextEdit.WrapAnywhere
                text: root.torOn ? root.tor.bridgeLines : ""
                color: Theme.text
                font.family: "monospace"
                font.pixelSize: 12
                placeholderText: "obfs4 192.0.2.1:443 FINGERPRINT cert=... iat-mode=0"
                placeholderTextColor: Theme.text3
                background: Rectangle { radius: Theme.radiusSmall; color: Theme.surface2 }
                onTextChanged: bridgesCard.saved = false
                Accessible.name: qsTr("Bridge lines")
            }
            Text {
                Layout.fillWidth: true
                visible: bridgesCard.problem.length > 0
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: bridgesCard.problem
                color: Theme.danger
                font.pixelSize: 13
            }
            Text {
                Layout.fillWidth: true
                visible: bridgesCard.saved && root.tor.state !== "connected"
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("Saved. Tor is reconnecting; messages wait until it's back.")
                color: Theme.text2
                font.pixelSize: 13
            }
            RvButton {
                objectName: "saveBridges"
                text: qsTr("Save and reconnect")
                enabled: root.torOn && (root.tor.bridgesAvailable || bridgesCard.pickedMode === "off")
                         && (bridgesCard.pickedMode !== root.tor.bridgeMode
                             || (bridgesCard.pickedMode === "custom" && bridgeLinesField.text.trim() !== root.tor.bridgeLines))
                onClicked: {
                    const problem = root.tor.setBridges(bridgesCard.pickedMode, bridgeLinesField.text)
                    bridgesCard.problem = problem
                    bridgesCard.saved = problem.length === 0
                }
            }
        }
    }
    // The sealed backup route (docs/tor-sealed-fallback.md): only in builds that carry the gateway's key.
    Caption { visible: root.torOn && root.tor.backupRouteAvailable; text: qsTr("BACKUP ROUTE") }
    Card {
        Layout.fillWidth: true
        visible: root.torOn && root.tor.backupRouteAvailable
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10
            RvSwitch {
                Layout.fillWidth: true
                label: qsTr("Only use the onion address")
                description: qsTr("When the onion address can't be reached, the app normally switches to a backup route: still through Tor, to the website, with every request sealed so only RedVault's server can read it. Cloudflare, which runs in front of the website, can then see that someone uses the backup route, when and how much, but not who or what. Turn this on to wait for the onion address instead.")
                checked: root.torOn && root.tor.onionOnly
                onToggled: (on) => root.tor.onionOnly = on
            }
            Text {
                Layout.fillWidth: true
                visible: root.torOn && root.tor.backupRoute
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("In use now: the onion address isn't answering. The app checks it every minute and moves back as soon as it does.")
                color: Theme.text2
                font.pixelSize: 12
            }
        }
    }

    Banner {
        Layout.fillWidth: true
        error: true
        text: root.torOn && root.tor.state === "unavailable"
              ? qsTr("Tor connection unavailable. Your private messages and media will remain queued until the secure connection is restored.")
                + (root.tor.bridgeMode === "off" && root.tor.bridgesAvailable
                   ? qsTr(" If Tor is blocked on this network, try Snowflake under Bridges above.") : "")
              : ""
    }
    Text {
        Layout.fillWidth: true
        visible: root.torOn && (root.tor.queued > 0)
        text: root.torOn ? root.tor.queued + (root.tor.queued === 1 ? qsTr(" message waiting") : qsTr(" messages waiting"))
                           + (root.tor.pausedUploads > 0 ? " (" + root.tor.pausedUploads + (root.tor.pausedUploads === 1 ? qsTr(" upload paused)") : qsTr(" uploads paused)")) : "")
                         : ""
        textFormat: Text.PlainText
        color: Theme.text2
        font.pixelSize: 13
    }

    Caption { text: qsTr("IN DETAIL") }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: qsTr("Through Tor: messages (including device keys), photos, files and voice messages, Files backups and share links, Stories, profile pictures, your account and sign-in, contacts, groups, channels and communities, the live connection, and update checks.\n")
              + qsTr("Not through Tor: the sound and picture of voice and video calls, one-to-one and in groups. They go through an encrypted relay (TURN), never directly to the other person. Setting up a call still goes through Tor.")
        color: Theme.text2
        font.pixelSize: 13
    }

    Caption { text: qsTr("WHAT OTHERS CAN STILL SEE") }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: qsTr("• RedVault's server never receives your IP address for messages and media, but it still sees your account, who you talk to, when, and how big the (encrypted) messages are.\n")
              + qsTr("• Your internet provider can see that you use Tor (not what for).\n")
              + qsTr("• Calls are not anonymous: they go through a relay (TURN) that sees your IP address and the call's timing, never its content. A call doesn't start without a relay; it never falls back to a direct connection.\n")
              + qsTr("• No system makes you completely anonymous.")
        color: Theme.text2
        font.pixelSize: 13
    }
}
