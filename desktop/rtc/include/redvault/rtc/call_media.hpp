// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// The desktop app's call media engine: WebRTC-compatible audio (Opus) and
// video (VP8) over DTLS-SRTP, built on libdatachannel, talking to browsers
// running the web app's call code (ui/src/state/CallContext.tsx).
//
//   - ICE goes through TURN relays only (relayOnly), like the web app, so
//     neither person learns the other's IP address. libdatachannel's Relay
//     policy alone doesn't do that: see relay_sdp.hpp for what does.
//   - Gathering completes before the SDP is handed out: offers and answers
//     carry every candidate and are sealed end to end by the caller of this
//     class (redvault/crypto/call_crypto.hpp); nothing trickles through the
//     server.
//   - Audio is always a 48 kHz mono stream in 20 ms frames; video is I420.
//     Capture and playback belong to the app (Qt Multimedia).
//
// Qt-free and thread-safe: libdatachannel calls back on its own threads, so
// the callbacks here must hand work off to the UI thread themselves.

namespace redvault::rtc {

class RtcError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One TURN (or STUN) URL, e.g. "turn:turn.example.com:3478?transport=udp".
struct IceServer {
    std::string url;
    std::string username;
    std::string credential;
};

struct Config {
    std::vector<IceServer> servers;
    /// Relay candidates only (the app always sets this; tests on one machine turn it off).
    bool relayOnly = true;
    /// How long to wait for ICE gathering before giving up.
    int gatherTimeoutMs = 8000;
};

enum class ConnectionState { New, Connecting, Connected, Disconnected, Failed, Closed };

/// One picture in I420 (planar Y, U, V; U and V at half resolution). Width and height are even.
struct VideoFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> data;  // width*height*3/2 bytes: Y plane, then U, then V

    static VideoFrame blank(int width, int height);
    std::uint8_t* y() { return data.data(); }
    std::uint8_t* u() { return data.data() + static_cast<std::size_t>(width) * height; }
    std::uint8_t* v() { return data.data() + static_cast<std::size_t>(width) * height * 5 / 4; }
    const std::uint8_t* y() const { return data.data(); }
    const std::uint8_t* u() const { return data.data() + static_cast<std::size_t>(width) * height; }
    const std::uint8_t* v() const { return data.data() + static_cast<std::size_t>(width) * height * 5 / 4; }
};

struct Callbacks {
    std::function<void(ConnectionState)> onState;
    /// A decoded remote picture (from libdatachannel's thread).
    std::function<void(const VideoFrame&)> onVideoFrame;
};

/// Audio frames are always this many samples: 20 ms at 48 kHz, mono.
inline constexpr int kSampleRate = 48000;
inline constexpr int kFrameSamples = 960;

class CallMedia {
public:
    CallMedia(Config config, Callbacks callbacks);
    ~CallMedia();
    CallMedia(const CallMedia&) = delete;
    CallMedia& operator=(const CallMedia&) = delete;

    /// Caller: an offer with one audio and one video section (both send and receive), once gathering is done.
    /// Throws RtcError if no relay candidate could be gathered (relayOnly).
    std::string createOffer();
    /// Callee: answers a browser's (or another desktop's) offer, once gathering is done.
    std::string acceptOffer(const std::string& offerSdp);
    /// Caller: the answer came back.
    void setAnswer(const std::string& answerSdp);
    /// A candidate that arrived on its own, for a peer that sends them separately. In a relay-only call one that
    /// isn't a relay candidate is dropped (false), and a malformed one throws RtcError. Nothing else adds candidates.
    bool addRemoteCandidate(const std::string& candidate, const std::string& mid);
    /// Whether the pair in use leaves through a TURN allocation (for diagnostics and tests).
    bool connectedThroughRelay() const;

    std::string localSdp() const;
    std::string remoteSdp() const;
    ConnectionState state() const;

    /// One 20 ms frame (kFrameSamples samples) of microphone audio; encoded and sent. Muted: send silence or nothing.
    void sendAudio(const std::int16_t* pcm);
    /// Fills `samples` of playback audio from the jitter buffer (silence when there's nothing).
    void readAudio(std::int16_t* out, std::size_t samples);
    /// Encodes and sends one picture (camera or screen). An empty frame stops nothing; just don't call it to send no video.
    void sendVideo(const VideoFrame& frame);
    void setVideoBitrate(int kbps);
    /// Received audio packets so far (for diagnostics and tests).
    std::uint64_t audioPacketsReceived() const;

    void close();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace redvault::rtc
