// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/rtc/call_media.hpp"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <random>

#include "codecs.hpp"
#include "redvault/rtc/relay_sdp.hpp"
#include "rtp.hpp"

namespace redvault::rtc {

namespace {

using Clock = std::chrono::steady_clock;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::uint32_t randomU32() {
    static std::mt19937 rng{std::random_device{}()};
    static std::mutex m;
    std::lock_guard lock(m);
    return rng();
}

/**
 * "turn:host:3478?transport=udp" -> libdatachannel's IceServer. libjuice
 * relays over UDP only, so TCP/TLS relay URLs are skipped (the web app
 * uses those; the server always offers a UDP one too).
 */
std::optional<::rtc::IceServer> toIceServer(const IceServer& s) {
    const std::string url = s.url;
    const auto colon = url.find(':');
    if (colon == std::string::npos) return std::nullopt;
    const std::string scheme = lower(url.substr(0, colon));
    std::string rest = url.substr(colon + 1);
    std::string transport = "udp";
    if (const auto q = rest.find('?'); q != std::string::npos) {
        const std::string query = lower(rest.substr(q + 1));
        rest = rest.substr(0, q);
        if (const auto t = query.find("transport="); t != std::string::npos) transport = query.substr(t + 10, query.find('&', t) - (t + 10));
    }
    std::string host = rest;
    std::uint16_t port = scheme == "turns" ? 5349 : 3478;
    if (const auto p = rest.rfind(':'); p != std::string::npos && rest.find(']') == std::string::npos) {
        host = rest.substr(0, p);
        port = static_cast<std::uint16_t>(std::stoi(rest.substr(p + 1)));
    }
    if (host.empty()) return std::nullopt;
    if (scheme == "stun") return ::rtc::IceServer(host, port);
    if (scheme == "turn" && transport == "udp") {
        return ::rtc::IceServer(host, port, s.username, s.credential, ::rtc::IceServer::RelayType::TurnUdp);
    }
    return std::nullopt;
}

ConnectionState mapState(::rtc::PeerConnection::State s) {
    using S = ::rtc::PeerConnection::State;
    switch (s) {
        case S::New: return ConnectionState::New;
        case S::Connecting: return ConnectionState::Connecting;
        case S::Connected: return ConnectionState::Connected;
        case S::Disconnected: return ConnectionState::Disconnected;
        case S::Failed: return ConnectionState::Failed;
        case S::Closed: return ConnectionState::Closed;
    }
    return ConnectionState::Failed;
}

bool pairIsRelay(::rtc::PeerConnection& connection) {
    ::rtc::Candidate local, remote;
    return connection.getSelectedCandidatePair(&local, &remote) && local.type() == ::rtc::Candidate::Type::Relayed;
}

struct Offered {
    std::string mid;
    int payloadType = -1;
};

/// The Opus and VP8 sections of a remote offer (payload types are the offerer's to choose).
void findCodecs(const ::rtc::Description& d, Offered& audio, Offered& video) {
    for (int i = 0; i < d.mediaCount(); ++i) {
        const auto entry = d.media(i);
        const auto* const* media = std::get_if<const ::rtc::Description::Media*>(&entry);
        if (!media || !*media) continue;
        const auto& m = **media;
        const std::string type = lower(m.type());
        for (const int pt : m.payloadTypes()) {
            const auto* map = m.rtpMap(pt);
            if (!map) continue;
            const std::string format = lower(map->format);
            if (type == "audio" && audio.payloadType < 0 && format == "opus") audio = {m.mid(), pt};
            if (type == "video" && video.payloadType < 0 && format == "vp8") video = {m.mid(), pt};
        }
    }
}

}  // namespace

struct CallMedia::Impl : std::enable_shared_from_this<CallMedia::Impl> {
    Config config;
    Callbacks callbacks;
    std::shared_ptr<::rtc::PeerConnection> pc;
    std::shared_ptr<::rtc::Track> audio;
    std::shared_ptr<::rtc::Track> video;
    std::shared_ptr<::rtc::RtpPacketizationConfig> audioConfig;
    std::shared_ptr<::rtc::RtpPacketizationConfig> videoConfig;
    int audioPt = 111;
    int videoPt = 96;

    OpusEnc opusEncoder;
    AudioJitterBuffer jitter;
    std::uint32_t audioClock = 0;

    std::mutex videoSendMutex;
    Vp8Enc vp8Encoder;
    std::atomic<bool> keyframeWanted{true};
    Clock::time_point videoStart = Clock::now();

    std::mutex videoRecvMutex;
    Vp8Assembler assembler;
    Vp8Dec vp8Decoder;
    bool haveKeyframe = false;
    Clock::time_point lastKeyframeRequest{};

    mutable std::mutex stateMutex;
    std::condition_variable gatheredCv;
    bool gathered = false;
    ConnectionState state = ConnectionState::New;

    void start() {
        ::rtc::Configuration c;
        for (const auto& s : config.servers) {
            if (auto server = toIceServer(s)) c.iceServers.push_back(*server);
        }
        c.iceTransportPolicy = config.relayOnly ? ::rtc::TransportPolicy::Relay : ::rtc::TransportPolicy::All;
        c.disableAutoNegotiation = true;
        pc = std::make_shared<::rtc::PeerConnection>(c);

        std::weak_ptr<Impl> weak = shared_from_this();
        std::weak_ptr<::rtc::PeerConnection> connection = pc;
        pc->onStateChange([weak, connection](::rtc::PeerConnection::State s) {
            auto self = weak.lock();
            if (!self) return;
            ConnectionState mapped = mapState(s);
            // The last line of defence: a relay-only call that somehow got a direct path doesn't stay up.
            if (mapped == ConnectionState::Connected && self->config.relayOnly) {
                if (const auto live = connection.lock(); live && !pairIsRelay(*live)) {
                    mapped = ConnectionState::Failed;
                    try {
                        live->close();
                    } catch (const std::exception&) {
                    }
                }
            }
            {
                std::lock_guard lock(self->stateMutex);
                self->state = mapped;
            }
            if (self->callbacks.onState) self->callbacks.onState(mapped);
        });
        pc->onGatheringStateChange([weak](::rtc::PeerConnection::GatheringState s) {
            auto self = weak.lock();
            if (!self || s != ::rtc::PeerConnection::GatheringState::Complete) return;
            {
                std::lock_guard lock(self->stateMutex);
                self->gathered = true;
            }
            self->gatheredCv.notify_all();
        });
    }

    void addTracks(const std::string& audioMid, int aPt, const std::optional<Offered>& videoOffer) {
        const std::string cname = "rv" + std::to_string(randomU32());
        const std::string msid = "rv-stream";

        audioPt = aPt;
        const std::uint32_t audioSsrc = randomU32();
        ::rtc::Description::Audio a(audioMid, ::rtc::Description::Direction::SendRecv);
        a.addOpusCodec(aPt);
        a.addSSRC(audioSsrc, cname, msid, "rv-audio");
        audio = pc->addTrack(a);
        audioConfig = std::make_shared<::rtc::RtpPacketizationConfig>(audioSsrc, cname, static_cast<std::uint8_t>(aPt),
                                                                       ::rtc::OpusRtpPacketizer::DefaultClockRate);
        auto audioPacketizer = std::make_shared<::rtc::OpusRtpPacketizer>(audioConfig);
        audioPacketizer->addToChain(std::make_shared<::rtc::RtcpSrReporter>(audioConfig));
        audioPacketizer->addToChain(std::make_shared<::rtc::RtcpReceivingSession>());
        audio->setMediaHandler(audioPacketizer);
        std::weak_ptr<Impl> weak = shared_from_this();
        audio->onMessage(
            [weak](::rtc::binary message) {
                auto self = weak.lock();
                if (!self) return;
                const auto rtp = parseRtp(reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
                if (rtp && rtp->payloadType == self->audioPt) self->jitter.push(rtp->sequence, rtp->payload, rtp->payloadSize);
            },
            nullptr);

        if (!videoOffer) return;
        videoPt = videoOffer->payloadType;
        const std::uint32_t videoSsrc = randomU32();
        ::rtc::Description::Video v(videoOffer->mid, ::rtc::Description::Direction::SendRecv);
        v.addVP8Codec(videoPt);
        v.addSSRC(videoSsrc, cname, msid, "rv-video");
        video = pc->addTrack(v);
        videoConfig = std::make_shared<::rtc::RtpPacketizationConfig>(videoSsrc, cname, static_cast<std::uint8_t>(videoPt),
                                                                       ::rtc::RtpPacketizer::VideoClockRate);
        auto videoPacketizer = std::make_shared<Vp8RtpPacketizer>(videoConfig);
        videoPacketizer->addToChain(std::make_shared<::rtc::RtcpSrReporter>(videoConfig));
        videoPacketizer->addToChain(std::make_shared<::rtc::RtcpNackResponder>());
        videoPacketizer->addToChain(std::make_shared<::rtc::PliHandler>([weak] {
            if (auto self = weak.lock()) self->keyframeWanted = true;
        }));
        videoPacketizer->addToChain(std::make_shared<::rtc::RtcpReceivingSession>());
        video->setMediaHandler(videoPacketizer);
        video->onMessage(
            [weak](::rtc::binary message) {
                if (auto self = weak.lock()) self->receiveVideo(message);
            },
            nullptr);
    }

    void receiveVideo(const ::rtc::binary& message) {
        const auto rtp = parseRtp(reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
        if (!rtp || rtp->payloadType != videoPt) return;
        std::optional<VideoFrame> decoded;
        bool askKeyframe = false;
        {
            std::lock_guard lock(videoRecvMutex);
            auto result = assembler.push(*rtp);
            if (result.lost) askKeyframe = true;
            if (result.frame) {
                if (result.keyframe) haveKeyframe = true;
                if (haveKeyframe) {
                    decoded = vp8Decoder.decode(*result.frame);
                    if (!decoded) askKeyframe = true;
                } else {
                    askKeyframe = true;
                }
            }
            if (askKeyframe) {
                const auto now = Clock::now();
                if (now - lastKeyframeRequest < std::chrono::milliseconds(500)) askKeyframe = false;
                else lastKeyframeRequest = now;
            }
        }
        if (askKeyframe && video) video->requestKeyframe();
        if (decoded && callbacks.onVideoFrame) callbacks.onVideoFrame(*decoded);
    }

    /// The only way a remote description gets in, so offers, answers and anything later pass the same filter.
    /// A relay-only call refuses a description with no relay left: there'd be nothing to connect to.
    ::rtc::Description remoteDescription(const std::string& sdp, ::rtc::Description::Type type) const {
        if (!config.relayOnly) return ::rtc::Description(sdp, type);
        const RelayFilterResult filtered = keepRelayCandidates(sdp);
        if (filtered.kept == 0) throw RtcError("relay_unreachable");
        std::optional<::rtc::Description> description;
        try {
            description.emplace(filtered.sdp, type);
        } catch (const std::exception&) {
            throw RtcError("bad_candidate");
        }
        // Read twice, by two parsers: libdatachannel's view has to agree with the filter's.
        for (const auto& candidate : description->candidates()) {
            if (candidate.type() != ::rtc::Candidate::Type::Relayed) throw RtcError("bad_candidate");
        }
        return *description;
    }

    std::string gatherAndDescribe() {
        std::unique_lock lock(stateMutex);
        gatheredCv.wait_for(lock, std::chrono::milliseconds(config.gatherTimeoutMs), [this] { return gathered; });
        lock.unlock();
        const auto description = pc->localDescription();
        if (!description) throw RtcError("Couldn't prepare the call");
        const std::string sdp = std::string(*description);
        if (config.relayOnly) {
            // Never hand out an offer or answer that names this computer's own address.
            const RelayFilterResult own = keepRelayCandidates(sdp);
            if (own.kept == 0 || own.removed > 0) throw RtcError("relay_unreachable");
        }
        return sdp;
    }
};

CallMedia::CallMedia(Config config, Callbacks callbacks) : impl_(std::make_shared<Impl>()) {
    impl_->config = std::move(config);
    impl_->callbacks = std::move(callbacks);
    impl_->start();
}

CallMedia::~CallMedia() {
    close();
}

std::string CallMedia::createOffer() {
    impl_->addTracks("0", 111, Offered{"1", 96});
    impl_->pc->setLocalDescription(::rtc::Description::Type::Offer);
    return impl_->gatherAndDescribe();
}

std::string CallMedia::acceptOffer(const std::string& offerSdp) {
    ::rtc::Description offer = impl_->remoteDescription(offerSdp, ::rtc::Description::Type::Offer);
    Offered audio, video;
    findCodecs(offer, audio, video);
    if (audio.payloadType < 0) throw RtcError("The call doesn't offer Opus audio");
    impl_->addTracks(audio.mid, audio.payloadType, video.payloadType >= 0 ? std::optional<Offered>(video) : std::nullopt);
    impl_->pc->setRemoteDescription(offer);
    impl_->pc->setLocalDescription(::rtc::Description::Type::Answer);
    return impl_->gatherAndDescribe();
}

void CallMedia::setAnswer(const std::string& answerSdp) {
    impl_->pc->setRemoteDescription(impl_->remoteDescription(answerSdp, ::rtc::Description::Type::Answer));
}

bool CallMedia::addRemoteCandidate(const std::string& candidate, const std::string& mid) {
    if (impl_->config.relayOnly && !isRelayCandidate(candidate)) return false;
    ::rtc::Candidate parsed(candidate, mid);
    if (impl_->config.relayOnly && parsed.type() != ::rtc::Candidate::Type::Relayed) return false;
    impl_->pc->addRemoteCandidate(parsed);
    return true;
}

bool CallMedia::connectedThroughRelay() const {
    return impl_->pc && pairIsRelay(*impl_->pc);
}

std::string CallMedia::localSdp() const {
    const auto d = impl_->pc ? impl_->pc->localDescription() : std::nullopt;
    return d ? std::string(*d) : std::string();
}

std::string CallMedia::remoteSdp() const {
    const auto d = impl_->pc ? impl_->pc->remoteDescription() : std::nullopt;
    return d ? std::string(*d) : std::string();
}

ConnectionState CallMedia::state() const {
    std::lock_guard lock(impl_->stateMutex);
    return impl_->state;
}

void CallMedia::sendAudio(const std::int16_t* pcm) {
    auto& i = *impl_;
    if (!i.audio || !i.audio->isOpen()) return;
    auto encoded = i.opusEncoder.encode(pcm);
    i.audioClock += kFrameSamples;
    ::rtc::binary frame(reinterpret_cast<const std::byte*>(encoded.data()), reinterpret_cast<const std::byte*>(encoded.data() + encoded.size()));
    try {
        i.audio->sendFrame(std::move(frame), ::rtc::FrameInfo(i.audioConfig->startTimestamp + i.audioClock));
    } catch (const std::exception&) {
        // The track closed under us: the call is ending.
    }
}

void CallMedia::readAudio(std::int16_t* out, std::size_t samples) {
    impl_->jitter.read(out, samples);
}

void CallMedia::sendVideo(const VideoFrame& frame) {
    auto& i = *impl_;
    if (!i.video || !i.video->isOpen()) return;
    std::lock_guard lock(i.videoSendMutex);
    bool keyframe = false;
    auto encoded = i.vp8Encoder.encode(frame, i.keyframeWanted.exchange(false), keyframe);
    if (encoded.empty()) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - i.videoStart).count();
    const auto timestamp = static_cast<std::uint32_t>(i.videoConfig->startTimestamp + static_cast<std::uint32_t>(elapsed * 90));
    ::rtc::binary data(reinterpret_cast<const std::byte*>(encoded.data()), reinterpret_cast<const std::byte*>(encoded.data() + encoded.size()));
    try {
        i.video->sendFrame(std::move(data), ::rtc::FrameInfo(timestamp));
    } catch (const std::exception&) {
    }
}

void CallMedia::setVideoBitrate(int kbps) {
    std::lock_guard lock(impl_->videoSendMutex);
    impl_->vp8Encoder.setBitrate(kbps);
}

std::uint64_t CallMedia::audioPacketsReceived() const {
    return impl_->jitter.received();
}

void CallMedia::close() {
    if (!impl_ || !impl_->pc) return;
    auto pc = std::move(impl_->pc);
    pc->resetCallbacks();
    // Close the tracks here, before the connection closes them on its own worker thread: libdatachannel resets a
    // track's callbacks without a lock, so a worker close racing the release below was a data race (TSan). A track
    // closes once, so the worker then finds it already closed and leaves it alone.
    for (auto* track : {&impl_->audio, &impl_->video}) {
        if (!*track) continue;
        try {
            (*track)->close();
        } catch (const std::exception&) {
        }
    }
    try {
        pc->close();
    } catch (const std::exception&) {
    }
    impl_->audio.reset();
    impl_->video.reset();
}

}  // namespace redvault::rtc
