import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A recovery key shown exactly once: copy it, confirm it's saved, done.
ColumnLayout {
    id: panel
    required property string recoveryKey
    signal finished()

    property bool copied: false
    spacing: 14

    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: "If you ever forget your password, this key is the only way back into your account and messages. "
              + "Write it down or keep it in a password manager. It won't be shown again, and nobody (including us) can recover it for you."
        color: Theme.text2
        font.pixelSize: Theme.fontSmall + 1
    }

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: keyText.implicitHeight + 28
        radius: Theme.radiusCard
        color: Theme.bg
        border.color: Theme.border

        TextEdit {
            id: keyText
            anchors.centerIn: parent
            width: parent.width - 28
            horizontalAlignment: TextEdit.AlignHCenter
            readOnly: true
            selectByMouse: true
            textFormat: TextEdit.PlainText
            // 13 groups of 4, shown 4 groups to a line.
            text: {
                const groups = panel.recoveryKey.split("-")
                let lines = []
                for (let i = 0; i < groups.length; i += 4)
                    lines.push(groups.slice(i, i + 4).join("-"))
                return lines.join("\n")
            }
            color: Theme.text
            font.family: "Consolas"
            font.pixelSize: 18
            font.letterSpacing: 1
        }
    }

    RowLayout {
        Layout.fillWidth: true
        RvButton {
            primary: false
            iconName: panel.copied ? "check" : "copy"
            text: panel.copied ? "Copied" : "Copy"
            onClicked: {
                // Kept out of Windows clipboard history and cloud sync.
                App.copySecret(panel.recoveryKey)
                panel.copied = true
            }
        }
        Item { Layout.fillWidth: true }
    }

    CheckBox {
        id: saved
        Layout.fillWidth: true
        text: "I've saved my recovery key somewhere safe"
        contentItem: Text {
            textFormat: Text.PlainText
            leftPadding: saved.indicator.width + 8
            text: saved.text
            color: Theme.text
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    RvButton {
        Layout.fillWidth: true
        text: "Done"
        enabled: saved.checked
        onClicked: panel.finished()
    }
}
