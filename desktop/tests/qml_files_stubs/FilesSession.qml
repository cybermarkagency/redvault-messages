// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QtQuick

// Stands in for the app's FilesSession (C++) in file_preview_page_test.cpp: the test sets `viewer` itself.
QtObject {
    property var viewer: ({})
    property bool pdfSupported: true
    property int closes: 0
    function openPreview(id) {}
    function closePreview() {
        closes += 1
        viewer = ({})
    }
}
