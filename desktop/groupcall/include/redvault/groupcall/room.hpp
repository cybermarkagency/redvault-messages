#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "redvault/groupcall/ffi.hpp"

// A LiveKit room, as the group call code sees it: who is in it, their audio
// and video, and the end-to-end encryption key the frames are sealed with.
// Room is the interface (so the call logic can be tested without the media
// library); connectLiveKitRoom() is the real thing, on livekit-ffi.
//
// Callbacks come from the media library's dispatcher thread: hand the work
// to your own thread rather than doing it there.

namespace redvault::lk {

/// One picture in I420 (planar Y, then U, then V, the last two at half size). Width and height are even.
struct VideoFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> data;  // width*height*3/2 bytes

    std::uint8_t* y() { return data.data(); }
    std::uint8_t* u() { return data.data() + static_cast<std::size_t>(width) * height; }
    std::uint8_t* v() { return data.data() + static_cast<std::size_t>(width) * height * 5 / 4; }
    const std::uint8_t* y() const { return data.data(); }
    const std::uint8_t* u() const { return data.data() + static_cast<std::size_t>(width) * height; }
    const std::uint8_t* v() const { return data.data() + static_cast<std::size_t>(width) * height * 5 / 4; }
};

enum class VideoSource { Camera, Screen };

enum class RoomState { Connected, Reconnecting, Disconnected };

/// What the frame encryption of one participant reports (livekit-ffi's EncryptionState).
enum class E2eeState { New, Ok, EncryptionFailed, DecryptionFailed, MissingKey, KeyRatcheted, InternalError };

/// Someone else in the room, by their LiveKit identity (a per-call pseudonym).
struct RemoteParticipant {
    std::string identity;
    bool micPublished = false;
    bool micMuted = true;
    bool cameraPublished = false;
    bool cameraMuted = true;
    bool screenPublished = false;
    /// A picture of this person's camera / screen has arrived.
    bool cameraReceiving = false;
    bool screenReceiving = false;
};

struct RoomCallbacks {
    /// Connected, reconnecting or gone; `detail` says why when it's gone.
    std::function<void(RoomState state, const std::string& detail)> onState;
    /// Someone joined or left, or published, unpublished, muted or unmuted something: read participants() again.
    std::function<void()> onParticipantsChanged;
    /// Who is speaking right now (LiveKit identities, own included).
    std::function<void(const std::vector<std::string>& identities)> onActiveSpeakers;
    /// A decoded picture of someone's camera or shared screen.
    std::function<void(const std::string& identity, VideoSource source, const VideoFrame& frame)> onVideo;
    std::function<void(const std::string& identity, E2eeState state)> onEncryption;
};

struct RoomConfig {
    /// wss://host/path of the LiveKit server.
    std::string url;
    std::string token;
};

/// Every sample rate and frame size on this interface: 48 kHz mono, 20 ms frames.
inline constexpr int kSampleRate = 48000;
inline constexpr int kFrameSamples = 960;

class Room {
public:
    virtual ~Room() = default;

    /// This device's own LiveKit identity (the pseudonym the server gave it).
    virtual std::string localIdentity() const = 0;
    virtual std::vector<RemoteParticipant> participants() const = 0;

    /// Installs the room key of `epoch` (32 bytes) in key slot epoch % 16 and sends with it from now on.
    /// Frames other people send under earlier epochs still decrypt. Nothing may be published before the first call.
    virtual void setKey(const std::vector<std::uint8_t>& key, std::int64_t epoch) = 0;

    /// Whether what this device sends would be sealed right now: a room key is installed and frame encryption is on.
    virtual bool frameEncryptionActive() = 0;

    /// Starts sending the microphone. Blocks until it is published. Throws FfiError, and sends nothing, if
    /// the published track's frame encryption can't be confirmed.
    virtual void publishMicrophone() = 0;
    /// One 20 ms frame of microphone audio (kFrameSamples samples).
    virtual void sendAudio(const std::int16_t* pcm) = 0;
    virtual void setMicrophoneMuted(bool muted) = 0;
    /// Fills `out` with everyone else's audio, mixed (silence when there's none).
    virtual void readAudio(std::int16_t* out, std::size_t samples) = 0;

    /// Starts a camera or screen track of about this size. Blocks until it is published. Same rule as the microphone.
    virtual void publishVideo(VideoSource source, int width, int height) = 0;
    virtual void sendVideo(VideoSource source, const VideoFrame& frame) = 0;
    /// A camera that's off stays published but muted, so others show the avatar instead.
    virtual void setVideoMuted(VideoSource source, bool muted) = 0;
    virtual void unpublishVideo(VideoSource source) = 0;

    /// Leaves the room and releases everything. Safe to call more than once.
    virtual void disconnect() = 0;
};

using RoomFactory = std::function<std::unique_ptr<Room>(const RoomConfig& config, RoomCallbacks callbacks)>;

/// Connects through livekit-ffi with end-to-end encryption on (a shared key per epoch, AES-GCM, HKDF; the same
/// settings as the web app's RoomKeyProvider). Blocks until connected. Throws FfiError.
std::unique_ptr<Room> connectLiveKitRoom(const RoomConfig& config, RoomCallbacks callbacks);

}  // namespace redvault::lk
