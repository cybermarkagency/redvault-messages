pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Settings › Privacy & Security › Network routing (docs/tor-routing-plan.md):
// how each kind of traffic leaves this computer. Read-only on purpose: the
// Tor-only features have no direct (HTTPS) switch.
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
        text: "Network routing"
        color: Theme.text
        font.family: Theme.displayFamily
        font.pixelSize: 26
        font.weight: Font.Bold
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: root.torOn
              ? "Private messages and media only ever travel through Tor, to Redvault's onion service. If Tor is unavailable they wait, encrypted on this computer, until it's back: there is no direct connection to fall back to. Calls use WebRTC through a relay (TURN) instead."
              : "Tor routing isn't active in this version of the app: messages and media go to Redvault's server directly, over HTTPS. They are still end-to-end encrypted."
        textFormat: Text.PlainText
        color: Theme.text2
        font.pixelSize: 14
    }

    Caption { text: "TOR" }
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
                       : root.tor.state === "connected" ? Theme.accent
                       : root.tor.state === "connecting" ? Theme.warn : Theme.danger
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    text: !root.torOn ? "Not in use"
                          : root.tor.state === "connected" ? "Tor connected"
                          : root.tor.state === "connecting" ? "Tor connecting · " + root.tor.progress + "%"
                          : "Tor unavailable"
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
                    text: root.torOn ? "Onion service: " + root.tor.onionHost : ""
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: Theme.text3
                    font.pixelSize: 12
                }
            }
        }
    }
    Banner {
        Layout.fillWidth: true
        error: true
        text: root.torOn && root.tor.state === "unavailable"
              ? "Tor connection unavailable. Your private messages and media will remain queued until the secure connection is restored."
              : ""
    }
    Text {
        Layout.fillWidth: true
        visible: root.torOn && (root.tor.queued > 0)
        text: root.torOn ? root.tor.queued + (root.tor.queued === 1 ? " message waiting" : " messages waiting")
                           + (root.tor.pausedUploads > 0 ? " (" + root.tor.pausedUploads + (root.tor.pausedUploads === 1 ? " upload paused)" : " uploads paused)") : "")
                         : ""
        textFormat: Text.PlainText
        color: Theme.text2
        font.pixelSize: 13
    }

    Caption { text: root.torOn ? "HOW EACH KIND OF TRAFFIC TRAVELS" : "PLANNED ROUTING (NOT ACTIVE IN THIS VERSION)" }
    Card {
        Layout.fillWidth: true
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10
            PolicyRow { feature: "Text messages"; transport: "Tor only"; viaTor: true }
            PolicyRow { feature: "Encrypted photos"; transport: "Tor only"; viaTor: true }
            PolicyRow { feature: "Encrypted files"; transport: "Tor only"; viaTor: true }
            PolicyRow { feature: "Private Stories"; transport: "Tor only"; viaTor: true }
            PolicyRow { feature: "Voice messages"; transport: "Tor only"; viaTor: true }
            PolicyRow { feature: "Voice calls"; transport: "WebRTC + TURN"; viaTor: false }
            PolicyRow { feature: "Video calls"; transport: "WebRTC + TURN"; viaTor: false }
        }
    }

    Caption { text: "WHAT OTHERS CAN STILL SEE" }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: "• Redvault's server never receives your IP address for messages and media, but it still sees your account, who you talk to, when, and how big the (encrypted) messages are.\n"
              + "• Your internet provider can see that you use Tor (not what for).\n"
              + "• Calls are not anonymous: they go through a relay (TURN) that sees your IP address and the call's timing, never its content. A call doesn't start without a relay; it never falls back to a direct connection.\n"
              + "• No system makes you completely anonymous."
        color: Theme.text2
        font.pixelSize: 13
    }
}
