// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// The photo editor of the story studio: crop (free and fixed shapes), turn, flip, brightness, contrast, saturation,
// blur, eight looks, drawing and erasing, undo and redo. Everything happens on this computer; the result is a fresh
// JPEG without the original file's metadata (PhotoEditor in studio_tools.hpp). The same tools as Android and the web.
Rectangle {
    id: panel
    property url source
    signal done(url file)
    signal cancelled()

    property string tool: "adjust"  // "crop" | "adjust" | "looks" | "draw"
    property string penColor: "white"
    property real penSize: 0.02
    property bool eraser: false
    readonly property var aspects: [{ label: qsTr("Free"), value: 0 }, { label: "1:1", value: 1 }, { label: "4:5", value: 0.8 },
                                    { label: "9:16", value: 9 / 16 }, { label: "16:9", value: 16 / 9 }]

    color: Theme.panel
    Accessible.role: Accessible.Dialog
    Accessible.name: "Edit photo"
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }

    PhotoEditor {
        id: editor
        objectName: "photoEditor"
        source: panel.source
        cropping: panel.tool === "crop"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            RvButton { secondary: true; text: qsTr("Cancel"); onClicked: panel.cancelled() }
            Text {
                Layout.fillWidth: true
                text: qsTr("Edit photo")
                horizontalAlignment: Text.AlignHCenter
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            RvButton { secondary: true; text: qsTr("Undo"); enabled: editor.canUndo; onClicked: editor.undo() }
            RvButton { secondary: true; text: qsTr("Redo"); enabled: editor.canRedo; onClicked: editor.redo() }
            RvButton { secondary: true; text: qsTr("Reset"); enabled: editor.edited; onClicked: editor.resetAll() }
            RvButton {
                text: qsTr("Done")
                enabled: editor.loaded
                onClicked: {
                    const file = editor.save()
                    if (file.toString().length > 0) panel.done(file)
                    else errorText.text = qsTr("The photo couldn't be saved. Try again.")
                }
            }
        }

        // The picture, with the crop frame or the pen on top.
        Item {
            id: area
            Layout.fillWidth: true
            Layout.fillHeight: true
            Image {
                id: shown
                objectName: "photoPreview"
                anchors.centerIn: parent
                readonly property real aspect: editor.previewAspect
                width: Math.min(area.width, area.height * aspect)
                height: width / aspect
                source: editor.previewUrl
                cache: false
                smooth: true
                fillMode: Image.Stretch
            }
            Text {
                anchors.centerIn: parent
                visible: !editor.loaded
                text: editor.error.length > 0 ? editor.error : qsTr("Opening the photo…")
                color: Theme.text2
                font.pixelSize: Theme.fontSize
            }

            // Drawing: the pen follows the pointer over the finished picture.
            MouseArea {
                anchors.fill: shown
                enabled: panel.tool === "draw"
                cursorShape: Qt.CrossCursor
                onPressed: (mouse) => editor.beginStroke(mouse.x / shown.width, mouse.y / shown.height, panel.penColor, panel.penSize, panel.eraser)
                onPositionChanged: (mouse) => editor.extendStroke(mouse.x / shown.width, mouse.y / shown.height)
                onReleased: editor.endStroke()
                onCanceled: editor.endStroke()
            }

            // The crop: drag inside to move it, a corner or an edge to resize it.
            Item {
                id: cropFrame
                visible: panel.tool === "crop" && editor.loaded
                x: shown.x + editor.crop.x * shown.width
                y: shown.y + editor.crop.y * shown.height
                width: editor.crop.w * shown.width
                height: editor.crop.h * shown.height
                Rectangle { anchors.fill: parent; color: "transparent"; border.width: 2; border.color: Theme.storyChrome }
                Handle { handle: "move"; anchors.fill: parent; anchors.margins: 14 }
                Handle { handle: "n"; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14; height: 14; anchors.topMargin: -7 }
                Handle { handle: "s"; anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 14; height: 14; anchors.bottomMargin: -7 }
                Handle { handle: "w"; anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.margins: 14; width: 14; anchors.leftMargin: -7 }
                Handle { handle: "e"; anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.right: parent.right; anchors.margins: 14; width: 14; anchors.rightMargin: -7 }
                Handle { handle: "nw"; x: -9; y: -9; width: 18; height: 18; dot: true }
                Handle { handle: "ne"; x: parent.width - 9; y: -9; width: 18; height: 18; dot: true }
                Handle { handle: "sw"; x: -9; y: parent.height - 9; width: 18; height: 18; dot: true }
                Handle { handle: "se"; x: parent.width - 9; y: parent.height - 9; width: 18; height: 18; dot: true }
            }
        }

        Text {
            id: errorText
            Layout.fillWidth: true
            visible: text.length > 0
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.danger
            font.pixelSize: Theme.fontSmall
        }

        // Which tool.
        Row {
            Layout.alignment: Qt.AlignHCenter
            spacing: 6
            Repeater {
                model: [{ id: "crop", label: qsTr("Crop and turn") }, { id: "adjust", label: qsTr("Adjust") }, { id: "looks", label: qsTr("Looks") }, { id: "draw", label: qsTr("Draw") }]
                delegate: Chip {
                    required property var modelData
                    text: modelData.label
                    checked: panel.tool === modelData.id
                    onClicked: panel.tool = modelData.id
                }
            }
        }

        // Crop and turn.
        ColumnLayout {
            Layout.fillWidth: true
            visible: panel.tool === "crop"
            spacing: 8
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: panel.aspects
                    delegate: Chip {
                        required property var modelData
                        text: modelData.label
                        onClicked: editor.setAspect(modelData.value)
                    }
                }
                Chip { text: qsTr("No crop"); enabled: editor.hasCrop; onClicked: editor.setAspect(-1) }
            }
            Row {
                spacing: 6
                RvButton { secondary: true; text: qsTr("⟲ Turn left"); onClicked: editor.rotate(-1) }
                RvButton { secondary: true; text: qsTr("Turn right ⟳"); onClicked: editor.rotate(1) }
                RvButton { secondary: true; text: editor.flipX ? qsTr("Flipped ✓") : qsTr("Flip"); onClicked: editor.flip() }
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Turning, flipping or cropping clears what you drew.")
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
        }

        // Adjust.
        GridLayout {
            Layout.fillWidth: true
            visible: panel.tool === "adjust"
            columns: 2
            columnSpacing: 16
            rowSpacing: 4
            Repeater {
                model: [{ id: "brightness", label: qsTr("Brightness"), from: -100 }, { id: "contrast", label: qsTr("Contrast"), from: -100 },
                        { id: "saturation", label: qsTr("Saturation"), from: -100 }, { id: "blur", label: qsTr("Blur"), from: 0 }]
                delegate: StudioSlider {
                    required property var modelData
                    Layout.fillWidth: true
                    label: modelData.label
                    from: modelData.from
                    to: 100
                    value: editor[modelData.id]
                    onBegan: editor.beginAdjust()
                    onMoved: (v) => editor.setAdjust(modelData.id, v)
                    onEnded: editor.endAdjust()
                }
            }
        }

        // Looks.
        Flow {
            Layout.fillWidth: true
            visible: panel.tool === "looks"
            spacing: 6
            Repeater {
                model: editor.filters
                delegate: Chip {
                    required property var modelData
                    text: modelData.label
                    checked: editor.filter === modelData.id
                    onClicked: editor.setFilter(modelData.id)
                }
            }
        }

        // Draw.
        ColumnLayout {
            Layout.fillWidth: true
            visible: panel.tool === "draw"
            spacing: 8
            Flow {
                Layout.fillWidth: true
                spacing: 8
                Repeater {
                    model: editor.penColors
                    delegate: AbstractButton {
                        id: swatch
                        required property string modelData
                        width: 26
                        height: 26
                        Accessible.name: swatch.modelData
                        onClicked: {
                            panel.penColor = swatch.modelData
                            panel.eraser = false
                        }
                        background: Rectangle {
                            radius: 13
                            color: Theme.storyColors[swatch.modelData] || "white"
                            border.width: panel.penColor === swatch.modelData && !panel.eraser ? 3 : 1
                            border.color: Theme.text
                        }
                    }
                }
                Chip { text: qsTr("Eraser"); checked: panel.eraser; onClicked: panel.eraser = !panel.eraser }
            }
            StudioSlider {
                Layout.fillWidth: true
                label: qsTr("Pen size")
                from: 0.004
                to: 0.08
                value: panel.penSize
                valueText: Math.round(panel.penSize * 1000) / 10 + "%"
                onMoved: (v) => panel.penSize = v
            }
            Text {
                Layout.fillWidth: true
                text: editor.strokeCount + (editor.strokeCount === 1 ? qsTr(" line drawn") : qsTr(" lines drawn")) + qsTr(". The eraser takes away drawing, never the photo.")
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
        }
    }

    component Handle: MouseArea {
        id: h
        property string handle
        property bool dot: false
        property point last
        cursorShape: handle === "move" ? Qt.SizeAllCursor : handle === "n" || handle === "s" ? Qt.SizeVerCursor
                   : handle === "e" || handle === "w" ? Qt.SizeHorCursor : handle === "nw" || handle === "se" ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
        Rectangle { anchors.centerIn: parent; visible: h.dot; width: 14; height: 14; radius: 7; color: Theme.storyChrome; border.width: 1; border.color: Theme.storyStage }
        onPressed: (mouse) => {
            h.last = mapToItem(area, mouse.x, mouse.y)
            editor.beginCropDrag()
        }
        onPositionChanged: (mouse) => {
            const p = mapToItem(area, mouse.x, mouse.y)
            editor.dragCrop(h.handle, (p.x - h.last.x) / shown.width, (p.y - h.last.y) / shown.height)
            h.last = p
        }
        onReleased: editor.endCropDrag()
        onCanceled: editor.endCropDrag()
    }
}
