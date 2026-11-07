pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Settings › Relay servers: the message relay and the call relays (TURN),
// with a reachability check.
ColumnLayout {
    id: root
    required property RelayCheck relays
    spacing: 12

    function toneColor(tone) {
        return tone === "ok" ? Theme.accent : tone === "warn" ? Theme.warn : Theme.text3
    }

    component Caption: Text {
        Layout.topMargin: 8
        color: Theme.text3
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
    }

    Text {
        text: "Relay servers"
        color: Theme.text
        font.family: Theme.displayFamily
        font.pixelSize: 26
        font.weight: Font.Bold
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: "Messages go through the message relay, and calls always go through a call relay, so nobody you talk to ever learns your IP address. Relays only ever carry ciphertext: they can't read messages or listen to calls."
        color: Theme.text2
        font.pixelSize: 14
    }
    RvButton {
        secondary: true
        iconName: "refresh"
        text: "Check connections"
        enabled: root.relays !== null
        onClicked: root.relays.checkAll()
    }

    Caption { text: "MESSAGE RELAY" }
    Card {
        Layout.fillWidth: true
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Icon {
                name: "globe"
                size: 20
                color: Theme.text2
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                spacing: 2
                Text {
                    text: root.relays ? root.relays.apiHost : ""
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.weight: Font.Medium
                }
                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: root.relays ? root.relays.apiBase : ""
                    textFormat: Text.PlainText
                    color: Theme.text3
                    font.family: Theme.monoFamily
                    font.pixelSize: 12
                }
            }
            Text {
                text: root.relays ? root.relays.apiStatus : ""
                textFormat: Text.PlainText
                color: root.toneColor(root.relays ? root.relays.apiTone : "")
                font.pixelSize: 13
            }
        }
    }

    Caption { text: "CALL RELAYS (TURN)" }
    Card {
        Layout.fillWidth: true
        spacing: 0
        Text {
            visible: root.relays !== null && root.relays.relayError.length > 0
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.relays ? root.relays.relayError : ""
            textFormat: Text.PlainText
            color: Theme.text3
        }
        Repeater {
            model: root.relays ? root.relays.relays : []
            delegate: ColumnLayout {
                id: relay
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: 0
                Rectangle {
                    visible: relay.index > 0
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    Layout.bottomMargin: 6
                    implicitHeight: 1
                    color: Theme.border
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumHeight: 44
                    spacing: 12
                    Icon {
                        name: "phone"
                        size: 20
                        color: Theme.text2
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 0
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: relay.modelData.host
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.family: Theme.monoFamily
                            font.pixelSize: 13
                        }
                        Text {
                            text: relay.modelData.transport + " · relay only"
                            textFormat: Text.PlainText
                            color: Theme.text3
                            font.pixelSize: 12
                        }
                    }
                    Text {
                        text: relay.modelData.status
                        textFormat: Text.PlainText
                        color: root.toneColor(relay.modelData.tone)
                        font.pixelSize: 13
                    }
                }
            }
        }
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: "This app relays calls over UDP. TCP and TLS relays are there for browsers on networks that block UDP."
        color: Theme.text3
        font.pixelSize: 12
    }
}
