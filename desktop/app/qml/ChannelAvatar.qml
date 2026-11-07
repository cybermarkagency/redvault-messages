import QtQuick

// A channel's photo, or its initial, as the web app's ChannelAvatar. The
// "channelavatar" image provider fetches it without the session cookie (and
// with the private link's token when the channel is private), so loading a
// photo never tells the server which account looked.
Avatar {
    id: avatar
    property string name: ""
    /// The session's "photo" for the channel: its photo URL and access token, or empty.
    property string photoKey: ""

    initial: name.length > 0 ? String.fromCodePoint(name.codePointAt(0)).toUpperCase() : "?"
    tintKey: name
    photo: photoKey.length > 0 ? "image://channelavatar/" + encodeURIComponent(photoKey) : ""
}
