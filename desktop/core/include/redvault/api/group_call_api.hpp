#pragma once

#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/api/live_api.hpp"

// The group call endpoints (server/src/routes/callRoutes.ts, mounted at
// /api/group-calls), mirroring ui/src/lib/api.ts. Free functions over
// ApiClient::call; all of them block. The events that go with them (ring,
// roster, room keys, ended) come over the live channel: live_api.hpp.

namespace redvault::api::groupcalls {

/// What starting or joining a call hands back: where to connect, and as whom.
struct Join {
    std::string callId;
    std::string conversationId;
    std::string kind;  // "voice" or "video"
    /// LiveKit's signaling address: a wss:// URL, or a path on the site (e.g. "/redvaultmessages/livekit").
    std::string url;
    std::string token;
    /// This person's pseudonym in the LiveKit room.
    std::string identity;
    /// This request started the call (rather than joining one already going on): its starter posts the link in the chat.
    bool created = false;
};

struct ActiveCall {
    std::string callId;
    std::string conversationId;
    std::string kind;
    int participants = 0;
};

/// Starts a call in a group, or joins the one already going on there. Only starting one needs Premium (403 "premium_required").
Join start(ApiClient& api, const std::string& conversationId, const std::string& kind);
/// Joins a call by its id: answering the ring, the Join button, or a pasted link. 404 "This call has ended." for anything else.
Join join(ApiClient& api, const std::string& callId);
/// Calls going on in your groups, for the Join banner.
std::vector<ActiveCall> active(ApiClient& api);
void decline(ApiClient& api, const std::string& callId);
void leave(ApiClient& api, const std::string& callId);

/// A room key sealed to one participant.
struct SealedKey {
    std::string userId;
    live::SealedSignal key;
};
/// The keyholder's copies of this epoch's room key. 409 "stale_epoch" if someone joined or left meanwhile
/// (a newer roster is on its way), 409 "not_keyholder" if it isn't their turn.
void sendKeys(ApiClient& api, const std::string& callId, std::int64_t epoch, const std::vector<SealedKey>& keys);

/// The LiveKit signaling address to connect to: `url` as the server gave it, made absolute with the site's
/// origin when it is only a path (wss for https, ws for http). `siteBase` is the web app's address, e.g.
/// "https://www.redvaultsystems.com/redvaultmessages/". Empty if it can't be worked out.
std::string signalingUrl(const std::string& url, const std::string& siteBase);

}  // namespace redvault::api::groupcalls
