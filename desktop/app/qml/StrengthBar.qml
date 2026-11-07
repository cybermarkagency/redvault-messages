import QtQuick
import QtQuick.Layouts

// The design's strength bar: Strong (accent, full), Fair (warning, 60%),
// Weak (danger text, 28%), labelled with the character count.
RowLayout {
    id: bar
    /// {label, fraction, tone, length} from VaultSession.strengthOf.
    property var strength: ({})
    readonly property color tone: strength.tone === "good" ? Theme.accent : strength.tone === "warn" ? Theme.warn : Theme.danger
    spacing: 10

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 5
        radius: 3
        color: Qt.alpha(Theme.text, 0.12)
        Rectangle {
            width: parent.width * (bar.strength.fraction || 0)
            height: parent.height
            radius: 3
            color: bar.tone
        }
    }
    Text {
        text: (bar.strength.label || "") + " · " + (bar.strength.length || 0) + " characters"
        textFormat: Text.PlainText
        color: bar.tone
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }
}
