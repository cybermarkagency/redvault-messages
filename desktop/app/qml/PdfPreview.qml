// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Pdf
import Redvault

// A PDF in the Files viewer, page by page as pictures (Qt PDF / PDFium). Only
// the pages are drawn: links, forms and scripts in the file do nothing, so a
// PDF can't open a browser or reach the internet from here. Only built in
// when Qt PDF is available (app/CMakeLists.txt); FilePreviewDialog loads it.
Item {
    id: pdf
    property url source
    property real zoom: 1
    readonly property bool ready: doc.status === PdfDocument.Ready
    readonly property bool failed: doc.status === PdfDocument.Error
    readonly property int page: pages.count > 0 ? Math.max(0, pages.indexAt(pages.width / 2, pages.contentY + pages.height / 3)) : 0

    function zoomBy(factor) { pdf.zoom = Math.max(0.25, Math.min(4, pdf.zoom * factor)) }

    PdfDocument {
        id: doc
        source: pdf.source
    }

    ListView {
        id: pages
        anchors.fill: parent
        anchors.bottomMargin: bar.height + 12
        clip: true
        spacing: 16
        model: pdf.ready ? doc.pageCount : 0
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: Math.max(width, pageWidth)
        flickableDirection: Flickable.AutoFlickDirection
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar {}
        readonly property real pageWidth: Math.max(1, Math.min(width - 32, 900)) * pdf.zoom
        objectName: "pdfPages"
        // Page heights follow the width (window size, zoom): stay on the page being read when they change.
        property int anchorPage: 0
        onMovementEnded: anchorPage = pdf.page
        onPageWidthChanged: Qt.callLater(() => pages.positionViewAtIndex(pages.anchorPage, ListView.Beginning))
        onCountChanged: { anchorPage = 0; Qt.callLater(() => pages.positionViewAtBeginning()) }

        delegate: Item {
            id: pageItem
            required property int index
            readonly property size points: doc.pagePointSize(index)
            width: pages.contentWidth
            height: points.width > 0 ? pages.pageWidth * points.height / points.width : pages.pageWidth * 1.414
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: pages.pageWidth
                height: parent.height
                color: "white"
                PdfPageImage {
                    anchors.fill: parent
                    document: doc
                    currentFrame: pageItem.index
                    asynchronous: true
                    cache: false
                    fillMode: Image.PreserveAspectFit
                    sourceSize.width: Math.round(pages.pageWidth * Screen.devicePixelRatio)
                }
            }
        }
        WheelHandler {
            acceptedModifiers: Qt.ControlModifier
            onWheel: (event) => pdf.zoomBy(event.angleDelta.y > 0 ? 1.15 : 1 / 1.15)
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: 10
        visible: !pdf.ready
        BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: !pdf.failed; visible: running }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: pdf.failed ? qsTr("This PDF can't be shown here (it may be damaged or password-protected). Download it instead.")
                             : qsTr("Opening…")
            textFormat: Text.PlainText
            color: Theme.text2
            font.pixelSize: 14
        }
    }

    // Page number and zoom.
    Rectangle {
        id: bar
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        visible: pdf.ready
        width: barRow.implicitWidth + 16
        height: 40
        radius: 20
        color: Theme.surface2
        border.color: Theme.border
        RowLayout {
            id: barRow
            anchors.centerIn: parent
            spacing: 4
            IconButton { iconName: "minus"; tip: qsTr("Zoom out (Ctrl + scroll)"); onClicked: pdf.zoomBy(1 / 1.25) }
            Text {
                Layout.minimumWidth: 52
                horizontalAlignment: Text.AlignHCenter
                text: Math.round(pdf.zoom * 100) + "%"
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: 13
            }
            IconButton { iconName: "plus"; tip: qsTr("Zoom in (Ctrl + scroll)"); onClicked: pdf.zoomBy(1.25) }
            Rectangle { width: 1; height: 20; color: Theme.border }
            Text {
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                text: qsTr("Page %1 of %2").arg(pdf.page + 1).arg(doc.pageCount)
                textFormat: Text.PlainText
                color: Theme.text2
                font.pixelSize: 13
            }
        }
    }
}
