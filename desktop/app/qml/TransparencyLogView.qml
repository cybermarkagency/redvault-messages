pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// Settings › Transparency log. Everything here is checked on this computer:
// the checkpoint's signature (under the log key pinned the first time), that
// the log only grew since last time, an inclusion proof for every listed
// entry, and that your own messages are in it.
Item {
    id: root
    required property TransparencyLog log
    property bool active: false
    property bool copied: false
    property bool showRaw: false

    onActiveChanged: if (log) log.activate(active)

    readonly property var cp: log ? log.checkpoint : ({})
    readonly property bool hasCheckpoint: cp.treeSize !== undefined
    readonly property bool keyChanged: cp.keyStatus === "changed"
    readonly property bool forked: cp.consistent === 0

    FileDialog {
        id: exportDialog
        title: "Export the log"
        fileMode: FileDialog.SaveFile
        nameFilters: ["JSON (*.json)"]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.log.exportTo(selectedFile)
    }

    component Stat: Rectangle {
        id: stat
        property string label
        property string value
        property string detail
        property bool mono: false
        property color tone: Theme.text
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredWidth: 0
        implicitHeight: statColumn.implicitHeight + 32
        radius: Theme.radiusCard
        color: Theme.surface
        border.color: Theme.borderStrong
        ColumnLayout {
            id: statColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            spacing: 6
            Text {
                text: stat.label
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: 12
            }
            Text {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: stat.value
                textFormat: Text.PlainText
                color: stat.tone
                font.family: stat.mono ? Theme.monoFamily : Theme.displayFamily
                font.pixelSize: stat.mono ? 15 : 26
                font.weight: stat.mono ? Font.Medium : Font.Bold
            }
            Text {
                Layout.fillWidth: true
                visible: stat.detail.length > 0
                wrapMode: Text.Wrap
                text: stat.detail
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: 12
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // --- Main ---
        Flickable {
            id: main
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: mainColumn.implicitHeight + 64
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: mainColumn
                x: 32
                y: 32
                width: main.width - 64
                spacing: 16

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: "Transparency log"
                        color: Theme.text
                        font.family: Theme.displayFamily
                        font.pixelSize: 26
                        font.weight: Font.Bold
                    }
                    AbstractButton {
                        id: liveToggle
                        visible: root.log !== null && !root.log.disabled
                        implicitHeight: 32
                        implicitWidth: liveRow.implicitWidth + 24
                        hoverEnabled: true
                        Accessible.name: root.log && root.log.live ? "Pause live updates" : "Resume live updates"
                        onClicked: root.log.live = !root.log.live
                        background: Rectangle {
                            radius: 16
                            color: liveToggle.hovered ? Theme.surface3 : Theme.surface2
                        }
                        contentItem: RowLayout {
                            id: liveRow
                            spacing: 8
                            Rectangle {
                                Layout.leftMargin: 12
                                implicitWidth: 8
                                implicitHeight: 8
                                radius: 4
                                color: root.log && root.log.live ? Theme.accent : Theme.warn
                            }
                            Text {
                                Layout.rightMargin: 12
                                text: root.log && root.log.live ? "Live · pause" : "Paused · resume"
                                textFormat: Text.PlainText
                                color: Theme.text2
                                font.pixelSize: 13
                            }
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.log && root.log.disabled
                          ? "The verifiable transparency log isn't set up on this server yet."
                          : "A public, append-only record of every message this network relays. Each entry is a commitment to a sealed message, logged in signed batches, so anyone can check nothing was altered or removed on the way through."
                    textFormat: Text.PlainText
                    color: Theme.text2
                    font.pixelSize: 14
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.log !== null && !root.log.disabled
                    spacing: 16

                    RowLayout {
                        spacing: 8
                        Icon {
                            name: "shield"
                            size: 16
                            color: Theme.text3
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: "The log never contains message content, senders or recipients. Only this computer can tell which entries are yours."
                            color: Theme.text3
                            font.pixelSize: 13
                        }
                    }

                    // Checks that failed.
                    Rectangle {
                        Layout.fillWidth: true
                        visible: root.log.error.length > 0 || root.keyChanged || root.forked
                        implicitHeight: alertColumn.implicitHeight + 24
                        radius: Theme.radiusControl
                        color: Theme.dangerSoft
                        ColumnLayout {
                            id: alertColumn
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: root.log.error.length > 0 ? root.log.error
                                    : root.keyChanged ? "The log's signing key changed since this computer first saw it. Only accept the new key if the operator has announced it."
                                    : "This checkpoint doesn't extend the one this computer verified before: the log may have been rewritten. Treat it as untrusted."
                                textFormat: Text.PlainText
                                color: Theme.danger
                                font.pixelSize: 13
                            }
                            RvButton {
                                visible: root.keyChanged
                                primary: false
                                danger: true
                                text: "Trust new key " + (root.cp.keyFingerprint || "")
                                onClicked: root.log.acceptNewKey()
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Stat {
                            label: "Entries logged"
                            value: root.hasCheckpoint ? Number(root.cp.treeSize).toLocaleString(Qt.locale(), "f", 0) : "0"
                        }
                        Stat {
                            label: "Latest signed checkpoint"
                            value: root.hasCheckpoint ? root.cp.rootShort : "none yet"
                            mono: true
                            detail: !root.hasCheckpoint ? ""
                                  : (root.cp.consistent === -1 ? "Signature checked · first look"
                                     : root.cp.consistent === 1 ? "Signature and history checked" : "History check failed")
                                    + " · key " + root.cp.keyFingerprint
                        }
                        Stat {
                            label: "Your messages checked here"
                            value: root.log.checkingMine ? "Checking…"
                                 : root.log.mineMismatches > 0 ? root.log.mineMismatches + (root.log.mineMismatches > 1 ? " mismatches" : " mismatch")
                                 : root.log.mineMatched > 0 ? "All match" : "Nothing yet"
                            tone: root.log.mineMismatches > 0 ? Theme.danger : Theme.accent
                            detail: root.log.pendingMine.length > 0 ? root.log.pendingMine.length + " waiting for the next checkpoint"
                                  : root.log.mineMatched > 0 ? root.log.mineMatched + " checked · 0 mismatches" : ""
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        SearchField {
                            id: logSearch
                            Layout.fillWidth: true
                            placeholderText: "Search by entry number or commitment"
                            onAccepted: root.log.search = text
                            onTextChanged: searchDelay.restart()
                            Timer {
                                id: searchDelay
                                interval: 400
                                onTriggered: root.log.search = logSearch.text
                            }
                        }
                        Chip {
                            text: "All entries"
                            checked: !root.log.onlyMine
                            onClicked: root.log.onlyMine = false
                        }
                        Chip {
                            text: "Only mine"
                            checked: root.log.onlyMine
                            onClicked: root.log.onlyMine = true
                        }
                        RvButton {
                            secondary: true
                            iconName: "download"
                            text: "Export"
                            onClicked: {
                                App.lock.expectExternalReturn()
                                exportDialog.open()
                            }
                        }
                    }

                    Banner {
                        Layout.fillWidth: true
                        text: root.log.failedRows > 0
                              ? root.log.failedRows + (root.log.failedRows === 1 ? " listed entry doesn't" : " listed entries don't")
                                + " prove to be in the signed checkpoint. The log is not behaving honestly for "
                                + (root.log.failedRows === 1 ? "it." : "them.")
                              : ""
                    }

                    // --- Table ---
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: table.implicitHeight
                        radius: Theme.radiusCard
                        color: Theme.surface
                        border.color: Theme.borderStrong
                        clip: true

                        ColumnLayout {
                            id: table
                            width: parent.width
                            spacing: 0

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40
                                Layout.leftMargin: 16
                                Layout.rightMargin: 16
                                spacing: 12
                                Repeater {
                                    model: [["Entry", 90], ["Commitment", -1], ["Status", 130]]
                                    delegate: Text {
                                        textFormat: Text.PlainText
                                        required property var modelData
                                        Layout.preferredWidth: modelData[1] > 0 ? modelData[1] : -1
                                        Layout.fillWidth: modelData[1] < 0
                                        text: modelData[0]
                                        color: Theme.text3
                                        font.pixelSize: 12
                                        font.weight: Font.DemiBold
                                    }
                                }
                            }
                            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }

                            // Your messages not in a checkpoint yet.
                            Repeater {
                                model: root.log.onlyMine ? root.log.pendingMine : []
                                delegate: RowLayout {
                                    id: pendingRow
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 44
                                    Layout.leftMargin: 16
                                    Layout.rightMargin: 16
                                    spacing: 12
                                    Text { Layout.preferredWidth: 90; text: "—"; color: Theme.text3; font.family: Theme.monoFamily }
                                    Text {
                                        Layout.fillWidth: true
                                        text: pendingRow.modelData.commitmentShort
                                        textFormat: Text.PlainText
                                        color: Theme.text2
                                        font.family: Theme.monoFamily
                                        font.pixelSize: 13
                                    }
                                    RowLayout {
                                        Layout.preferredWidth: 130
                                        spacing: 5
                                        Icon { name: "clock"; size: 14; color: Theme.warn }
                                        Text { text: "Pending"; color: Theme.warn; font.pixelSize: 13 }
                                    }
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                Layout.margins: 20
                                visible: root.log.entries.length === 0 && (!root.log.onlyMine || root.log.pendingMine.length === 0)
                                horizontalAlignment: Text.AlignHCenter
                                text: root.log.onlyMine ? "None of your recent messages are in the log yet." : "No entries yet."
                                textFormat: Text.PlainText
                                color: Theme.text3
                            }

                            Repeater {
                                model: root.log.entries
                                delegate: ItemDelegate {
                                    id: entryRow
                                    required property var modelData
                                    Layout.fillWidth: true
                                    implicitHeight: 44
                                    leftPadding: 16
                                    rightPadding: 16
                                    onClicked: root.log.select(modelData.index)
                                    background: Rectangle {
                                        color: root.log.selected.index === entryRow.modelData.index ? Theme.selected
                                             : entryRow.hovered ? Qt.alpha(Theme.selected, 0.5) : "transparent"
                                    }
                                    contentItem: RowLayout {
                                        spacing: 12
                                        Text {
                                            Layout.preferredWidth: 90
                                            text: "#" + entryRow.modelData.index
                                            textFormat: Text.PlainText
                                            color: Theme.text
                                            font.family: Theme.monoFamily
                                            font.pixelSize: 13
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Text {
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                                text: entryRow.modelData.commitmentShort
                                                textFormat: Text.PlainText
                                                color: Theme.text2
                                                font.family: Theme.monoFamily
                                                font.pixelSize: 13
                                            }
                                            Rectangle {
                                                visible: entryRow.modelData.mine
                                                implicitWidth: yours.implicitWidth + 12
                                                implicitHeight: 20
                                                radius: 10
                                                color: Theme.yoursBg
                                                Text {
                                                    id: yours
                                                    anchors.centerIn: parent
                                                    text: "Yours"
                                                    color: Theme.yoursText
                                                    font.pixelSize: 11
                                                    font.weight: Font.DemiBold
                                                }
                                            }
                                        }
                                        RowLayout {
                                            Layout.preferredWidth: 130
                                            spacing: 5
                                            readonly property string s: entryRow.modelData.status
                                            Icon {
                                                name: parent.s === "verified" ? "check" : parent.s === "checking" ? "clock" : "close"
                                                size: 14
                                                color: parent.s === "verified" ? Theme.accent : parent.s === "checking" ? Theme.text3 : Theme.danger
                                            }
                                            Text {
                                                text: parent.s === "verified" ? "Verified here"
                                                    : parent.s === "checking" ? "Checking…"
                                                    : parent.s === "mismatch" ? "Mismatch" : "Proof failed"
                                                textFormat: Text.PlainText
                                                color: parent.s === "verified" ? Theme.accent : parent.s === "checking" ? Theme.text3 : Theme.danger
                                                font.pixelSize: 13
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- Entry details ---
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 360
            visible: root.log !== null && !root.log.disabled
            color: Theme.panel
            Rectangle {
                width: 1
                height: parent.height
                color: Theme.border
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 48
                visible: root.log.selected.index === undefined
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: "Select an entry to check its inclusion proof on this computer."
                color: Theme.text3
            }

            Flickable {
                id: panel
                anchors.fill: parent
                anchors.leftMargin: 1
                visible: root.log.selected.index !== undefined
                clip: true
                contentWidth: width
                contentHeight: panelColumn.implicitHeight + 48
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                ColumnLayout {
                    id: panelColumn
                    x: 24
                    y: 24
                    width: panel.width - 48
                    spacing: 12
                    readonly property var e: root.log.selected

                    Text {
                        text: "ENTRY"
                        color: Theme.text3
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.8
                    }
                    Text {
                        text: "#" + (panelColumn.e.index !== undefined ? panelColumn.e.index : "")
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.family: Theme.monoFamily
                        font.pixelSize: 28
                        font.weight: Font.Medium
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: proofText.implicitHeight + 24
                        radius: Theme.radiusControl
                        color: panelColumn.e.checking || panelColumn.e.ok ? Theme.accentSoft : Theme.dangerSoft
                        border.color: panelColumn.e.checking || panelColumn.e.ok ? Theme.accentBorder : "transparent"
                        Text {
                            id: proofText
                            anchors.fill: parent
                            anchors.margins: 12
                            wrapMode: Text.Wrap
                            text: panelColumn.e.checking ? "Checking the inclusion proof…"
                                : panelColumn.e.ok ? "Included in the latest signed checkpoint. The inclusion proof below checks out."
                                : "The inclusion proof doesn't check out against the signed checkpoint."
                            textFormat: Text.PlainText
                            color: panelColumn.e.checking || panelColumn.e.ok ? Theme.accent : Theme.danger
                            font.pixelSize: 13
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        visible: panelColumn.e.mine !== undefined && panelColumn.e.mine.length > 0
                        implicitHeight: mineText.implicitHeight + 24
                        radius: Theme.radiusControl
                        color: Theme.yoursBg
                        Text {
                            id: mineText
                            anchors.fill: parent
                            anchors.margins: 12
                            wrapMode: Text.Wrap
                            text: panelColumn.e.mine === "mismatch" ? "Your message. The commitment this computer computed doesn't match the log."
                                                                     : "Your message. The commitment this computer computed matches the log."
                            textFormat: Text.PlainText
                            color: Theme.yoursText
                            font.pixelSize: 13
                        }
                    }

                    Text { text: "Commitment"; color: Theme.text3; font.pixelSize: 12 }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: commitText.implicitHeight + 20
                        radius: Theme.radiusSmall
                        color: Theme.surface2
                        TextEdit {
                            id: commitText
                            anchors.fill: parent
                            anchors.margins: 10
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.WrapAnywhere
                            text: panelColumn.e.commitment || ""
                            color: Theme.text2
                            font.family: Theme.monoFamily
                            font.pixelSize: 12
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 12
                        rowSpacing: 8
                        Text { text: "In checkpoint"; color: Theme.text3; font.pixelSize: 13 }
                        Text {
                            text: panelColumn.e.checkpointSize ? "#" + panelColumn.e.checkpointSize + " entries" : "—"
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.family: Theme.monoFamily
                            font.pixelSize: 13
                        }
                        Text { text: "Scheme"; color: Theme.text3; font.pixelSize: 13 }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: "SHA-256 commitment v1 · RFC 9162 tree · Ed25519"
                            color: Theme.text
                            font.family: Theme.monoFamily
                            font.pixelSize: 12
                        }
                    }

                    Text { text: "Inclusion proof"; color: Theme.text3; font.pixelSize: 12 }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: pathColumn.implicitHeight + 20
                        radius: Theme.radiusSmall
                        color: Theme.surface2
                        Column {
                            id: pathColumn
                            x: 10
                            y: 10
                            width: parent.width - 20
                            spacing: 4
                            Repeater {
                                model: panelColumn.e.path || []
                                delegate: Text {
                                    textFormat: Text.PlainText
                                    required property string modelData
                                    width: pathColumn.width
                                    elide: Text.ElideMiddle
                                    text: modelData
                                    color: Theme.text2
                                    font.family: Theme.monoFamily
                                    font.pixelSize: 11
                                }
                            }
                            Text {
                                visible: !panelColumn.e.path || panelColumn.e.path.length === 0
                                text: panelColumn.e.checking ? "…" : "(single entry: no path)"
                                textFormat: Text.PlainText
                                color: Theme.text3
                                font.family: Theme.monoFamily
                                font.pixelSize: 11
                            }
                        }
                    }

                    RvButton {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        iconName: "shield-check"
                        text: "Re-check on this computer"
                        enabled: !panelColumn.e.checking
                        onClicked: root.log.recheckSelected()
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        RvButton {
                            Layout.fillWidth: true
                            secondary: true
                            iconName: root.copied ? "check" : "copy"
                            text: root.copied ? "Copied" : "Copy proof"
                            onClicked: {
                                root.log.copySelectedProof()
                                root.copied = true
                                copiedTimer.restart()
                            }
                            Timer {
                                id: copiedTimer
                                interval: 1500
                                onTriggered: root.copied = false
                            }
                        }
                    }
                }
            }
        }
    }
}
