#include "redvault/groupcall/room.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "e2ee.pb.h"
#include "ffi.pb.h"
#include "redvault/crypto/bytes.hpp"
#include "redvault/groupcall/audio_mixer.hpp"

namespace redvault::lk {

namespace pb = ::livekit::proto;

namespace {

constexpr int kKeyRingSize = 16;  // the web's RoomKeyProvider keyringSize
constexpr const char* kRatchetSalt = "LKFrameEncryptionKey";
constexpr auto kConnectTimeout = std::chrono::seconds(40);
constexpr auto kCallTimeout = std::chrono::seconds(15);

E2eeState e2eeState(pb::EncryptionState s) {
    switch (s) {
        case pb::OK: return E2eeState::Ok;
        case pb::ENCRYPTION_FAILED: return E2eeState::EncryptionFailed;
        case pb::DECRYPTION_FAILED: return E2eeState::DecryptionFailed;
        case pb::MISSING_KEY: return E2eeState::MissingKey;
        case pb::KEY_RATCHETED: return E2eeState::KeyRatcheted;
        case pb::INTERNAL_ERROR: return E2eeState::InternalError;
        default: return E2eeState::New;
    }
}

VideoSource videoSource(pb::TrackSource s) {
    return s == pb::SOURCE_SCREENSHARE ? VideoSource::Screen : VideoSource::Camera;
}

std::int64_t nowMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Shared (never owned alone): the library's event dispatcher holds it while it runs a handler, so a room that
/// is being dropped never has a handler running on freed memory. RoomHandle is what callers own.
class LiveKitRoom final : public Room, public std::enable_shared_from_this<LiveKitRoom> {
public:
    explicit LiveKitRoom(RoomCallbacks callbacks) : ffi_(Ffi::get()), callbacks_(std::move(callbacks)) {}

    ~LiveKitRoom() override { shutdown(); }

    /// Leaves, and stops listening.
    void shutdown() {
        disconnect();
        if (listener_ != 0) {
            ffi_.removeListener(listener_);
            listener_ = 0;
        }
    }

    void connect(const RoomConfig& config) {
        listener_ = ffi_.addListener([weak = weak_from_this()](const pb::FfiEvent& e) {
            if (const auto self = weak.lock()) self->onEvent(e);
        });
        const std::uint64_t id = ffi_.nextAsyncId();
        auto done = ffi_.expect(id);

        pb::FfiRequest request;
        auto* connect = request.mutable_connect();
        connect->set_url(config.url);
        connect->set_token(config.token);
        connect->set_request_async_id(id);
        auto* options = connect->mutable_options();
        options->set_auto_subscribe(true);
        options->set_adaptive_stream(false);
        options->set_dynacast(false);
        // Relay only (docs/tor-routing-plan.md): media goes through LiveKit's own TURN relay, never straight
        // between this computer and anyone else. No reachable relay means no call, never a direct fallback.
        options->mutable_rtc_config()->set_ice_transport_type(pb::TRANSPORT_RELAY);
        // The deprecated `e2ee` field: frame encryption only, like the web app's `e2ee` option (the newer
        // `encryption` also seals data packets, which group calls don't use).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        auto* e2ee = options->mutable_e2ee();
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        e2ee->set_encryption_type(pb::GCM);
        auto* provider = e2ee->mutable_key_provider_options();
        // A shared-key provider needs a first key. This one is random and never given to anyone: nobody can use
        // it, and the first real room key goes in its own slot before anything is sent.
        const auto placeholder = crypto::randomBytes(32);
        provider->set_shared_key(placeholder.data(), placeholder.size());
        provider->set_ratchet_window_size(0);
        provider->set_ratchet_salt(kRatchetSalt);
        provider->set_failure_tolerance(-1);
        provider->set_key_ring_size(kKeyRingSize);
        provider->set_key_derivation_function(pb::HKDF);

        try {
            ffi_.request(request);
        } catch (...) {
            ffi_.cancel(id);
            throw;
        }
        if (done.wait_for(kConnectTimeout) != std::future_status::ready) {
            ffi_.cancel(id);
            throw FfiError("Timed out connecting to the call server");
        }
        const auto event = done.get();
        const auto& callback = event->connect();
        if (callback.has_error()) throw FfiError(callback.error());

        const auto& result = callback.result();
        roomHandle_ = result.room().handle().id();
        localParticipantHandle_ = result.local_participant().handle().id();
        localIdentity_ = result.local_participant().info().identity();
        {
            std::lock_guard lock(mutex_);
            for (const auto& entry : result.participants()) {
                Remote& remote = remotes_[entry.participant().info().identity()];
                remote.identity = entry.participant().info().identity();
                owned_.push_back(entry.participant().handle().id());
                for (const auto& publication : entry.publications()) addPublication(remote, publication);
            }
        }
        connected_ = true;

        // Room events were held back until now.
        pb::FfiRequest ready;
        ready.mutable_ready_for_room_event()->set_room_handle(roomHandle_);
        ffi_.request(ready);
    }

    std::string localIdentity() const override { return localIdentity_; }

    std::vector<RemoteParticipant> participants() const override {
        std::lock_guard lock(mutex_);
        std::vector<RemoteParticipant> out;
        for (const auto& [identity, remote] : remotes_) {
            RemoteParticipant p;
            p.identity = identity;
            for (const auto& [sid, track] : remote.tracks) {
                if (!track.video) {
                    p.micPublished = true;
                    p.micMuted = track.muted;
                } else if (track.source == VideoSource::Screen) {
                    p.screenPublished = true;
                    p.screenReceiving = track.receiving;
                } else {
                    p.cameraPublished = true;
                    p.cameraMuted = track.muted;
                    p.cameraReceiving = track.receiving && !track.muted;
                }
            }
            out.push_back(std::move(p));
        }
        return out;
    }

    void setKey(const std::vector<std::uint8_t>& key, std::int64_t epoch) override {
        const int index = static_cast<int>(((epoch % kKeyRingSize) + kKeyRingSize) % kKeyRingSize);
        pb::FfiRequest request;
        auto* e2ee = request.mutable_e2ee();
        e2ee->set_room_handle(roomHandle_);
        auto* set = e2ee->mutable_set_shared_key();
        set->set_shared_key(key.data(), key.size());
        set->set_key_index(index);
        ffi_.request(request);
        keyIndex_ = index;
        applyKeyIndex();
    }

    bool frameEncryptionActive() override {
        if (keyIndex_.load() < 0) return false;
        pb::FfiRequest list;
        auto* e2ee = list.mutable_e2ee();
        e2ee->set_room_handle(roomHandle_);
        e2ee->mutable_manager_get_frame_cryptors();
        try {
            const pb::FfiResponse listed = ffi_.request(list);
            for (const auto& fc : listed.e2ee().manager_get_frame_cryptors().frame_cryptors()) {
                if (fc.participant_identity() == localIdentity_ && !fc.enabled()) return false;
            }
        } catch (const FfiError&) {
            return false;
        }
        return true;
    }

    void publishMicrophone() override {
        std::lock_guard publishing(publishMutex_);
        if (audio_.published) return;
        if (!frameEncryptionActive()) throw FfiError("Frame encryption isn't on");
        pb::FfiRequest source;
        auto* s = source.mutable_new_audio_source();
        s->set_type(pb::AUDIO_SOURCE_NATIVE);
        auto* options = s->mutable_options();
        // The app cleans the microphone itself (echo cancellation needs what the speaker plays).
        options->set_echo_cancellation(false);
        options->set_noise_suppression(false);
        options->set_auto_gain_control(false);
        s->set_sample_rate(kSampleRate);
        s->set_num_channels(1);
        s->set_queue_size_ms(500);
        const std::uint64_t sourceHandle = ffi_.request(source).new_audio_source().source().handle().id();

        pb::FfiRequest track;
        auto* t = track.mutable_create_audio_track();
        t->set_name("microphone");
        t->set_source_handle(sourceHandle);
        const std::uint64_t trackHandle = ffi_.request(track).create_audio_track().track().handle().id();

        pb::TrackPublishOptions options2;
        options2.set_source(pb::SOURCE_MICROPHONE);
        const std::string sid = publish(trackHandle, options2);
        {
            std::lock_guard lock(mutex_);
            audio_.sourceHandle = sourceHandle;
            audio_.trackHandle = trackHandle;
            audio_.sid = sid;
            audio_.published = true;
        }
        applyKeyIndex();
        // Muted before it was published (the person hit mute while connecting).
        if (audio_.muted) muteTrack(trackHandle, true);
        // Frames only start once the track's own cryptor is confirmed: sendAudio drops them until then.
        if (!trackEncrypted(sid)) throw FfiError("Frame encryption isn't on for the microphone");
        std::lock_guard lock(mutex_);
        audio_.encrypted = true;
    }

    void sendAudio(const std::int16_t* pcm) override {
        std::uint64_t source;
        {
            std::lock_guard lock(mutex_);
            if (!audio_.published || !audio_.encrypted || audio_.muted) return;
            source = audio_.sourceHandle;
        }
        pb::FfiRequest request;
        auto* capture = request.mutable_capture_audio_frame();
        capture->set_source_handle(source);
        auto* buffer = capture->mutable_buffer();
        buffer->set_data_ptr(reinterpret_cast<std::uint64_t>(pcm));
        buffer->set_num_channels(1);
        buffer->set_sample_rate(kSampleRate);
        buffer->set_samples_per_channel(kFrameSamples);
        try {
            ffi_.request(request);
        } catch (const FfiError&) {
        }
    }

    void setMicrophoneMuted(bool muted) override {
        std::uint64_t track;
        {
            std::lock_guard lock(mutex_);
            audio_.muted = muted;
            if (!audio_.published) return;
            track = audio_.trackHandle;
        }
        muteTrack(track, muted);
    }

    void readAudio(std::int16_t* out, std::size_t samples) override { mixer_.mix(out, samples); }

    void publishVideo(VideoSource source, int width, int height) override {
        std::lock_guard publishing(publishMutex_);
        LocalVideo& local = video(source);
        if (local.published) return;
        if (!frameEncryptionActive()) throw FfiError("Frame encryption isn't on");
        pb::FfiRequest request;
        auto* s = request.mutable_new_video_source();
        s->set_type(pb::VIDEO_SOURCE_NATIVE);
        s->mutable_resolution()->set_width(static_cast<std::uint32_t>(width));
        s->mutable_resolution()->set_height(static_cast<std::uint32_t>(height));
        s->set_is_screencast(source == VideoSource::Screen);
        const std::uint64_t sourceHandle = ffi_.request(request).new_video_source().source().handle().id();

        pb::FfiRequest track;
        auto* t = track.mutable_create_video_track();
        t->set_name(source == VideoSource::Screen ? "screen" : "camera");
        t->set_source_handle(sourceHandle);
        const std::uint64_t trackHandle = ffi_.request(track).create_video_track().track().handle().id();

        pb::TrackPublishOptions options;
        options.set_source(source == VideoSource::Screen ? pb::SOURCE_SCREENSHARE : pb::SOURCE_CAMERA);
        options.set_video_codec(pb::VP8);
        options.set_simulcast(false);
        auto* encoding = options.mutable_video_encoding();
        encoding->set_max_bitrate(source == VideoSource::Screen ? 1'800'000 : 900'000);
        encoding->set_max_framerate(source == VideoSource::Screen ? 15.0 : 24.0);
        const std::string sid = publish(trackHandle, options);
        {
            std::lock_guard lock(mutex_);
            local.sourceHandle = sourceHandle;
            local.trackHandle = trackHandle;
            local.sid = sid;
            local.published = true;
            local.muted = false;
        }
        applyKeyIndex();
        if (!trackEncrypted(sid)) {
            unpublish(source);
            throw FfiError("Frame encryption isn't on for this video");
        }
        std::lock_guard lock(mutex_);
        local.encrypted = true;
    }

    void sendVideo(VideoSource source, const VideoFrame& frame) override {
        std::uint64_t handle;
        {
            std::lock_guard lock(mutex_);
            const LocalVideo& local = video(source);
            if (!local.published || !local.encrypted || local.muted) return;
            handle = local.sourceHandle;
        }
        if (frame.width <= 0 || frame.height <= 0 || frame.data.size() < static_cast<std::size_t>(frame.width) * frame.height * 3 / 2) return;
        pb::FfiRequest request;
        auto* capture = request.mutable_capture_video_frame();
        capture->set_source_handle(handle);
        capture->set_timestamp_us(nowMicros());
        capture->set_rotation(pb::VIDEO_ROTATION_0);
        auto* buffer = capture->mutable_buffer();
        buffer->set_type(pb::I420);
        buffer->set_width(static_cast<std::uint32_t>(frame.width));
        buffer->set_height(static_cast<std::uint32_t>(frame.height));
        buffer->set_data_ptr(reinterpret_cast<std::uint64_t>(frame.y()));
        const std::uint32_t luma = static_cast<std::uint32_t>(frame.width) * frame.height;
        const std::uint32_t chroma = luma / 4;
        auto plane = [&](const std::uint8_t* data, std::uint32_t stride, std::uint32_t size) {
            auto* c = buffer->add_components();
            c->set_data_ptr(reinterpret_cast<std::uint64_t>(data));
            c->set_stride(stride);
            c->set_size(size);
        };
        plane(frame.y(), static_cast<std::uint32_t>(frame.width), luma);
        plane(frame.u(), static_cast<std::uint32_t>(frame.width / 2), chroma);
        plane(frame.v(), static_cast<std::uint32_t>(frame.width / 2), chroma);
        try {
            ffi_.request(request);
        } catch (const FfiError&) {
        }
    }

    void setVideoMuted(VideoSource source, bool muted) override {
        std::uint64_t track;
        {
            std::lock_guard lock(mutex_);
            LocalVideo& local = video(source);
            local.muted = muted;
            if (!local.published) return;
            track = local.trackHandle;
        }
        muteTrack(track, muted);
    }

    void unpublishVideo(VideoSource source) override {
        std::lock_guard publishing(publishMutex_);
        unpublish(source);
    }

    /// With publishMutex_ held.
    void unpublish(VideoSource source) {
        LocalVideo local;
        {
            std::lock_guard lock(mutex_);
            local = video(source);
            video(source) = LocalVideo{};
        }
        if (!local.published) return;
        const std::uint64_t id = ffi_.nextAsyncId();
        auto done = ffi_.expect(id);
        pb::FfiRequest request;
        auto* unpublish = request.mutable_unpublish_track();
        unpublish->set_local_participant_handle(localParticipantHandle_);
        unpublish->set_track_sid(local.sid);
        unpublish->set_stop_on_unpublish(true);
        unpublish->set_request_async_id(id);
        try {
            ffi_.request(request);
            done.wait_for(kCallTimeout);
        } catch (const FfiError&) {
            ffi_.cancel(id);
        }
        ffi_.dropHandle(local.trackHandle);
        ffi_.dropHandle(local.sourceHandle);
    }

    void disconnect() override {
        if (closed_.exchange(true)) return;
        if (connected_) {
            const std::uint64_t id = ffi_.nextAsyncId();
            auto done = ffi_.expect(id);
            pb::FfiRequest request;
            auto* disconnect = request.mutable_disconnect();
            disconnect->set_room_handle(roomHandle_);
            disconnect->set_request_async_id(id);
            try {
                ffi_.request(request);
                done.wait_for(std::chrono::seconds(5));
            } catch (const FfiError&) {
                ffi_.cancel(id);
            }
        }
        std::vector<std::uint64_t> handles;
        {
            std::lock_guard lock(mutex_);
            handles.swap(owned_);
            for (auto& [identity, remote] : remotes_) {
                for (auto& [sid, track] : remote.tracks) {
                    if (track.streamHandle) handles.push_back(track.streamHandle);
                    if (track.trackHandle) handles.push_back(track.trackHandle);
                }
            }
            remotes_.clear();
            streams_.clear();
            for (const LocalVideo* v : {&camera_, &screen_}) {
                if (v->trackHandle) handles.push_back(v->trackHandle);
                if (v->sourceHandle) handles.push_back(v->sourceHandle);
            }
            if (audio_.trackHandle) handles.push_back(audio_.trackHandle);
            if (audio_.sourceHandle) handles.push_back(audio_.sourceHandle);
            audio_ = LocalAudio{};
            camera_ = LocalVideo{};
            screen_ = LocalVideo{};
        }
        for (const auto h : handles) ffi_.dropHandle(h);
        if (roomHandle_) ffi_.dropHandle(roomHandle_);
        mixer_.clear();
    }

private:
    struct RemoteTrack {
        std::string sid;
        bool video = false;
        VideoSource source = VideoSource::Camera;
        bool muted = false;
        bool receiving = false;
        std::uint64_t publicationHandle = 0;
        std::uint64_t trackHandle = 0;
        std::uint64_t streamHandle = 0;
    };
    struct Remote {
        std::string identity;
        std::map<std::string, RemoteTrack> tracks;
    };
    struct LocalAudio {
        bool published = false;
        bool encrypted = false;
        bool muted = false;
        std::uint64_t sourceHandle = 0;
        std::uint64_t trackHandle = 0;
        std::string sid;
    };
    struct LocalVideo {
        bool published = false;
        bool encrypted = false;
        bool muted = false;
        std::uint64_t sourceHandle = 0;
        std::uint64_t trackHandle = 0;
        std::string sid;
    };
    struct StreamOwner {
        std::string identity;
        std::string sid;
        bool video = false;
    };

    LocalVideo& video(VideoSource s) { return s == VideoSource::Screen ? screen_ : camera_; }

    void addPublication(Remote& remote, const pb::OwnedTrackPublication& publication) {
        const auto& info = publication.info();
        RemoteTrack& track = remote.tracks[info.sid()];
        track.sid = info.sid();
        track.video = info.kind() == pb::KIND_VIDEO;
        track.source = videoSource(info.source());
        track.muted = info.muted();
        track.publicationHandle = publication.handle().id();
        owned_.push_back(publication.handle().id());
    }

    std::string publish(std::uint64_t trackHandle, const pb::TrackPublishOptions& options) {
        const std::uint64_t id = ffi_.nextAsyncId();
        auto done = ffi_.expect(id);
        pb::FfiRequest request;
        auto* publish = request.mutable_publish_track();
        publish->set_local_participant_handle(localParticipantHandle_);
        publish->set_track_handle(trackHandle);
        *publish->mutable_options() = options;
        publish->set_request_async_id(id);
        try {
            ffi_.request(request);
        } catch (...) {
            ffi_.cancel(id);
            throw;
        }
        if (done.wait_for(kCallTimeout) != std::future_status::ready) {
            ffi_.cancel(id);
            throw FfiError("Timed out publishing to the call");
        }
        const auto event = done.get();
        const auto& callback = event->publish_track();
        if (callback.has_error()) throw FfiError(callback.error());
        const std::string sid = callback.publication().info().sid();
        {
            std::lock_guard lock(mutex_);
            owned_.push_back(callback.publication().handle().id());
        }
        return sid;
    }

    void muteTrack(std::uint64_t track, bool muted) {
        pb::FfiRequest request;
        auto* mute = request.mutable_local_track_mute();
        mute->set_track_handle(track);
        mute->set_mute(muted);
        try {
            ffi_.request(request);
        } catch (const FfiError&) {
        }
    }

    /// Waits for this track's frame cryptor to show up, on and using the newest key's slot.
    bool trackEncrypted(const std::string& sid) {
        for (int attempt = 0; attempt < 40; ++attempt) {
            pb::FfiRequest list;
            auto* e2ee = list.mutable_e2ee();
            e2ee->set_room_handle(roomHandle_);
            e2ee->mutable_manager_get_frame_cryptors();
            try {
                const pb::FfiResponse listed = ffi_.request(list);
                for (const auto& fc : listed.e2ee().manager_get_frame_cryptors().frame_cryptors()) {
                    if (fc.participant_identity() != localIdentity_ || fc.track_sid() != sid) continue;
                    if (!fc.enabled()) return false;
                    if (fc.key_index() == keyIndex_.load()) return true;
                }
            } catch (const FfiError&) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    /// Makes every track this device sends use the newest room key's slot. The library keeps the slot per
    /// track and starts at 0, so it has to be told after a key arrives and after each publish.
    void applyKeyIndex() {
        const int index = keyIndex_.load();
        if (index < 0) return;
        for (int attempt = 0; attempt < 20; ++attempt) {
            pb::FfiRequest list;
            auto* e2ee = list.mutable_e2ee();
            e2ee->set_room_handle(roomHandle_);
            e2ee->mutable_manager_get_frame_cryptors();
            std::set<std::string> mine;
            try {
                const pb::FfiResponse listed = ffi_.request(list);
                for (const auto& fc : listed.e2ee().manager_get_frame_cryptors().frame_cryptors()) {
                    if (fc.participant_identity() != localIdentity_) continue;
                    mine.insert(fc.track_sid());
                    if (fc.key_index() == index) continue;
                    pb::FfiRequest set;
                    auto* s = set.mutable_e2ee();
                    s->set_room_handle(roomHandle_);
                    auto* k = s->mutable_cryptor_set_key_index();
                    k->set_participant_identity(fc.participant_identity());
                    k->set_track_sid(fc.track_sid());
                    k->set_key_index(index);
                    ffi_.request(set);
                }
            } catch (const FfiError&) {
                return;
            }
            // A track published a moment ago may not have its cryptor yet.
            std::set<std::string> expected;
            {
                std::lock_guard lock(mutex_);
                if (audio_.published) expected.insert(audio_.sid);
                for (const LocalVideo* v : {&camera_, &screen_}) {
                    if (v->published) expected.insert(v->sid);
                }
            }
            bool complete = true;
            for (const auto& sid : expected) complete = complete && mine.count(sid) > 0;
            if (complete) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void onEvent(const pb::FfiEvent& e) {
        if (closed_) return;
        switch (e.message_case()) {
            case pb::FfiEvent::kRoomEvent:
                if (e.room_event().room_handle() == roomHandle_) onRoomEvent(e.room_event());
                break;
            case pb::FfiEvent::kAudioStreamEvent: onAudio(e.audio_stream_event()); break;
            case pb::FfiEvent::kVideoStreamEvent: onVideo(e.video_stream_event()); break;
            default: break;
        }
    }

    void changed() {
        if (callbacks_.onParticipantsChanged) callbacks_.onParticipantsChanged();
    }

    void onRoomEvent(const pb::RoomEvent& e) {
        switch (e.message_case()) {
            case pb::RoomEvent::kParticipantConnected: {
                const auto& p = e.participant_connected().info();
                {
                    std::lock_guard lock(mutex_);
                    Remote& remote = remotes_[p.info().identity()];
                    remote.identity = p.info().identity();
                    owned_.push_back(p.handle().id());
                }
                changed();
                break;
            }
            case pb::RoomEvent::kParticipantDisconnected: {
                const std::string identity = e.participant_disconnected().participant_identity();
                std::vector<std::uint64_t> drop;
                {
                    std::lock_guard lock(mutex_);
                    const auto it = remotes_.find(identity);
                    if (it != remotes_.end()) {
                        for (auto& [sid, track] : it->second.tracks) {
                            if (track.streamHandle) {
                                streams_.erase(track.streamHandle);
                                drop.push_back(track.streamHandle);
                            }
                            if (track.trackHandle) drop.push_back(track.trackHandle);
                        }
                        remotes_.erase(it);
                    }
                }
                mixer_.remove(identity);
                for (const auto h : drop) ffi_.dropHandle(h);
                changed();
                break;
            }
            case pb::RoomEvent::kTrackPublished: {
                {
                    std::lock_guard lock(mutex_);
                    Remote& remote = remotes_[e.track_published().participant_identity()];
                    remote.identity = e.track_published().participant_identity();
                    addPublication(remote, e.track_published().publication());
                }
                changed();
                break;
            }
            case pb::RoomEvent::kTrackUnpublished: {
                const std::string identity = e.track_unpublished().participant_identity();
                const std::string sid = e.track_unpublished().publication_sid();
                std::vector<std::uint64_t> drop;
                bool audio = false;
                {
                    std::lock_guard lock(mutex_);
                    const auto it = remotes_.find(identity);
                    if (it != remotes_.end()) {
                        const auto t = it->second.tracks.find(sid);
                        if (t != it->second.tracks.end()) {
                            audio = !t->second.video;
                            if (t->second.streamHandle) {
                                streams_.erase(t->second.streamHandle);
                                drop.push_back(t->second.streamHandle);
                            }
                            if (t->second.trackHandle) drop.push_back(t->second.trackHandle);
                            it->second.tracks.erase(t);
                        }
                    }
                }
                if (audio) mixer_.remove(identity);
                for (const auto h : drop) ffi_.dropHandle(h);
                changed();
                break;
            }
            case pb::RoomEvent::kTrackSubscribed: subscribed(e.track_subscribed()); break;
            case pb::RoomEvent::kTrackUnsubscribed: {
                const std::string identity = e.track_unsubscribed().participant_identity();
                const std::string sid = e.track_unsubscribed().track_sid();
                std::vector<std::uint64_t> drop;
                bool audio = false;
                {
                    std::lock_guard lock(mutex_);
                    const auto it = remotes_.find(identity);
                    if (it != remotes_.end()) {
                        const auto t = it->second.tracks.find(sid);
                        if (t != it->second.tracks.end()) {
                            audio = !t->second.video;
                            if (t->second.streamHandle) {
                                streams_.erase(t->second.streamHandle);
                                drop.push_back(t->second.streamHandle);
                                t->second.streamHandle = 0;
                            }
                            if (t->second.trackHandle) {
                                drop.push_back(t->second.trackHandle);
                                t->second.trackHandle = 0;
                            }
                            t->second.receiving = false;
                        }
                    }
                }
                if (audio) mixer_.remove(identity);
                for (const auto h : drop) ffi_.dropHandle(h);
                changed();
                break;
            }
            case pb::RoomEvent::kTrackMuted:
                setMuted(e.track_muted().participant_identity(), e.track_muted().track_sid(), true);
                break;
            case pb::RoomEvent::kTrackUnmuted:
                setMuted(e.track_unmuted().participant_identity(), e.track_unmuted().track_sid(), false);
                break;
            case pb::RoomEvent::kActiveSpeakersChanged: {
                std::vector<std::string> identities(e.active_speakers_changed().participant_identities().begin(),
                                                    e.active_speakers_changed().participant_identities().end());
                if (callbacks_.onActiveSpeakers) callbacks_.onActiveSpeakers(identities);
                break;
            }
            case pb::RoomEvent::kConnectionStateChanged:
                if (callbacks_.onState) {
                    switch (e.connection_state_changed().state()) {
                        case pb::CONN_CONNECTED: callbacks_.onState(RoomState::Connected, {}); break;
                        case pb::CONN_RECONNECTING: callbacks_.onState(RoomState::Reconnecting, {}); break;
                        default: break;  // the Disconnected event carries the reason
                    }
                }
                break;
            case pb::RoomEvent::kReconnecting:
                if (callbacks_.onState) callbacks_.onState(RoomState::Reconnecting, {});
                break;
            case pb::RoomEvent::kReconnected:
                if (callbacks_.onState) callbacks_.onState(RoomState::Connected, {});
                break;
            case pb::RoomEvent::kDisconnected:
                if (callbacks_.onState) callbacks_.onState(RoomState::Disconnected, pb::DisconnectReason_Name(e.disconnected().reason()));
                break;
            case pb::RoomEvent::kE2EeStateChanged:
                if (callbacks_.onEncryption) callbacks_.onEncryption(e.e2ee_state_changed().participant_identity(), e2eeState(e.e2ee_state_changed().state()));
                break;
            default: break;
        }
    }

    void setMuted(const std::string& identity, const std::string& sid, bool muted) {
        {
            std::lock_guard lock(mutex_);
            const auto it = remotes_.find(identity);
            if (it == remotes_.end()) return;
            const auto t = it->second.tracks.find(sid);
            if (t == it->second.tracks.end()) return;
            t->second.muted = muted;
            if (muted) t->second.receiving = false;
        }
        changed();
    }

    void subscribed(const pb::TrackSubscribed& s) {
        const std::string identity = s.participant_identity();
        const auto& info = s.track().info();
        const bool isVideo = info.kind() == pb::KIND_VIDEO;
        pb::FfiRequest request;
        if (isVideo) {
            auto* stream = request.mutable_new_video_stream();
            stream->set_track_handle(s.track().handle().id());
            stream->set_type(pb::VIDEO_STREAM_NATIVE);
            stream->set_format(pb::I420);
            stream->set_normalize_stride(true);
            stream->set_queue_size_frames(3);
        } else {
            auto* stream = request.mutable_new_audio_stream();
            stream->set_track_handle(s.track().handle().id());
            stream->set_type(pb::AUDIO_STREAM_NATIVE);
            stream->set_sample_rate(kSampleRate);
            stream->set_num_channels(1);
        }
        std::uint64_t streamHandle = 0;
        try {
            const auto response = ffi_.request(request);
            streamHandle = isVideo ? response.new_video_stream().stream().handle().id() : response.new_audio_stream().stream().handle().id();
        } catch (const FfiError&) {
            ffi_.dropHandle(s.track().handle().id());
            return;
        }
        {
            std::lock_guard lock(mutex_);
            Remote& remote = remotes_[identity];
            remote.identity = identity;
            RemoteTrack& track = remote.tracks[info.sid()];
            track.sid = info.sid();
            track.video = isVideo;
            track.trackHandle = s.track().handle().id();
            track.streamHandle = streamHandle;
            streams_[streamHandle] = StreamOwner{identity, info.sid(), isVideo};
        }
        changed();
    }

    void onAudio(const pb::AudioStreamEvent& e) {
        if (e.message_case() != pb::AudioStreamEvent::kFrameReceived) return;
        const auto& frame = e.frame_received().frame();
        std::string identity;
        {
            std::lock_guard lock(mutex_);
            const auto it = streams_.find(e.stream_handle());
            if (it != streams_.end()) identity = it->second.identity;
        }
        const auto& info = frame.info();
        if (!identity.empty() && info.num_channels() == 1 && info.sample_rate() == kSampleRate) {
            mixer_.push(identity, reinterpret_cast<const std::int16_t*>(info.data_ptr()), info.samples_per_channel());
        }
        ffi_.dropHandle(frame.handle().id());
    }

    void onVideo(const pb::VideoStreamEvent& e) {
        if (e.message_case() != pb::VideoStreamEvent::kFrameReceived) return;
        const auto& buffer = e.frame_received().buffer();
        const auto& info = buffer.info();
        StreamOwner owner;
        bool known = false;
        {
            std::lock_guard lock(mutex_);
            const auto it = streams_.find(e.stream_handle());
            if (it != streams_.end()) {
                owner = it->second;
                known = true;
            }
        }
        if (known && info.type() == pb::I420 && info.components_size() == 3 && info.width() >= 2 && info.height() >= 2) {
            const int w = static_cast<int>(info.width()) & ~1;
            const int h = static_cast<int>(info.height()) & ~1;
            VideoFrame frame;
            frame.width = w;
            frame.height = h;
            frame.data.resize(static_cast<std::size_t>(w) * h * 3 / 2);
            std::uint8_t* dst = frame.data.data();
            for (int plane = 0; plane < 3; ++plane) {
                const auto& c = info.components(plane);
                const int pw = plane == 0 ? w : w / 2;
                const int ph = plane == 0 ? h : h / 2;
                const auto* src = reinterpret_cast<const std::uint8_t*>(c.data_ptr());
                for (int row = 0; row < ph; ++row) {
                    std::memcpy(dst, src + static_cast<std::size_t>(row) * c.stride(), static_cast<std::size_t>(pw));
                    dst += pw;
                }
            }
            bool first = false;
            VideoSource source = VideoSource::Camera;
            {
                std::lock_guard lock(mutex_);
                const auto r = remotes_.find(owner.identity);
                if (r != remotes_.end()) {
                    const auto t = r->second.tracks.find(owner.sid);
                    if (t != r->second.tracks.end()) {
                        source = t->second.source;
                        if (!t->second.receiving) {
                            t->second.receiving = true;
                            first = true;
                        }
                    }
                }
            }
            if (first) changed();
            if (callbacks_.onVideo) callbacks_.onVideo(owner.identity, source, frame);
        }
        ffi_.dropHandle(buffer.handle().id());
    }

    Ffi& ffi_;
    RoomCallbacks callbacks_;
    int listener_ = 0;
    std::atomic<bool> closed_{false};
    std::atomic<bool> connected_{false};
    std::uint64_t roomHandle_ = 0;
    std::uint64_t localParticipantHandle_ = 0;
    std::string localIdentity_;
    std::atomic<int> keyIndex_{-1};

    mutable std::mutex mutex_;
    std::mutex publishMutex_;
    std::map<std::string, Remote> remotes_;
    std::map<std::uint64_t, StreamOwner> streams_;
    std::vector<std::uint64_t> owned_;
    LocalAudio audio_;
    LocalVideo camera_;
    LocalVideo screen_;
    AudioMixer mixer_;
};

}  // namespace

namespace {

/// What callers own: forwards to the shared room, and shuts it down when dropped.
class RoomHandle final : public Room {
public:
    explicit RoomHandle(std::shared_ptr<LiveKitRoom> room) : room_(std::move(room)) {}
    ~RoomHandle() override { room_->shutdown(); }
    std::string localIdentity() const override { return room_->localIdentity(); }
    std::vector<RemoteParticipant> participants() const override { return room_->participants(); }
    void setKey(const std::vector<std::uint8_t>& key, std::int64_t epoch) override { room_->setKey(key, epoch); }
    bool frameEncryptionActive() override { return room_->frameEncryptionActive(); }
    void publishMicrophone() override { room_->publishMicrophone(); }
    void sendAudio(const std::int16_t* pcm) override { room_->sendAudio(pcm); }
    void setMicrophoneMuted(bool muted) override { room_->setMicrophoneMuted(muted); }
    void readAudio(std::int16_t* out, std::size_t samples) override { room_->readAudio(out, samples); }
    void publishVideo(VideoSource source, int width, int height) override { room_->publishVideo(source, width, height); }
    void sendVideo(VideoSource source, const VideoFrame& frame) override { room_->sendVideo(source, frame); }
    void setVideoMuted(VideoSource source, bool muted) override { room_->setVideoMuted(source, muted); }
    void unpublishVideo(VideoSource source) override { room_->unpublishVideo(source); }
    void disconnect() override { room_->disconnect(); }

private:
    std::shared_ptr<LiveKitRoom> room_;
};

}  // namespace

std::unique_ptr<Room> connectLiveKitRoom(const RoomConfig& config, RoomCallbacks callbacks) {
    auto room = std::make_shared<LiveKitRoom>(std::move(callbacks));
    auto handle = std::make_unique<RoomHandle>(room);  // if connecting fails, dropping it cleans up
    room->connect(config);
    return handle;
}

}  // namespace redvault::lk
