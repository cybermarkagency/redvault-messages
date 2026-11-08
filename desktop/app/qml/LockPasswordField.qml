// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// A password for a key or a locked file: a suggestion from the generator
// (24 characters or 6 words, regenerated on demand), the strength bar, and
// the weak-password error. `problem` is empty when the password can be used.
ColumnLayout {
    id: field
    required property FilesSession files
    property string label: "Key password"
    property bool words: false
    property alias text: input.text
    readonly property string problem: files.passwordProblem(input.text)
    readonly property bool acceptable: problem.length === 0

    function generate() { input.text = files.suggestPassword(words) }

    spacing: 8
    Component.onCompleted: if (input.text.length === 0) generate()

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Text {
            Layout.fillWidth: true
            text: field.label
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
        // 24 characters | 6 words
        Rectangle {
            implicitWidth: kinds.implicitWidth + 8
            implicitHeight: 32
            radius: Theme.radiusSmall
            color: Theme.surface2
            Row {
                id: kinds
                anchors.centerIn: parent
                spacing: 2
                Repeater {
                    model: [{ words: false, text: "24 characters" }, { words: true, text: "6 words" }]
                    delegate: AbstractButton {
                        id: kind
                        required property var modelData
                        implicitWidth: kindText.implicitWidth + 20
                        implicitHeight: 26
                        checked: field.words === modelData.words
                        onClicked: {
                            field.words = modelData.words
                            field.generate()
                        }
                        Accessible.name: modelData.text
                        background: Rectangle { radius: 8; color: kind.checked ? Theme.surface3 : "transparent" }
                        contentItem: Text {
                            textFormat: Text.PlainText
                            id: kindText
                            text: kind.modelData.text
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            color: kind.checked ? Theme.text : Theme.text2
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                    }
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        RvTextField {
            id: input
            Layout.fillWidth: true
            implicitHeight: 44
            font.family: Theme.monoFamily
            font.pixelSize: 13
            Accessible.name: field.label
        }
        IconButton {
            filled: true
            implicitWidth: 44
            implicitHeight: 44
            iconName: "refresh"
            tip: "Generate another"
            onClicked: field.generate()
        }
    }

    StrengthBar {
        Layout.fillWidth: true
        visible: input.text.length > 0
        strength: field.files.strengthOf(input.text)
    }
    Text {
        Layout.fillWidth: true
        visible: input.text.length > 0 && !field.acceptable
        wrapMode: Text.Wrap
        text: field.problem
        textFormat: Text.PlainText
        color: Theme.danger
        font.pixelSize: 13
    }
}
