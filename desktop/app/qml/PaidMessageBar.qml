import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Above the composer in a 1:1 chat with someone who charges for messages:
// what sending costs, Pay (Stripe's page opens in the browser), and the free
// safety contact. As the web app's components/earnings/PaidMessageBar.tsx.
Rectangle {
    id: bar
    required property EarningsSession earnings
    property string name: ""
    readonly property var quote: earnings ? earnings.quote : ({ required: false })
    readonly property bool payingHere: earnings !== null && earnings.payContext === "paid_message"

    visible: quote.required === true
    implicitHeight: visible ? column.implicitHeight + 20 : 0
    radius: Theme.radiusControl
    color: Theme.accentSoft
    border.color: Theme.accentBorder
    Accessible.role: Accessible.Grouping
    Accessible.name: "Paid messages"

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Icon {
                Layout.alignment: Qt.AlignTop
                name: bar.earnings && bar.earnings.safety ? "shield" : "credit-card"
                size: 18
                color: Theme.accent
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontSmall + 1
                text: {
                    if (bar.quote.paid) return "Paid. Your next message to " + bar.name + " goes through."
                    if (bar.earnings && bar.earnings.safety)
                        return "Safety contact: for something urgent about " + bar.name + "'s safety or account. Free once every 30 days; misuse can be reported."
                    const what = bar.quote.mode === "first_contact"
                        ? bar.name + " charges " + bar.quote.price + " to start a chat (then it's free)."
                        : bar.name + " charges " + bar.quote.price + " for each message."
                    return what + " Messages stay end-to-end encrypted: paying never lets anyone read them."
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !bar.quote.paid
            spacing: 8
            RvButton {
                visible: !(bar.earnings && bar.earnings.safety) && bar.quote.purchasable === true
                text: bar.payingHere ? "Waiting for payment..." : "Pay " + bar.quote.price
                enabled: bar.earnings !== null && !bar.earnings.paying
                onClicked: bar.earnings.payForMessage()
            }
            Text {
                visible: !(bar.earnings && bar.earnings.safety) && bar.quote.purchasable !== true
                text: "Payments aren't available in this app right now."
                color: Theme.text2
                font.pixelSize: Theme.fontSmall
            }
            RvButton {
                visible: bar.payingHere
                secondary: true
                text: "Stop waiting"
                onClicked: bar.earnings.cancelPayment()
            }
            RvButton {
                visible: bar.quote.safetyAvailable === true && !bar.payingHere
                primary: false
                text: bar.earnings && bar.earnings.safety ? "Cancel safety contact" : "Safety contact (free)"
                onClicked: bar.earnings.safety = !bar.earnings.safety
            }
            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth: true
            visible: bar.payingHere && bar.earnings.payStatus.length > 0
            wrapMode: Text.Wrap
            text: bar.earnings ? bar.earnings.payStatus : ""
            textFormat: Text.PlainText
            color: Theme.text2
            font.pixelSize: Theme.fontSmall
        }
        Text {
            Layout.fillWidth: true
            visible: bar.earnings !== null && bar.earnings.payError.length > 0 && bar.earnings.payErrorContext === "paid_message"
            wrapMode: Text.Wrap
            text: bar.earnings ? bar.earnings.payError : ""
            textFormat: Text.PlainText
            color: Theme.danger
            font.pixelSize: Theme.fontSmall
        }
    }
}
