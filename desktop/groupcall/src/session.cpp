#include "redvault/groupcall/session.hpp"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "redvault/api/api_error.hpp"
#include "redvault/api/group_call_api.hpp"
#include "redvault/crypto/group_call_crypto.hpp"
#include "redvault/model/group_call_link.hpp"

namespace redvault::lk {

namespace {

namespace gcm = model::group_calls;

constexpr const char* kEndedLeft = "You left the call";
constexpr const char* kEndedRemoved = "You were removed from this group, so you left the call";
constexpr const char* kEndedNoEncryption = "End-to-end encryption couldn't be turned on, so nothing was sent. Try joining again.";
constexpr const char* kEndedEncryptionFailed = "End-to-end encryption failed, so the call was closed here.";
constexpr const char* kNoticeUndecryptable = "Someone's audio or video can't be decrypted. It won't play until their key arrives.";
constexpr const char* kNoticeNotSealing = "Your audio or video isn't being sent: this call's key is missing here.";
constexpr std::size_t kMaxBuffered = 256;

/// Runs functions one after another on a thread of its own. The thread is detached, so a connect that is still
/// waiting on the network never holds up closing the app; drain() waits a little for it anyway.
class SerialWorker {
public:
    SerialWorker() : state_(std::make_shared<State>()) {
        std::thread([s = state_] { loop(s); }).detach();
    }
    ~SerialWorker() {
        {
            std::lock_guard lock(state_->mutex);
            state_->closed = true;
        }
        state_->cv.notify_all();
    }
    void run(std::function<void()> fn) {
        {
            std::lock_guard lock(state_->mutex);
            state_->queue.push_back(std::move(fn));
        }
        state_->cv.notify_all();
    }
    /// Waits until everything queued has run (or the time is up).
    void drain(std::chrono::milliseconds timeout) {
        std::unique_lock lock(state_->mutex);
        state_->cv.wait_for(lock, timeout, [&] { return state_->queue.empty() && !state_->busy; });
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::function<void()>> queue;
        bool closed = false;
        bool busy = false;
    };
    static void loop(std::shared_ptr<State> s) {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock lock(s->mutex);
                s->cv.wait(lock, [&] { return s->closed || !s->queue.empty(); });
                if (s->queue.empty()) return;
                fn = std::move(s->queue.front());
                s->queue.pop_front();
                s->busy = true;
            }
            try {
                fn();
            } catch (...) {
            }
            {
                std::lock_guard lock(s->mutex);
                s->busy = false;
            }
            s->cv.notify_all();
        }
    }
    std::shared_ptr<State> state_;
};

std::string errorMessage(const std::exception& e, const char* fallback) {
    if (dynamic_cast<const api::ApiError*>(&e) || dynamic_cast<const crypto::groupcalls::GroupKeyError*>(&e)) return e.what();
    return fallback;
}

}  // namespace

/// Shared with the media threads (which only need the room) and the background worker.
struct GroupCallSession::Shared {
    std::mutex mutex;
    std::shared_ptr<Room> room;
    std::unique_ptr<SerialWorker> worker;

    void setRoom(std::shared_ptr<Room> r) {
        std::lock_guard lock(mutex);
        room = std::move(r);
    }
    std::shared_ptr<Room> getRoom() {
        std::lock_guard lock(mutex);
        return room;
    }
};

struct GroupCallSession::Call {
    std::uint64_t generation = 0;
    std::string callId;
    std::string conversationId;
    std::string kind;
    std::unique_ptr<gcm::RoomKeyExchange> exchange;
    std::shared_ptr<Room> room;
    std::vector<api::live::LiveEvent> buffer;
    std::set<std::string> speaking;
    std::string localIdentity;
    bool ringing = false;
    bool joinedServer = false;
    bool ready = false;
    bool announce = false;
    bool publishStarted = false;
    bool screenBusy = false;
    bool closed = false;
    /// Everyone whose frame encryption is reporting a problem right now.
    std::set<std::string> encryptionProblems;
};

GroupCallSession::GroupCallSession(Environment environment, Hooks hooks)
    : env_(std::move(environment)), hooks_(std::move(hooks)), shared_(std::make_shared<Shared>()) {
    if (!env_.roomFactory) env_.roomFactory = connectLiveKitRoom;
    if (!env_.now) {
        env_.now = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        };
    }
}

GroupCallSession::~GroupCallSession() {
    *alive_ = false;
    if (call_) finish("");
    // Let the goodbye (disconnect, leave) go out before the app closes.
    if (shared_->worker) shared_->worker->drain(std::chrono::seconds(4));
}

void GroupCallSession::post(std::function<void()> fn) {
    env_.post(std::move(fn));
}

void GroupCallSession::background(std::function<void()> fn) {
    if (env_.background) {
        env_.background(std::move(fn));
        return;
    }
    if (!shared_->worker) shared_->worker = std::make_unique<SerialWorker>();
    shared_->worker->run(std::move(fn));
}

void GroupCallSession::changed() {
    if (*alive_ && hooks_.changed) hooks_.changed();
}

bool GroupCallSession::inCall() const {
    return call_ != nullptr && view_.phase != "ended";
}

bool GroupCallSession::busy() const {
    return inCall() || (env_.otherCallActive && env_.otherCallActive());
}

std::shared_ptr<Room> GroupCallSession::mediaRoom() const {
    return shared_->getRoom();
}

std::optional<crypto::Bytes> GroupCallSession::publicKeyOf(const std::string& userId) const {
    if (userId == env_.self->userId) return env_.self->publicKey;
    if (!call_) return std::nullopt;
    const auto group = env_.group(call_->conversationId);
    if (!group) return std::nullopt;
    for (const auto& m : group->members) {
        if (m.userId == userId && !m.publicKey.empty()) return m.publicKey;
    }
    return std::nullopt;
}

// --- Starting, joining, ringing ---

std::string GroupCallSession::start(const std::string& conversationId, const std::string& kind) {
    if (busy()) return "You're already in a call. Leave it first.";
    const auto group = env_.group(conversationId);
    if (!group) return "Open the group chat and try again.";

    call_ = std::make_unique<Call>();
    call_->generation = ++generation_;
    call_->conversationId = conversationId;
    call_->kind = kind;
    view_ = View{};
    view_.present = true;
    view_.conversationId = conversationId;
    view_.groupLabel = group->label;
    view_.kind = kind;
    view_.phase = "connecting";
    view_.cameraOff = kind != "video";
    refreshTiles();
    changed();

    // A call already going on here is joined, not started again (and joining needs no plan).
    const auto ongoing = active_.find(conversationId);
    const std::string ongoingId = ongoing != active_.end() ? ongoing->second.callId : std::string();
    const auto generation = call_->generation;
    const auto alive = std::weak_ptr<bool>(alive_);
    auto api = env_.api;
    auto postFn = env_.post;
    background([=, this] {
        try {
            api::groupcalls::Join join = !ongoingId.empty() ? api::groupcalls::join(*api, ongoingId) : api::groupcalls::start(*api, conversationId, kind);
            postFn([=, this] {
                if (alive.lock()) onJoined(generation, join);
            });
        } catch (const std::exception& e) {
            const std::string message = errorMessage(e, "Couldn't start the call. Try again.");
            postFn([=, this] {
                if (alive.lock()) onFailed(generation, message);
            });
        }
    });
    return {};
}

std::string GroupCallSession::joinByLink(const std::string& callId) {
    // The call is ringing here already: pasting its link is the same as answering.
    if (call_ && call_->ringing && call_->callId == callId) {
        accept();
        return {};
    }
    if (busy()) return "You're already in a call. Leave it first.";

    call_ = std::make_unique<Call>();
    call_->generation = ++generation_;
    call_->callId = callId;
    view_ = View{};
    view_.present = true;
    view_.callId = callId;
    view_.phase = "connecting";
    refreshTiles();
    changed();

    const auto generation = call_->generation;
    const auto alive = std::weak_ptr<bool>(alive_);
    auto api = env_.api;
    auto postFn = env_.post;
    background([=, this] {
        try {
            // The server answers "This call has ended" for a call that was never started, one that's over,
            // and one in a group you're not in.
            api::groupcalls::Join join = api::groupcalls::join(*api, callId);
            postFn([=, this] {
                if (alive.lock()) onJoined(generation, join);
            });
        } catch (const std::exception& e) {
            const std::string message = errorMessage(e, "Couldn't join the call.");
            postFn([=, this] {
                if (alive.lock()) onFailed(generation, message);
            });
        }
    });
    return {};
}

void GroupCallSession::accept() {
    if (!call_ || !call_->ringing) return;
    call_->ringing = false;
    view_.phase = "connecting";
    changed();
    const auto generation = call_->generation;
    const std::string callId = call_->callId;
    const auto alive = std::weak_ptr<bool>(alive_);
    auto api = env_.api;
    auto postFn = env_.post;
    background([=, this] {
        try {
            api::groupcalls::Join join = api::groupcalls::join(*api, callId);
            postFn([=, this] {
                if (alive.lock()) onJoined(generation, join);
            });
        } catch (const std::exception& e) {
            const std::string message = errorMessage(e, "Couldn't join the call.");
            postFn([=, this] {
                if (alive.lock()) onFailed(generation, message);
            });
        }
    });
}

void GroupCallSession::decline() {
    if (!call_ || !call_->ringing) return;
    const std::string callId = call_->callId;
    auto api = env_.api;
    background([api, callId] {
        try {
            api::groupcalls::decline(*api, callId);
        } catch (const std::exception&) {
        }
    });
    finish("");
}

void GroupCallSession::onJoined(std::uint64_t generation, api::groupcalls::Join join) {
    if (generation != generation_ || !call_ || call_->closed) {
        // Left while the server was still admitting us.
        auto api = env_.api;
        const std::string callId = join.callId;
        background([api, callId] {
            try {
                api::groupcalls::leave(*api, callId);
            } catch (const std::exception&) {
            }
        });
        return;
    }
    call_->joinedServer = true;
    connect(join);
}

void GroupCallSession::onFailed(std::uint64_t generation, const std::string& message) {
    if (generation != generation_ || !call_ || call_->closed) return;
    finish(message);
}

void GroupCallSession::connect(const api::groupcalls::Join& join) {
    const auto group = env_.group(join.conversationId);
    if (!group) {
        finish("That call is in a group chat that isn't loaded yet. Open your chats and try the link again.");
        return;
    }
    call_->callId = join.callId;
    call_->conversationId = join.conversationId;
    call_->kind = join.kind;
    call_->announce = join.created;
    gcm::RoomKeyExchange::Config config;
    config.callId = join.callId;
    config.conversationId = join.conversationId;
    config.selfUserId = env_.self->userId;
    config.selfPrivateKey = env_.self->privateKey.view();
    config.publicKeyOf = [this](const std::string& userId) { return publicKeyOf(userId); };
    config.now = env_.now;
    call_->exchange = std::make_unique<gcm::RoomKeyExchange>(std::move(config));

    view_.callId = join.callId;
    view_.conversationId = join.conversationId;
    view_.groupLabel = group->label;
    view_.kind = join.kind;
    view_.cameraOff = join.kind != "video";
    view_.phase = "connecting";
    changed();

    const std::string url = api::groupcalls::signalingUrl(join.url, env_.siteBase);
    if (url.empty()) {
        finish("Couldn't reach the call server. Try again in a minute.");
        return;
    }
    const auto generation = call_->generation;
    const auto alive = std::weak_ptr<bool>(alive_);
    auto postFn = env_.post;
    auto factory = env_.roomFactory;
    auto callbacks = callbacksFor(generation);
    const std::string token = join.token;
    background([=, this] {
        try {
            std::shared_ptr<Room> room = factory(RoomConfig{url, token}, callbacks);
            postFn([=, this] {
                if (alive.lock()) onConnected(generation, room);
            });
        } catch (const std::exception&) {
            postFn([=, this] {
                if (alive.lock()) onFailed(generation, "Couldn't reach the call server. Try again in a minute.");
            });
        }
    });
}

RoomCallbacks GroupCallSession::callbacksFor(std::uint64_t generation) {
    const auto alive = std::weak_ptr<bool>(alive_);
    auto postFn = env_.post;
    RoomCallbacks cb;
    cb.onState = [=, this](RoomState state, const std::string& detail) {
        postFn([=, this] {
            if (alive.lock()) onRoomState(generation, state, detail);
        });
    };
    cb.onParticipantsChanged = [=, this] {
        postFn([=, this] {
            if (!alive.lock() || generation != generation_ || !call_) return;
            refreshTiles();
            changed();
        });
    };
    cb.onActiveSpeakers = [=, this](const std::vector<std::string>& identities) {
        postFn([=, this] {
            if (!alive.lock() || generation != generation_ || !call_) return;
            std::set<std::string> now(identities.begin(), identities.end());
            if (now == call_->speaking) return;
            call_->speaking = std::move(now);
            refreshTiles();
            changed();
        });
    };
    cb.onEncryption = [=, this](const std::string& identity, E2eeState state) {
        postFn([=, this] {
            if (alive.lock()) onEncryption(generation, identity, state);
        });
    };
    auto video = hooks_.video;
    cb.onVideo = [video](const std::string& identity, VideoSource source, const VideoFrame& frame) {
        if (video) video(identity + (source == VideoSource::Screen ? ":screen" : ":camera"), frame);
    };
    return cb;
}

void GroupCallSession::onConnected(std::uint64_t generation, std::shared_ptr<Room> room) {
    if (generation != generation_ || !call_ || call_->closed) {
        // Left while connecting: finish() already told the server.
        background([room] { room->disconnect(); });
        return;
    }
    call_->room = room;
    call_->localIdentity = room->localIdentity();
    shared_->setRoom(room);
    call_->ready = true;
    view_.phase = "active";
    view_.connectedAt = env_.now();

    // The link goes out only now that the call is really on: until the starter is connected there's nothing to join.
    if (call_->announce && env_.announce) {
        call_->announce = false;
        const std::string conversationId = call_->conversationId;
        const std::string text = gcm::inviteText(call_->kind, gcm::joinUrl(env_.siteBase, call_->callId));
        auto announce = env_.announce;
        auto postFn = env_.post;
        const auto alive = std::weak_ptr<bool>(alive_);
        background([=, this] {
            bool ok = false;
            try {
                ok = announce(conversationId, text);
            } catch (const std::exception&) {
            }
            if (!ok) {
                postFn([=, this] {
                    if (!alive.lock() || generation != generation_ || !call_) return;
                    view_.notice = "Couldn't post the call link in the chat. People can still join from the Join button.";
                    changed();
                });
            }
        });
    }

    auto buffered = std::move(call_->buffer);
    call_->buffer.clear();
    const std::string callId = call_->callId;
    for (const auto& event : buffered) {
        if (event.callId == callId && call_ && !call_->closed) handleCallEvent(event);
    }
    if (call_) refreshTiles();
    changed();
}

void GroupCallSession::onEncryption(std::uint64_t generation, const std::string& identity, E2eeState state) {
    if (generation != generation_ || !call_ || call_->closed) return;
    const bool self = !identity.empty() && identity == call_->localIdentity;
    switch (state) {
        case E2eeState::New:
        case E2eeState::Ok:
        case E2eeState::KeyRatcheted:
            call_->encryptionProblems.erase(identity);
            if (call_->encryptionProblems.empty() && (view_.notice == kNoticeUndecryptable || view_.notice == kNoticeNotSealing)) {
                view_.notice.clear();
                changed();
            }
            return;
        case E2eeState::EncryptionFailed:
        case E2eeState::InternalError:
            if (self) {
                finish(kEndedEncryptionFailed);
                return;
            }
            break;
        case E2eeState::DecryptionFailed:
        case E2eeState::MissingKey:
            break;
    }
    call_->encryptionProblems.insert(identity);
    view_.notice = self ? kNoticeNotSealing : kNoticeUndecryptable;
    changed();
}

void GroupCallSession::onRoomState(std::uint64_t generation, RoomState state, const std::string&) {
    if (generation != generation_ || !call_ || call_->closed) return;
    switch (state) {
        case RoomState::Reconnecting:
            view_.phase = "reconnecting";
            changed();
            break;
        case RoomState::Connected:
            if (call_->ready) {
                view_.phase = "active";
                changed();
            }
            break;
        case RoomState::Disconnected:
            finish("Call dropped");
            break;
    }
}

// --- Events from the live channel ---

void GroupCallSession::onLiveEvent(const api::live::LiveEvent& event) {
    if (event.type.rfind("group-", 0) != 0) return;
    if (event.type == "group-incoming") {
        if (!event.conversationId) return;
        active_[*event.conversationId] = ActiveCall{event.callId, event.kind.value_or("voice"), 1};
        if (hooks_.activeChanged) hooks_.activeChanged();
        const auto group = env_.group(*event.conversationId);
        if (!group || busy() || event.hostId == env_.self->userId) return;

        call_ = std::make_unique<Call>();
        call_->generation = ++generation_;
        call_->callId = event.callId;
        call_->conversationId = *event.conversationId;
        call_->kind = event.kind.value_or("voice");
        call_->ringing = true;
        view_ = View{};
        view_.present = true;
        view_.callId = event.callId;
        view_.conversationId = *event.conversationId;
        view_.groupLabel = group->label;
        view_.kind = call_->kind;
        view_.phase = "incoming";
        view_.cameraOff = call_->kind != "video";
        for (const auto& m : group->members) {
            if (event.hostId && m.userId == *event.hostId) view_.hostHandle = m.handle;
        }
        changed();
        return;
    }

    // Out of the group: leave now, however far the call has got. The server has already taken us off the roster.
    if (event.type == "group-removed") {
        if (!call_ || call_->callId != event.callId) return;
        const std::string conversationId = call_->conversationId;
        const bool ringing = call_->ringing;
        active_.erase(conversationId);
        if (hooks_.activeChanged) hooks_.activeChanged();
        finish(ringing ? "" : kEndedRemoved);
        return;
    }

    if (event.type == "group-ended") {
        const auto it = event.conversationId ? active_.find(*event.conversationId) : active_.end();
        if (it != active_.end() && it->second.callId == event.callId) {
            active_.erase(it);
            if (hooks_.activeChanged) hooks_.activeChanged();
        }
    }

    if (!call_) return;
    // Ringing and no answer in time: the call may still be on, the chat shows a Join button.
    if (event.type == "group-ring-stop") {
        if (call_->ringing && call_->callId == event.callId) finish("");
        return;
    }
    if (!call_->ready) {
        // Joining: hold on to this call's events until the room is connected.
        if (event.type == "group-roster" || event.type == "group-key") {
            if (call_->buffer.size() < kMaxBuffered) call_->buffer.push_back(event);
        } else if (event.type == "group-ended" && call_->callId == event.callId && !call_->ringing) {
            finish("Call ended");
        } else if (event.type == "group-ended" && call_->ringing && call_->callId == event.callId) {
            finish("");
        }
        return;
    }
    if (call_->callId != event.callId) return;
    if (event.type == "group-ended") {
        finish("Call ended");
        return;
    }
    handleCallEvent(event);
}

void GroupCallSession::handleCallEvent(const api::live::LiveEvent& event) {
    if (!call_ || !call_->exchange) return;
    gcm::RoomKeyExchange::Outcome outcome;
    if (event.type == "group-roster") {
        std::vector<gcm::RosterMember> members;
        for (const auto& m : event.members) members.push_back(gcm::RosterMember{m.userId, m.identity});
        outcome = call_->exchange->onRoster(event.epoch, event.keyholder.value_or(""), members);
        if (outcome.rosterChanged) {
            active_[call_->conversationId] = ActiveCall{call_->callId, call_->kind, static_cast<int>(members.size())};
            if (hooks_.activeChanged) hooks_.activeChanged();
        }
    } else if (event.type == "group-key") {
        if (!event.sealed) return;
        outcome = call_->exchange->onKey(event.epoch, event.from.value_or(""), crypto::calls::Sealed{event.sealed->ciphertext, event.sealed->nonce});
    } else {
        return;
    }
    apply(outcome);
}

void GroupCallSession::apply(const gcm::RoomKeyExchange::Outcome& outcome) {
    if (!call_) return;
    if (outcome.sas) view_.sas = *outcome.sas;
    view_.keyReady = call_->exchange->keyReady();
    if (outcome.clearNotice) view_.notice.clear();
    if (outcome.notice) view_.notice = *outcome.notice;

    const bool install = outcome.install.has_value();
    const bool sendKeys = !outcome.toSend.empty();
    const bool publishNow = install && !call_->publishStarted;
    if (publishNow) call_->publishStarted = true;

    if (install || sendKeys) {
        auto room = call_->room;
        auto api = env_.api;
        auto postFn = env_.post;
        const auto alive = std::weak_ptr<bool>(alive_);
        const auto generation = call_->generation;
        const std::string callId = call_->callId;
        const bool video = call_->kind == "video";
        std::vector<std::uint8_t> keyBytes;
        std::int64_t keyEpoch = 0;
        if (install) {
            keyBytes.assign(outcome.install->key.data(), outcome.install->key.data() + outcome.install->key.size());
            keyEpoch = outcome.install->epoch;
        }
        std::vector<api::groupcalls::SealedKey> toSend = outcome.toSend;
        const std::int64_t sendEpoch = outcome.sendEpoch;

        background([=, this, kb = std::move(keyBytes)]() mutable {
            // 1. The key goes in first, so what is sent from here on is sealed under it.
            if (install) {
                room->setKey(kb, keyEpoch);
                sodium_memzero(kb.data(), kb.size());
            }
            // 2. The keyholder hands everyone else their copy before doing anything slow.
            if (sendKeys) {
                try {
                    api::groupcalls::sendKeys(*api, callId, sendEpoch, toSend);
                } catch (const api::ApiError& e) {
                    // 409: someone joined or left meanwhile; the next roster brings a new epoch.
                    if (e.status() != 409) {
                        postFn([=, this] {
                            if (!alive.lock() || generation != generation_ || !call_) return;
                            view_.notice = "Couldn't hand out the call key. Others may not hear you.";
                            changed();
                        });
                    }
                } catch (const std::exception&) {
                }
            }
            // 3. Nothing is published until a key is held and frames are confirmed to be sealed.
            if (publishNow) {
                bool encryptionOk = false;
                try {
                    encryptionOk = room->frameEncryptionActive();
                } catch (const std::exception&) {
                }
                if (!encryptionOk) {
                    postFn([=, this] {
                        if (alive.lock()) onPublished(generation, false, false, false);
                    });
                    return;
                }
                bool microphoneOk = true;
                bool cameraOk = true;
                try {
                    room->publishMicrophone();
                } catch (const std::exception&) {
                    microphoneOk = false;
                }
                if (microphoneOk && video) {
                    try {
                        room->publishVideo(VideoSource::Camera, 640, 480);
                    } catch (const std::exception&) {
                        cameraOk = false;
                    }
                }
                postFn([=, this] {
                    if (alive.lock()) onPublished(generation, true, microphoneOk, cameraOk);
                });
            }
        });
    }
    refreshTiles();
    changed();
}

void GroupCallSession::onPublished(std::uint64_t generation, bool encryptionOk, bool microphoneOk, bool cameraOk) {
    if (generation != generation_ || !call_ || call_->closed) return;
    if (!encryptionOk) {
        finish(kEndedNoEncryption);
        return;
    }
    if (!microphoneOk) {
        finish("Couldn't send your audio to the call. Try joining again.");
        return;
    }
    view_.published = true;
    if (!cameraOk) {
        view_.cameraOff = true;
        view_.notice = "Couldn't use your camera.";
    }
    refreshTiles();
    changed();
}

// --- Controls ---

void GroupCallSession::toggleMute() {
    if (!call_ || !call_->room) return;
    view_.muted = !view_.muted;
    auto room = call_->room;
    const bool muted = view_.muted;
    background([room, muted] { room->setMicrophoneMuted(muted); });
    refreshTiles();
    changed();
}

void GroupCallSession::toggleCamera() {
    if (!call_ || !call_->room || call_->kind != "video" || !view_.published) return;
    view_.cameraOff = !view_.cameraOff;
    auto room = call_->room;
    const bool off = view_.cameraOff;
    background([room, off] { room->setVideoMuted(VideoSource::Camera, off); });
    refreshTiles();
    changed();
}

void GroupCallSession::cameraFailed() {
    if (!call_ || !call_->room || call_->kind != "video") return;
    view_.cameraOff = true;
    view_.notice = "Couldn't use your camera.";
    auto room = call_->room;
    background([room] { room->setVideoMuted(VideoSource::Camera, true); });
    refreshTiles();
    changed();
}

std::string GroupCallSession::toggleScreenShare() {
    if (!call_ || !call_->room || !view_.published || call_->screenBusy) return {};
    const std::string tier = env_.tier ? env_.tier() : std::string("free");
    if (tier != "premium") return "Screen sharing is part of Premium.";
    call_->screenBusy = true;
    const bool on = !view_.sharingScreen;
    auto room = call_->room;
    const auto generation = call_->generation;
    const auto alive = std::weak_ptr<bool>(alive_);
    auto postFn = env_.post;
    background([=, this] {
        bool ok = true;
        try {
            if (on) room->publishVideo(VideoSource::Screen, 1280, 720);
            else room->unpublishVideo(VideoSource::Screen);
        } catch (const std::exception&) {
            ok = false;
        }
        postFn([=, this] {
            if (alive.lock()) onScreen(generation, on, ok);
        });
    });
    return {};
}

void GroupCallSession::onScreen(std::uint64_t generation, bool on, bool ok) {
    if (generation != generation_ || !call_ || call_->closed) return;
    call_->screenBusy = false;
    if (ok) {
        view_.sharingScreen = on;
        if (on) view_.notice.clear();
    } else {
        // Cancelled, or the token doesn't allow it.
        view_.sharingScreen = false;
        if (on) view_.notice = "Couldn't share your screen.";
    }
    refreshTiles();
    changed();
}

void GroupCallSession::screenStopped() {
    if (!call_ || !view_.sharingScreen) return;
    toggleScreenShare();
}

void GroupCallSession::setMinimized(bool minimized) {
    if (!view_.present) return;
    view_.minimized = minimized;
    changed();
}

void GroupCallSession::dismiss() {
    if (call_ || !view_.present) return;
    view_ = View{};
    changed();
}

void GroupCallSession::leave() {
    if (!call_) return;
    finish(kEndedLeft);
}

void GroupCallSession::finish(const std::string& message) {
    if (!call_) return;
    call_->closed = true;
    ++generation_;
    auto room = call_->room;
    const bool joined = call_->joinedServer;
    const std::string callId = call_->callId;
    shared_->setRoom(nullptr);
    call_.reset();

    auto api = env_.api;
    background([room, joined, callId, api] {
        if (room) room->disconnect();
        if (joined && !callId.empty()) {
            try {
                api::groupcalls::leave(*api, callId);
            } catch (const std::exception&) {
            }
        }
    });

    if (message.empty()) {
        view_ = View{};
    } else {
        view_.phase = "ended";
        view_.endedMessage = message;
        view_.minimized = false;
        view_.tiles.clear();
        view_.muted = false;
        view_.sharingScreen = false;
        view_.published = false;
    }
    changed();
    refreshActive();
}

void GroupCallSession::refreshActive() {
    if (!*alive_ || !env_.api || !env_.self) return;
    const auto alive = std::weak_ptr<bool>(alive_);
    auto api = env_.api;
    auto postFn = env_.post;
    background([=, this] {
        try {
            auto calls = api::groupcalls::active(*api);
            postFn([=, this] {
                if (!alive.lock()) return;
                std::map<std::string, ActiveCall> next;
                for (const auto& c : calls) next[c.conversationId] = ActiveCall{c.callId, c.kind, c.participants};
                active_ = std::move(next);
                if (hooks_.activeChanged) hooks_.activeChanged();
            });
        } catch (const std::exception&) {
        }
    });
}

void GroupCallSession::clearActive() {
    active_.clear();
    if (hooks_.activeChanged) hooks_.activeChanged();
}

// --- Tiles ---

void GroupCallSession::refreshTiles() {
    view_.tiles.clear();
    if (!call_ || view_.phase == "incoming" || view_.phase == "ended") return;
    const auto group = env_.group(call_->conversationId);
    auto memberOf = [&](const std::string& userId) -> const Member* {
        if (!group) return nullptr;
        for (const auto& m : group->members) {
            if (m.userId == userId) return &m;
        }
        return nullptr;
    };

    const bool published = view_.published;
    Tile self;
    self.key = "self";
    self.userId = env_.self->userId;
    self.handle = "You";
    self.name = env_.self->handle;
    self.avatarUrl = env_.self->avatarUrl.value_or("");
    self.isSelf = true;
    self.videoOn = call_->kind == "video" && !view_.cameraOff && published;
    self.micMuted = view_.muted;
    self.speaking = !view_.muted && !call_->localIdentity.empty() && call_->speaking.count(call_->localIdentity) > 0;
    view_.tiles.push_back(self);
    if (view_.sharingScreen) {
        Tile screen = self;
        screen.key = "self:screen";
        screen.screen = true;
        screen.videoOn = true;
        screen.speaking = false;
        view_.tiles.push_back(screen);
    }

    if (!call_->room) return;
    std::vector<RemoteParticipant> remotes = call_->room->participants();
    // In the order people joined; anyone the roster doesn't know yet comes last.
    auto rank = [&](const RemoteParticipant& p) -> std::size_t {
        if (call_->exchange) {
            const auto& members = call_->exchange->members();
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (members[i].identity == p.identity) return i;
            }
        }
        return static_cast<std::size_t>(-1);
    };
    std::stable_sort(remotes.begin(), remotes.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
    for (const auto& p : remotes) {
        const auto userId = call_->exchange ? call_->exchange->userOfIdentity(p.identity) : std::nullopt;
        const Member* member = userId ? memberOf(*userId) : nullptr;
        Tile tile;
        tile.key = p.identity + ":camera";
        tile.userId = userId.value_or("");
        tile.handle = member ? member->handle : "Joining...";
        tile.name = member ? member->handle : "";
        tile.avatarUrl = member ? member->avatarUrl : "";
        tile.videoOn = p.cameraReceiving;
        tile.micMuted = !p.micPublished || p.micMuted;
        tile.speaking = call_->speaking.count(p.identity) > 0;
        view_.tiles.push_back(tile);
        if (p.screenPublished) {
            Tile screen = tile;
            screen.key = p.identity + ":screen";
            screen.screen = true;
            screen.videoOn = p.screenReceiving;
            screen.speaking = false;
            view_.tiles.push_back(screen);
        }
    }
}

// --- Media (any thread) ---

void GroupCallSession::sendAudio(const std::int16_t* pcm) {
    if (const auto room = mediaRoom()) room->sendAudio(pcm);
}

void GroupCallSession::readAudio(std::int16_t* out, std::size_t samples) {
    if (const auto room = mediaRoom()) {
        room->readAudio(out, samples);
    } else {
        std::fill(out, out + samples, std::int16_t{0});
    }
}

void GroupCallSession::sendVideo(VideoSource source, const VideoFrame& frame) {
    if (const auto room = mediaRoom()) room->sendVideo(source, frame);
}

}  // namespace redvault::lk
