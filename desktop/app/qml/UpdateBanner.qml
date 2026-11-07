import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// "redvaultmessages X is available": shown across the top of the window while an
// update is waiting, downloading or installing.
Rectangle {
    id: banner
    readonly property bool active: Updater.status === Updater.Available
                                   || Updater.status === Updater.Downloading
                                   || Updater.status === Updater.Installing

    visible: active
    implicitHeight: active ? row.implicitHeight + 16 : 0
    color: Theme.accentSoft

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 12
        spacing: 10

        Icon {
            name: "download"
            size: 18
            color: Theme.accent
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            spacing: 0
            Text {
                Layout.fillWidth: true
                text: Updater.status === Updater.Downloading ? "Downloading redvaultmessages " + Updater.availableVersion + "…"
                    : Updater.status === Updater.Installing ? "Installing redvaultmessages " + Updater.availableVersion + ". It will reopen in a moment."
                    : "redvaultmessages " + Updater.availableVersion + " is available."
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.text
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: Updater.error.length > 0 ? Updater.error
                    : Updater.status === Updater.Available ? (Updater.notes.length > 0 ? Updater.notes + " " : "")
                                                             + "Updating signs you out; sign back in afterwards."
                                                             + (Updater.freshnessWarning.length > 0 ? " " + Updater.freshnessWarning : "")
                    : ""
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Updater.error.length > 0 ? Theme.danger : Theme.text2
                font.pixelSize: Theme.fontSmall
            }
        }
        BusyIndicator {
            Layout.preferredWidth: 24
            Layout.preferredHeight: 24
            visible: Updater.status !== Updater.Available
            running: visible
        }
        RvButton {
            visible: Updater.status === Updater.Available
            implicitHeight: 32
            iconName: "download"
            text: "Update now"
            onClicked: Updater.install()
        }
    }
}
