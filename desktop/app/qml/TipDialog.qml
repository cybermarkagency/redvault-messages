pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A voluntary tip: $1, $3, $5, $10 or another amount. It doesn't unlock
// anything. Paying happens on Stripe's page in the browser; the server says
// when it's through. As the web app's components/earnings/TipDialog.tsx.
RvDialog {
    id: dialog
    required property EarningsSession earnings
    property string creatorUserId: ""
    property string name: ""
    property int amount: 300
    property bool custom: false
    readonly property int cents: custom ? (earnings ? earnings.parseAmount(customField.text) : -1) : amount
    readonly property bool payingHere: earnings !== null && earnings.payContext === "tip"
    readonly property bool done: earnings !== null && earnings.paidProduct === "tip"

    title: "Tip " + name
    width: 460

    function openFor(userId, displayName) {
        creatorUserId = userId
        name = displayName
        amount = 300
        custom = false
        customField.text = ""
        showName.checked = true
        if (earnings) earnings.clearPayment()
        open()
    }

    onClosed: {
        if (earnings && !payingHere) earnings.clearPayment()
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 14

        Text {
            Layout.fillWidth: true
            visible: dialog.done
            wrapMode: Text.Wrap
            text: "Thank you! Your tip went through."
            color: Theme.text
            font.pixelSize: Theme.fontSize
        }

        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            visible: !dialog.done
            wrapMode: Text.Wrap
            text: "Tips are voluntary and don't unlock anything. You pay on Stripe's page in your browser; RedVault never sees your card."
            color: Theme.text2
            font.pixelSize: Theme.fontSmall + 1
        }

        Flow {
            Layout.fillWidth: true
            visible: !dialog.done
            spacing: 6
            Accessible.role: Accessible.RadioButton
            Accessible.name: "Amount"
            Repeater {
                model: dialog.earnings ? dialog.earnings.tipPresets : []
                delegate: Chip {
                    required property var modelData
                    text: modelData.label
                    checked: !dialog.custom && dialog.amount === modelData.minor
                    onClicked: {
                        dialog.custom = false
                        dialog.amount = modelData.minor
                    }
                }
            }
            Chip {
                text: "Other"
                checked: dialog.custom
                onClicked: {
                    dialog.custom = true
                    customField.forceActiveFocus()
                }
            }
        }

        RvTextField {
            id: customField
            Layout.fillWidth: true
            visible: dialog.custom && !dialog.done
            placeholderText: "Amount in US dollars, like 7.50"
            Accessible.name: "Tip amount in US dollars"
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: !dialog.done
            spacing: 4
            ConsentBox {
                id: showName
                Layout.fillWidth: true
                checked: true
                label: "Show " + dialog.name + " who it's from"
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: showName.checked
                    ? "They'll see your username with the tip."
                    : "They'll see “a supporter”. RedVault and Stripe still know who paid (for refunds and fraud checks), so this isn't anonymous."
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
        }

        Text {
            Layout.fillWidth: true
            visible: dialog.payingHere && dialog.earnings.payStatus.length > 0
            wrapMode: Text.Wrap
            text: dialog.earnings ? dialog.earnings.payStatus : ""
            textFormat: Text.PlainText
            color: Theme.text2
            font.pixelSize: Theme.fontSmall
        }
        Banner {
            Layout.fillWidth: true
            text: dialog.earnings && dialog.earnings.payErrorContext === "tip" ? dialog.earnings.payError : ""
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            RvButton {
                visible: !dialog.done
                primary: false
                text: dialog.payingHere ? "Stop waiting" : "Cancel"
                onClicked: {
                    if (dialog.payingHere) dialog.earnings.cancelPayment()
                    else dialog.close()
                }
            }
            RvButton {
                visible: !dialog.done
                text: dialog.cents > 0 ? "Tip " + dialog.earnings.formatAmount(dialog.cents) : "Tip"
                enabled: dialog.earnings !== null && !dialog.earnings.paying && dialog.cents > 0
                onClicked: dialog.earnings.tip(dialog.creatorUserId, dialog.cents, showName.checked)
            }
            RvButton {
                visible: dialog.done
                text: "Done"
                onClicked: dialog.close()
            }
        }
    }
}
