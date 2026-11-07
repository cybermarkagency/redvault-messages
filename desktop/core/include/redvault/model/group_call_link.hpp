#pragma once

#include <optional>
#include <string>
#include <string_view>

// Group call addresses, as the web app writes and reads them
// (ui/src/lib/groupCallLink.ts). The starter's app posts the link in the group
// chat once it is connected; members paste it (or the whole message) into
// Calls -> Join a call link, or click it in the chat, and join in the app.
// joinUrl/startUrl are also what the web app's useGroupCallDeepLink opens, for
// links followed outside the app.
namespace redvault::model::group_calls {

/// "3f2b8c1e-9a4d-4e6f-8b7a-1c2d3e4f5a6b": a UUID's shape, either case.
bool isId(std::string_view text);

/// The call id in a pasted group call link, or in a whole chat message that
/// carries one (…?group-call=<call id>), in lowercase; nullopt for anything else.
std::optional<std::string> parseCallLink(std::string_view text);

/// The address that joins a call: <web app base>?group-call=<call id>. `webAppBase` ends with '/'.
std::string joinUrl(std::string_view webAppBase, std::string_view callId);

/// The address that starts a call in a group: <web app base>?start-group-call=<group id>&kind=voice|video.
/// Empty if `conversationId` isn't a UUID.
std::string startUrl(std::string_view webAppBase, std::string_view conversationId, std::string_view kind);

/// The chat message the starter's app posts, word for word as the web app's groupCallInviteText writes it.
/// `kind` is "voice" or "video"; `url` is joinUrl(...).
std::string inviteText(std::string_view kind, std::string_view url);

}  // namespace redvault::model::group_calls
