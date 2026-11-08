// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Stand-in for ForwardDialog: records what would have been forwarded.
import QtQuick

Item {
    required property ChatSession chats
    property var channelPost: null
    signal forwarded()
    function openForChannelPost(post) { channelPost = post }
}
