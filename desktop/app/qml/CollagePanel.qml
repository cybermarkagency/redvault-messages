// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// Collages for the story studio: two to four photos in one of six layouts, with spacing and a background, drawn on
// this computer into one fresh 1080 x 1920 JPEG (CollageMaker in studio_tools.hpp), as on Android and the web.
Rectangle {
    id: panel
    signal done(url file)
    signal cancelled()

    color: Theme.panel
    Accessible.role: Accessible.Dialog
    Accessible.name: "Make a collage"
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }

    CollageMaker { id: maker; objectName: "collageMaker" }

    FileDialog {
        id: pick
        title: qsTr("Add photos to the collage")
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Photos (*.jpg *.jpeg *.png *.webp *.bmp)"]
        onAccepted: maker.addPhotos(selectedFiles)
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        // The collage as it will look.
        Item {
            Layout.fillHeight: true
            Layout.preferredWidth: height * 9 / 16
            Rectangle {
                anchors.fill: parent
                radius: Theme.radiusCard
                color: Theme.surface2
                clip: true
                Image {
                    objectName: "collagePreview"
                    anchors.fill: parent
                    visible: maker.ready
                    source: maker.previewUrl
                    cache: false
                    smooth: true
                    fillMode: Image.PreserveAspectFit
                }
                Text {
                    anchors.centerIn: parent
                    width: parent.width - 40
                    visible: !maker.ready
                    text: qsTr("Add two to four photos.")
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: Theme.text2
                    font.pixelSize: Theme.fontSize
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                RvButton { secondary: true; text: qsTr("Cancel"); onClicked: panel.cancelled() }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Collage")
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                }
                RvButton {
                    text: qsTr("Use collage")
                    enabled: maker.ready
                    onClicked: {
                        const file = maker.save()
                        if (file.toString().length > 0) panel.done(file)
                    }
                }
            }
            Text {
                text: qsTr("Photos (%1 of 4)").arg(maker.count)
                color: Theme.text
                font.pixelSize: Theme.fontSize
                font.weight: Font.DemiBold
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                Repeater {
                    model: maker.photos
                    delegate: Rectangle {
                        id: thumb
                        required property var modelData
                        required property int index
                        width: 96
                        height: 132
                        radius: Theme.radiusSmall
                        color: Theme.surface2
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: thumb.modelData
                            sourceSize: Qt.size(192, 264)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            autoTransform: true
                        }
                        Row {
                            anchors.bottom: parent.bottom
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottomMargin: 4
                            spacing: 2
                            SmallButton { text: "‹"; tip: qsTr("Earlier"); enabled: thumb.index > 0; onClicked: maker.movePhoto(thumb.index, -1) }
                            SmallButton { text: "×"; tip: qsTr("Remove this photo"); onClicked: maker.removePhoto(thumb.index) }
                            SmallButton { text: "›"; tip: qsTr("Later"); enabled: thumb.index + 1 < maker.count; onClicked: maker.movePhoto(thumb.index, 1) }
                        }
                    }
                }
                RvButton {
                    secondary: true
                    iconName: "plus"
                    text: qsTr("Add photos")
                    enabled: maker.count < 4
                    onClicked: pick.open()
                }
            }
            Text {
                visible: maker.layouts.length > 0
                text: qsTr("Layout")
                color: Theme.text
                font.pixelSize: Theme.fontSize
                font.weight: Font.DemiBold
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: maker.layouts
                    delegate: Chip {
                        required property var modelData
                        text: modelData.label
                        checked: maker.layout === modelData.id
                        onClicked: maker.layout = modelData.id
                    }
                }
            }
            StudioSlider {
                Layout.fillWidth: true
                Layout.maximumWidth: 360
                label: qsTr("Spacing")
                from: 0
                to: 40
                stepSize: 1
                value: maker.gap
                onMoved: (v) => maker.gap = v
            }
            Text { text: qsTr("Background"); color: Theme.text; font.pixelSize: Theme.fontSize; font.weight: Font.DemiBold }
            Row {
                spacing: 8
                Repeater {
                    model: maker.backgrounds
                    delegate: AbstractButton {
                        id: swatch
                        required property var modelData
                        required property int index
                        width: 30
                        height: 30
                        Accessible.name: swatch.modelData.label
                        onClicked: maker.background = swatch.index
                        background: Rectangle {
                            radius: 15
                            color: swatch.modelData.color
                            border.width: maker.background === swatch.index ? 3 : 1
                            border.color: Theme.text
                        }
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                visible: maker.error.length > 0
                text: maker.error
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.danger
                font.pixelSize: Theme.fontSmall
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("The collage is drawn on this computer into a new picture, so the photos' location and camera details stay behind.")
                wrapMode: Text.Wrap
                color: Theme.text3
                font.pixelSize: Theme.fontSmall
            }
            Item { Layout.fillHeight: true }
        }
    }

    component SmallButton: AbstractButton {
        id: b
        property string tip
        width: 26
        height: 26
        opacity: enabled ? 1 : 0.35
        Accessible.name: b.tip
        ToolTip.visible: hovered && b.tip.length > 0
        ToolTip.text: b.tip
        background: Rectangle { radius: 13; color: Theme.storyScrim }
        contentItem: Text {
            text: b.text
            color: Theme.storyChrome
            font.pixelSize: 15
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
