import QtQuick

// A small status label: "accent" (good), "warn", "danger" or "plain".
Rectangle {
    id: pill
    property string text
    property string tone: "plain"

    implicitWidth: label.implicitWidth + 20
    implicitHeight: 24
    radius: 12
    color: tone === "accent" ? Theme.accentSoft : tone === "warn" ? Theme.warnSoft : tone === "danger" ? Theme.dangerSoft : Theme.surface2
    border.color: tone === "accent" ? Theme.accentBorder : tone === "warn" ? "#4A3F28" : tone === "danger" ? "#5A2D28" : Theme.border
    Accessible.role: Accessible.StaticText
    Accessible.name: text

    Text {
        id: label
        anchors.centerIn: parent
        text: pill.text
        textFormat: Text.PlainText
        color: pill.tone === "accent" ? Theme.accent : pill.tone === "warn" ? Theme.warn : pill.tone === "danger" ? Theme.danger : Theme.text2
        font.pixelSize: 12
        font.weight: Font.Medium
    }
}
