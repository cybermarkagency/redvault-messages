import QtQuick
import Redvault

// A profile photo when there is one (fetched with the session cookie by the
// "avatar" image provider, already cut to a circle), otherwise an initial on
// one of the design's tints. People are circles, groups rounded squares. A
// community's or creator's picture comes from the "community" provider, cut
// to the same shape.
Rectangle {
    id: avatar
    property string initial: "?"
    property bool group: false
    property int size: 40
    /// The server's avatarUrl ("/api/users/<id>/avatar?v=..."), or empty.
    property string source: ""
    /// A whole image URL to show instead (a channel's photo, see ChannelAvatar), or empty.
    property string photo: ""
    /// A community's or creator's picture path (CommunitySession.imagePath / creatorAvatarPath), or empty.
    property string communityImage: ""
    /// Picks the tint (a user or chat id); the initial when empty.
    property string tintKey: ""
    /// A dot for "online" at the bottom right.
    property bool online: false

    width: size
    height: size
    radius: group ? Math.round(size * 0.3) : size / 2
    color: photo.status === Image.Ready ? "transparent" : Theme.tintFor(tintKey.length > 0 ? tintKey : initial)

    Text {
        anchors.centerIn: parent
        visible: photo.status !== Image.Ready
        text: avatar.initial
        textFormat: Text.PlainText
        color: Theme.avatarInk
        font.pixelSize: avatar.size * 0.4
        font.weight: Font.DemiBold
    }

    Image {
        id: photo
        anchors.fill: parent
        visible: status === Image.Ready
        asynchronous: true
        // Twice the size, for high-DPI screens.
        sourceSize: Qt.size(avatar.size * 2, avatar.size * 2)
        source: avatar.photo.length > 0 ? avatar.photo
              : avatar.communityImage.length > 0
                ? "image://community/" + (avatar.group ? "rounded" : "circle") + "/"
                  + (App.communities ? App.communities.imageRevision : 0) + "/" + encodeURIComponent(avatar.communityImage)
              : avatar.source.length > 0 && !avatar.group ? "image://avatar/" + encodeURIComponent(avatar.source) : ""
    }

    Rectangle {
        visible: avatar.online
        width: Math.max(10, avatar.size * 0.28)
        height: width
        radius: width / 2
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        color: Theme.accent
        border.width: 2
        border.color: Theme.panel
    }
}
