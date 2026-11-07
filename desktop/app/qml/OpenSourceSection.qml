import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Settings > About: the app's licence (AGPL-3.0-only), this version's source link, the full licence and the
// third-party notices (built in, plain text) and where to report a problem getting the source. The link and the
// contact come from the build (REDVAULT_SOURCE_CODE_URL, REDVAULT_SOURCE_CONTACT); with no link set, none is shown.
ColumnLayout {
    id: section
    readonly property var info: App.openSourceInfo()
    readonly property string sourceUrl: info.sourceUrl || ""
    readonly property string contact: info.contact || ""

    spacing: 10

    component Para: Text {
        Layout.fillWidth: true
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.text2
        font.pixelSize: Theme.fontSmall + 1
    }

    Text {
        Layout.fillWidth: true
        Layout.topMargin: 18
        text: "Open-Source Licenses and Source Code"
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.text
        font.pixelSize: Theme.fontSize + 1
        font.weight: Font.DemiBold
    }

    Para {
        text: "RedVault Messages is free software, licensed under the GNU Affero General Public License, version 3 only (AGPL-3.0-only). It comes with no warranty."
    }

    Para {
        text: "Version " + (section.info.version || "")
        color: Theme.text
    }

    // Source code: a link only when the build has one; never a made-up address.
    ColumnLayout {
        Layout.fillWidth: true
        visible: section.sourceUrl.length > 0
        spacing: 4
        RvButton {
            objectName: "sourceCodeLink"
            primary: false
            leftPadding: 0
            iconName: "external-link"
            text: "Source code for this version"
            onClicked: Qt.openUrlExternally(section.sourceUrl)
        }
        Para {
            text: section.sourceUrl
            color: Theme.text3
            wrapMode: Text.WrapAnywhere
        }
    }
    Para {
        visible: section.sourceUrl.length === 0
        text: "The public source code link will appear here once it is published."
    }

    Flow {
        Layout.fillWidth: true
        spacing: 8
        RvButton {
            secondary: true
            text: "View the full license"
            onClicked: textDialog.show("license", "GNU Affero General Public License, version 3")
        }
        RvButton {
            secondary: true
            text: "View third-party notices"
            onClicked: textDialog.show("notices", "Third-party notices")
        }
    }

    Para {
        text: section.contact.length > 0
              ? "To report a problem getting the source code, contact " + section.contact + "."
              : "To report a problem getting the source code, use the contact address in the Terms of Service."
    }
    RvButton {
        visible: section.contact.length === 0
        primary: false
        leftPadding: 0
        text: "View the Terms of Service"
        onClicked: termsDialog.show("terms")
    }

    Para {
        text: "RedVault Messages is not affiliated with or endorsed by Signal Messenger LLC."
        color: Theme.text3
    }

    PlainTextDocumentDialog {
        id: textDialog
    }
    LegalDocumentDialog {
        id: termsDialog
    }
}
