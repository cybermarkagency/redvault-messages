#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/api/live_api.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/groupcall/room.hpp"
#include "redvault/model/group_call_keys.hpp"

namespace redvault::lk {

/**
 * One group call, from the ring to the end: the desktop counterpart of the web
 * app's state/GroupCallContext.tsx, and interoperable with it. Qt-free, and
 * driven from one thread (the app's UI thread) so it needs no locks of its own:
 *
 *   - Setup: start or join over the API (the server checks the plan and the
 *     group), connect to LiveKit with the token it hands back, then follow the
 *     live channel's roster and room-key events (model/group_call_keys.hpp).
 *   - Nothing is published until a room key is held and the room confirms frame
 *     encryption is on; LiveKit drops any frame it has no key for rather than send
 *     it in the clear. Encryption failing later is shown to the person, and if it's
 *     their own sending that failed, the call ends here.
 *   - Being removed from the group ends the call at once.
 *   - Once the starter is connected (and only then), its app posts the call's
 *     link in the group chat.
 *   - Blocking work (HTTP, connecting, publishing) runs on a serial background
 *     worker, in order, and reports back through `post`; the view is only ever
 *     touched on the owner thread.
 *
 * Audio and video go through sendAudio / readAudio / sendVideo, which are safe
 * to call from the audio and capture threads.
 */
class GroupCallSession {
public:
    /// A member of the group, as the app knows them from its group list.
    struct Member {
        std::string userId;
        std::string handle;
        std::string avatarUrl;
        crypto::Bytes publicKey;
    };
    struct GroupInfo {
        std::string label;
        std::vector<Member> members;
    };

    struct Environment {
        std::shared_ptr<api::ApiClient> api;
        std::shared_ptr<const data::SelfIdentity> self;
        /// The web app's address, ending in '/': LiveKit's path is relative to its origin, and invite links point here.
        std::string siteBase;
        /// A group chat by conversation id; nullopt if it isn't loaded.
        std::function<std::optional<GroupInfo>(const std::string& conversationId)> group;
        /// The account's plan ("free", "pro", "premium"), looked up when screen sharing is asked for.
        std::function<std::string()> tier;
        /// A one-to-one call is going on: nobody is in two calls at once.
        std::function<bool()> otherCallActive;
        /// Runs a function later on the owner thread (queued, never inline).
        std::function<void(std::function<void()>)> post;
        /// Runs blocking functions one after another on a background thread. Leave empty for the built-in worker.
        std::function<void(std::function<void()>)> background;
        /// Posts a message in the group chat as an ordinary end-to-end encrypted message. Blocks; returns false on failure.
        std::function<bool(const std::string& conversationId, const std::string& text)> announce;
        /// Connects to LiveKit. Defaults to connectLiveKitRoom.
        RoomFactory roomFactory;
        std::function<std::int64_t()> now;
    };

    /// One picture on the call screen: a person's camera (or avatar when it is off) or a shared screen.
    struct Tile {
        std::string key;  // "self", "self:screen", "<identity>:camera", "<identity>:screen"
        std::string userId;
        /// What the tile is labelled with ("You" for yourself).
        std::string handle;
        /// Their own name, for the initial on their avatar.
        std::string name;
        std::string avatarUrl;
        bool isSelf = false;
        bool screen = false;
        bool videoOn = false;
        bool micMuted = false;
        bool speaking = false;
    };

    struct View {
        bool present = false;
        std::string callId;
        std::string conversationId;
        std::string groupLabel;
        std::string kind;   // "voice" or "video"
        std::string phase;  // "incoming", "connecting", "active", "reconnecting", "ended"
        std::string hostHandle;
        bool muted = false;
        bool cameraOff = true;
        bool sharingScreen = false;
        bool minimized = false;
        std::int64_t connectedAt = 0;
        std::vector<Tile> tiles;
        std::vector<std::string> sas;
        /// A room key for the current epoch is held (until then nothing is sent or shown).
        bool keyReady = false;
        /// The microphone (and camera, in a video call) is being sent.
        bool published = false;
        std::string notice;
        std::string endedMessage;
    };

    struct ActiveCall {
        std::string callId;
        std::string kind;
        int participants = 0;
    };

    struct Hooks {
        /// The view changed (owner thread).
        std::function<void()> changed;
        /// The calls going on in your groups changed (owner thread).
        std::function<void()> activeChanged;
        /// A decoded picture for a tile, from the media library's thread.
        std::function<void(const std::string& tileKey, const VideoFrame& frame)> video;
    };

    GroupCallSession(Environment environment, Hooks hooks);
    ~GroupCallSession();
    GroupCallSession(const GroupCallSession&) = delete;
    GroupCallSession& operator=(const GroupCallSession&) = delete;

    // --- Owner thread ---

    /// Starts a call in a group (or joins the one already going on there). Returns a message to show instead when it can't.
    std::string start(const std::string& conversationId, const std::string& kind);
    /// Joins the call a pasted link points to. Returns a message to show instead when it can't; a call that has
    /// ended is reported on the call screen.
    std::string joinByLink(const std::string& callId);
    /// Rings: a group-incoming event for a group you're in.
    void accept();
    void decline();
    void leave();
    void toggleMute();
    void toggleCamera();
    /// The camera couldn't be opened: mute the track and say so.
    void cameraFailed();
    /// Returns a message when it isn't allowed (it needs Premium).
    std::string toggleScreenShare();
    /// The screen couldn't be captured, or the person stopped sharing from the system's own controls.
    void screenStopped();
    void setMinimized(bool minimized);
    /// Closes the "call ended" banner.
    void dismiss();
    void onLiveEvent(const api::live::LiveEvent& event);
    /// Asks the server which of your groups have a call going on.
    void refreshActive();
    void clearActive();

    const View& view() const { return view_; }
    const std::map<std::string, ActiveCall>& active() const { return active_; }
    bool inCall() const;

    // --- Any thread ---
    /// One 20 ms frame (kFrameSamples) from the microphone.
    void sendAudio(const std::int16_t* pcm);
    void readAudio(std::int16_t* out, std::size_t samples);
    void sendVideo(VideoSource source, const VideoFrame& frame);

private:
    struct Call;
    struct Shared;

    bool busy() const;
    void finish(const std::string& message);
    void connect(const api::groupcalls::Join& join);
    void onJoined(std::uint64_t generation, api::groupcalls::Join join);
    void onFailed(std::uint64_t generation, const std::string& message);
    void onConnected(std::uint64_t generation, std::shared_ptr<Room> room);
    void onPublished(std::uint64_t generation, bool encryptionOk, bool microphoneOk, bool cameraOk);
    void onEncryption(std::uint64_t generation, const std::string& identity, E2eeState state);
    void onScreen(std::uint64_t generation, bool on, bool ok);
    void onRoomState(std::uint64_t generation, RoomState state, const std::string& detail);
    void handleCallEvent(const api::live::LiveEvent& event);
    void apply(const model::group_calls::RoomKeyExchange::Outcome& outcome);
    void refreshTiles();
    void changed();
    void post(std::function<void()> fn);
    void background(std::function<void()> fn);
    std::optional<crypto::Bytes> publicKeyOf(const std::string& userId) const;
    RoomCallbacks callbacksFor(std::uint64_t generation);
    std::shared_ptr<Room> mediaRoom() const;

    Environment env_;
    Hooks hooks_;
    std::shared_ptr<Shared> shared_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::unique_ptr<Call> call_;
    std::uint64_t generation_ = 0;
    View view_;
    std::map<std::string, ActiveCall> active_;
};

}  // namespace redvault::lk
